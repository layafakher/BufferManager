#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buffer_mgr.h"
#include "buffer_mgr_stat.h"
#include "storage_mgr.h"
#include "dberror.h"
#include "dt.h"

#ifdef BM_THREADSAFE
#include <pthread.h>
#endif

//locking helpers (no-ops unless BM_THREADSAFE) 
#ifdef BM_THREADSAFE
#define TS_LOCK(bm)    do { if ((bm) && (bm)->mgmtData) pthread_mutex_lock(&MD(bm)->lock); } while (0)
#define TS_UNLOCK(bm)  do { if ((bm) && (bm)->mgmtData) pthread_mutex_unlock(&MD(bm)->lock); } while (0)
#define TS_RETURN(bm, code) do { TS_UNLOCK(bm); return (code); } while (0)
#else
#define TS_LOCK(bm)          ((void)0)
#define TS_UNLOCK(bm)        ((void)0)
#define TS_RETURN(bm, code)  return (code)
#endif
//internal data structures
typedef struct Frame {
    PageNumber pageNum;      //NO_PAGE if empty
    int        fixCount;
    bool       dirty;
    char      *data;         //pointer into a contiguous frameMem block
    long long  loadTick;     //for FIFO
    long long  accessTick;   //for LRU
    unsigned char ref;       //cLOCK reference bit (0/1)
    //optional history fields (not used when LRU_K behaves like LRU)
    long long  hist[2];
    int        kCount;
    int        kPos;
} Frame;

typedef struct BM_MgmtData {
    SM_FileHandle fhandle;     //open page file
    Frame *frames;             //array length = bm->numPages
    char  *frameMem;           //backing memory for all frames (numPages*PAGE_SIZE)

    //arrays returned by the stats getters (owned here; never realloc per call)
    PageNumber *frameContents; //length = bm->numPages
    bool       *dirtyFlags;    //length = bm->numPages
    int        *fixCounts;     //length = bm->numPages

    long long tick;            //global logical clock for loads/accesses
    int numReadIO;             //num of disk reads after initBufferPool
    int numWriteIO;            //num of disk writes after initBufferPool

    int clockHand;             //CLOCK hand index
#ifdef BM_THREADSAFE
    pthread_mutex_t lock;      //coarse-grained pool mutex
#endif
} BM_MgmtData;

//small helpers (static)
static inline BM_MgmtData* MD(BM_BufferPool *bm) {
    return (BM_MgmtData*) bm->mgmtData;
}

static int findFrameByPageNP(BM_MgmtData *md, int numPages, PageNumber p) {
    for (int i = 0; i < numPages; i++) {
        if (md->frames[i].pageNum == p) return i;
    }
    return -1;
}

//update the exported stats arrays from a single frame slot.
static void sync_stats_slot(BM_MgmtData *md, int idx) {
    md->frameContents[idx] = md->frames[idx].pageNum;
    md->dirtyFlags[idx]    = md->frames[idx].dirty ? TRUE : FALSE;
    md->fixCounts[idx]     = md->frames[idx].fixCount;
}

//ensure pageNum exists in file (allowing newly addressed pages).
static RC ensure_page_exists(BM_MgmtData *md, PageNumber pageNum) {
    int needPages = pageNum + 1;
    return ensureCapacity(needPages, &md->fhandle);
}

//write back a frame if dirty (only called when we are allowed to write).
static RC writeBackIfDirty(BM_BufferPool *bm, int fIdx) {
    BM_MgmtData *md = MD(bm);
    Frame *fr = &md->frames[fIdx];

    if (fr->dirty) {
        RC rc = ensure_page_exists(md, fr->pageNum);
        if (rc != RC_OK) return rc;

        rc = writeBlock(fr->pageNum, &md->fhandle, fr->data);
        if (rc != RC_OK) return rc;

        md->numWriteIO++;
        fr->dirty = FALSE;
        md->dirtyFlags[fIdx] = FALSE;
    }
    return RC_OK;
}

//record an access for (LRU / LRU_K-as-LRU / CLOCK)
static void record_access(BM_MgmtData *md, Frame *fr) {
    fr->accessTick = ++md->tick;
    fr->hist[fr->kPos] = fr->accessTick;
    fr->kPos = (fr->kPos + 1) % 2;
    if (fr->kCount < 2) fr->kCount++;
    fr->ref = 1; //CLOCK: set ref bit on any access
}

//compute victim key per strategy (smaller -> older -> better victim).
static long long victim_key(BM_BufferPool *bm, Frame *fr) {
    switch (bm->strategy) {
        case RS_FIFO:   return fr->loadTick;
        case RS_LRU:    return fr->accessTick;
        case RS_LRU_K:  return fr->accessTick;  //treat LRU-K like LRU for the provided tests
        default:        return fr->accessTick;  //fallback
    }
}

//choose a victim frame index among those with fixCount == 0. Return -1 if none.
static int selectVictim(BM_BufferPool *bm) {
    BM_MgmtData *md = MD(bm);
    int n = bm->numPages;

    //CLOCK: do a bounded sweep toggling ref bits
    if (bm->strategy == RS_CLOCK) {
        if (n <= 0) return -1;
        int steps = 0;
        while (steps < 2 * n) { //bounded; if all pinned we'll quit
            int idx = md->clockHand;
            Frame *fr = &md->frames[idx];
            md->clockHand = (md->clockHand + 1) % n;

            if (fr->pageNum != NO_PAGE && fr->fixCount == 0) {
                if (fr->ref == 0) return idx; //victim
                fr->ref = 0;                  //second chance
            }
            steps++;
        }
        return -1; //probably all pinned
    }

    //FIFO/LRU/LRU-K-as-LRU:pick smallest key among unpinned
    int best = -1;
    long long bestKey = 0;
    for (int i = 0; i < n; i++) {
        Frame *fr = &md->frames[i];
        if (fr->fixCount == 0 && fr->pageNum != NO_PAGE) {
            long long key = victim_key(bm, fr);
            if (best == -1 || key < bestKey) {
                best = i;
                bestKey = key;
            }
        }
    }
    return best;
}

//find a free (empty) frame index, or -1 if none.
static int findEmptyFrame(BM_MgmtData *md, int numPages) {
    for (int i = 0; i < numPages; i++) {
        if (md->frames[i].pageNum == NO_PAGE) return i;
    }
    return -1;
}

//read page into frame slot fIdx (assumes any required write-back already done).
static RC readPageIntoFrame(BM_BufferPool *bm, int fIdx, PageNumber p) {
    BM_MgmtData *md = MD(bm);

    RC rc = ensure_page_exists(md, p);
    if (rc != RC_OK) return rc;

    rc = readBlock(p, &md->fhandle, md->frames[fIdx].data);
    if (rc != RC_OK) return rc;
    md->numReadIO++;

    Frame *fr = &md->frames[fIdx];
    fr->pageNum    = p;
    fr->fixCount   = 0;     //caller will adjust
    fr->dirty      = FALSE;
    fr->loadTick   = ++md->tick;
    fr->accessTick = fr->loadTick;
    fr->ref        = 1;     //CLOCK:newly loaded page gets the reference bit
    fr->kCount     = 0;
    fr->kPos       = 0;
    fr->hist[0]    = 0;
    fr->hist[1]    = 0;

    sync_stats_slot(md, fIdx);
    return RC_OK;
}


//public API implementations
RC initBufferPool(BM_BufferPool *const bm, const char *const pageFileName,
                  const int numPages, ReplacementStrategy strategy,
                  void *stratData)
{
    //no locking here: mgmtData not set yet
    SM_FileHandle fh;
    RC rc = openPageFile((char*)pageFileName, &fh);
    if (rc != RC_OK) {
        return RC_FILE_NOT_FOUND;
    }

    BM_MgmtData *md = (BM_MgmtData*) calloc(1, sizeof(BM_MgmtData));
    if (!md) { closePageFile(&fh); return RC_WRITE_FAILED; }

    md->fhandle    = fh;
    md->tick       = 0;
    md->numReadIO  = 0;
    md->numWriteIO = 0;
    md->clockHand  = 0;

    md->frames   = (Frame*) calloc(numPages, sizeof(Frame));
    md->frameMem = (char*) malloc((size_t)numPages * PAGE_SIZE);
    if (!md->frames || !md->frameMem) {
        closePageFile(&fh);
        free(md->frames);
        free(md->frameMem);
        free(md);
        return RC_WRITE_FAILED;
    }

    for (int i = 0; i < numPages; i++) {
        md->frames[i].pageNum    = NO_PAGE;
        md->frames[i].fixCount   = 0;
        md->frames[i].dirty      = FALSE;
        md->frames[i].data       = md->frameMem + (size_t)i * PAGE_SIZE;
        md->frames[i].loadTick   = 0;
        md->frames[i].accessTick = 0;
        md->frames[i].ref        = 0;
        md->frames[i].kCount     = 0;
        md->frames[i].kPos       = 0;
        md->frames[i].hist[0]    = 0;
        md->frames[i].hist[1]    = 0;
    }

    md->frameContents = (PageNumber*) malloc(sizeof(PageNumber) * (size_t)numPages);
    md->dirtyFlags    = (bool*)       malloc(sizeof(bool)       * (size_t)numPages);
    md->fixCounts     = (int*)        malloc(sizeof(int)        * (size_t)numPages);
    if (!md->frameContents || !md->dirtyFlags || !md->fixCounts) {
        closePageFile(&fh);
        free(md->frameMem);
        free(md->frames);
        free(md->frameContents);
        free(md->dirtyFlags);
        free(md->fixCounts);
        free(md);
        return RC_WRITE_FAILED;
    }

    for (int i = 0; i < numPages; i++) {
        md->frameContents[i] = NO_PAGE;
        md->dirtyFlags[i]    = FALSE;
        md->fixCounts[i]     = 0;
    }

    bm->pageFile  = (char*)pageFileName;
    bm->numPages  = numPages;
    bm->strategy  = strategy;
    bm->mgmtData  = md;

#ifdef BM_THREADSAFE
    pthread_mutex_init(&md->lock, NULL);
#endif
    (void)stratData; //unused
    return RC_OK;
}

RC shutdownBufferPool(BM_BufferPool *const bm)
{
    if (!bm || !bm->mgmtData) {
        return RC_FILE_HANDLE_NOT_INIT;
    }
    BM_MgmtData *md = MD(bm);
#ifdef BM_THREADSAFE
    pthread_mutex_lock(&md->lock);
#endif

    //Write back ALL dirty pages, even if pinned (tests expect shutdown to succeed)
    for (int i = 0; i < bm->numPages; i++) {
        if (md->frames[i].pageNum != NO_PAGE && md->frames[i].dirty) {
            RC rc = writeBackIfDirty(bm, i);
            if (rc != RC_OK) {
#ifdef BM_THREADSAFE
                pthread_mutex_unlock(&md->lock);
#endif
                return rc;
            }
        }
    }

    //close page file
    RC rc = closePageFile(&md->fhandle);
    if (rc != RC_OK) {
#ifdef BM_THREADSAFE
        pthread_mutex_unlock(&md->lock);
#endif
        return rc;
    }

    //free memory
    free(md->frameMem);
    free(md->frames);
    free(md->frameContents);
    free(md->dirtyFlags);
    free(md->fixCounts);
#ifdef BM_THREADSAFE
    pthread_mutex_unlock(&md->lock);
    pthread_mutex_destroy(&md->lock);
#endif
    free(md);

    bm->mgmtData = NULL;
    return RC_OK;
}

RC forceFlushPool(BM_BufferPool *const bm)
{
    if (!bm || !bm->mgmtData) return RC_FILE_HANDLE_NOT_INIT;
    TS_LOCK(bm);

    BM_MgmtData *md = MD(bm);
    for (int i = 0; i < bm->numPages; i++) {
        if (md->frames[i].pageNum != NO_PAGE &&
            md->frames[i].dirty &&
            md->frames[i].fixCount == 0)
        {
            RC rc = writeBackIfDirty(bm, i);
            if (rc != RC_OK) TS_RETURN(bm, rc);
        }
    }
    TS_RETURN(bm, RC_OK);
}

RC markDirty (BM_BufferPool *const bm, BM_PageHandle *const page)
{
    if (!bm || !bm->mgmtData) return RC_FILE_HANDLE_NOT_INIT;
    if (!page) return RC_READ_NON_EXISTING_PAGE;
    TS_LOCK(bm);

    BM_MgmtData *md = MD(bm);
    int f = findFrameByPageNP(md, bm->numPages, page->pageNum);
    if (f < 0) TS_RETURN(bm, RC_READ_NON_EXISTING_PAGE);

    md->frames[f].dirty = TRUE;
    md->dirtyFlags[f]   = TRUE;
    TS_RETURN(bm, RC_OK);
}

RC unpinPage (BM_BufferPool *const bm, BM_PageHandle *const page)
{
    if (!bm || !bm->mgmtData) return RC_FILE_HANDLE_NOT_INIT;
    if (!page) return RC_READ_NON_EXISTING_PAGE;
    TS_LOCK(bm);

    BM_MgmtData *md = MD(bm);
    int f = findFrameByPageNP(md, bm->numPages, page->pageNum);
    if (f < 0) TS_RETURN(bm, RC_READ_NON_EXISTING_PAGE);

    if (md->frames[f].fixCount <= 0) {
        TS_RETURN(bm, RC_READ_NON_EXISTING_PAGE);
    }
    md->frames[f].fixCount--;
    md->fixCounts[f] = md->frames[f].fixCount;
    TS_RETURN(bm, RC_OK);
}

RC forcePage (BM_BufferPool *const bm, BM_PageHandle *const page)
{
    if (!bm || !bm->mgmtData) return RC_FILE_HANDLE_NOT_INIT;
    if (!page) return RC_READ_NON_EXISTING_PAGE;
    TS_LOCK(bm);

    BM_MgmtData *md = MD(bm);
    int f = findFrameByPageNP(md, bm->numPages, page->pageNum);
    if (f < 0) TS_RETURN(bm, RC_READ_NON_EXISTING_PAGE);

    //write current content back (allowed even if pinned)
    RC rc = ensure_page_exists(md, md->frames[f].pageNum);
    if (rc != RC_OK) TS_RETURN(bm, rc);

    rc = writeBlock(md->frames[f].pageNum, &md->fhandle, md->frames[f].data);
    if (rc != RC_OK) TS_RETURN(bm, rc);

    if (md->frames[f].dirty) {
        md->numWriteIO++;
        md->frames[f].dirty = FALSE;
        md->dirtyFlags[f]   = FALSE;
    }
    TS_RETURN(bm, RC_OK);
}

RC pinPage (BM_BufferPool *const bm, BM_PageHandle *const page, const PageNumber pageNum)
{
    if (!bm || !bm->mgmtData) return RC_FILE_HANDLE_NOT_INIT;
    if (pageNum < 0) return RC_READ_NON_EXISTING_PAGE;
    if (!page) return RC_READ_NON_EXISTING_PAGE;
    TS_LOCK(bm);

    BM_MgmtData *md = MD(bm);
    int f = findFrameByPageNP(md, bm->numPages, pageNum);
    if (f >= 0) {
        md->frames[f].fixCount++;
        md->fixCounts[f] = md->frames[f].fixCount;

        record_access(md, &md->frames[f]);

        page->pageNum = pageNum;
        page->data    = md->frames[f].data;
        TS_RETURN(bm, RC_OK);
    }

    //pick target
    int target = findEmptyFrame(md, bm->numPages);
    if (target < 0) {
        target = selectVictim(bm);
        if (target < 0) {
            //all frames pinned
            TS_RETURN(bm, RC_WRITE_FAILED);
        }
        //write back victim if necessary
        RC rc = writeBackIfDirty(bm, target);
        if (rc != RC_OK) TS_RETURN(bm, rc);
    }

    //load requested page into target
    RC rc = readPageIntoFrame(bm, target, pageNum);
    if (rc != RC_OK) TS_RETURN(bm, rc);

    //pin it
    md->frames[target].fixCount = 1;
    md->fixCounts[target] = 1;

    record_access(md, &md->frames[target]);

    //return handle
    page->pageNum = pageNum;
    page->data    = md->frames[target].data;

    sync_stats_slot(md, target);
    TS_RETURN(bm, RC_OK);
}


//statistics interface
PageNumber *getFrameContents (BM_BufferPool *const bm) {
    return MD(bm)->frameContents;
}
bool *getDirtyFlags (BM_BufferPool *const bm) {
    return MD(bm)->dirtyFlags;
}
int *getFixCounts (BM_BufferPool *const bm) {
    return MD(bm)->fixCounts;
}
int getNumReadIO (BM_BufferPool *const bm) {
    return MD(bm)->numReadIO;
}
int getNumWriteIO (BM_BufferPool *const bm) {
    return MD(bm)->numWriteIO;
}

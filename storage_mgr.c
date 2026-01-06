#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage_mgr.h"
#include "dberror.h"

//internal bookkeeping for FILE
typedef struct SM_Internal { FILE *fp; } SM_Internal;

//portable strdup fallback-if needed 
static char *sm_strdup(const char *s) {
    if (!s) return NULL;
#if defined(_MSC_VER)
    return _strdup(s);
#else
    char *d = strdup(s);
    if (d) return d;
    size_t n = strlen(s) + 1;
    d = (char *)malloc(n);
    if (!d) return NULL;
    memcpy(d, s, n);
    return d;
#endif
}

//helpers 
static FILE *fh_fp(SM_FileHandle *f) {
    return (!f || !f->mgmtInfo) ? NULL : ((SM_Internal*)f->mgmtInfo)->fp;
}

static long file_size_bytes(FILE *fp) {
    long cur = ftell(fp);
    if (cur < 0) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) return -1;
    long end = ftell(fp);
    //restore position 
    (void)fseek(fp, cur, SEEK_SET);
    return end;
}

// API impl 

void initStorageManager(void) {
    //not required but harmless and can help debugging
    RC_message = NULL;
}

//create a new page file with exactly one zeroed page 
RC createPageFile(char *fileName) {
    if (!fileName) THROW(RC_WRITE_FAILED, "fileName is NULL");
    FILE *fp = fopen(fileName, "wb+");
    if (!fp) THROW(RC_WRITE_FAILED, "could not create file");

    char *zero = (char*)calloc(PAGE_SIZE, 1);
    if (!zero) { fclose(fp); THROW(RC_WRITE_FAILED, "alloc failed"); }

    size_t w = fwrite(zero, 1, PAGE_SIZE, fp);
    free(zero);
    if (w != PAGE_SIZE) { fclose(fp); THROW(RC_WRITE_FAILED, "initial page write failed"); }

    fflush(fp);
    fclose(fp);
    return RC_OK;
}

//open existing page file and initialize handle fields 
RC openPageFile(char *fileName, SM_FileHandle *fHandle) {
    if (!fHandle)              THROW(RC_FILE_HANDLE_NOT_INIT, "file handle is NULL");
    if (!fileName || !*fileName) THROW(RC_FILE_NOT_FOUND, "file name is empty");

    FILE *fp = fopen(fileName, "rb+");
    if (!fp) THROW(RC_FILE_NOT_FOUND, "file not found");

    long size = file_size_bytes(fp);
    if (size < 0) { fclose(fp); THROW(RC_FILE_HANDLE_NOT_INIT, "could not stat file"); }
    if (size % PAGE_SIZE != 0) { fclose(fp); THROW(RC_FILE_HANDLE_NOT_INIT, "file size not multiple of PAGE_SIZE"); }
    int total = (int)(size / PAGE_SIZE);

    SM_Internal *in = (SM_Internal*)malloc(sizeof(SM_Internal));
    if (!in) { fclose(fp); THROW(RC_FILE_HANDLE_NOT_INIT, "alloc mgmtInfo failed"); }

    char *fname_dup = sm_strdup(fileName);
    if (!fname_dup) { free(in); fclose(fp); THROW(RC_FILE_HANDLE_NOT_INIT, "alloc fileName failed"); }

    in->fp = fp;
    fHandle->fileName = fname_dup;          /* duplicated for safe lifetime */
    fHandle->mgmtInfo = in;
    fHandle->totalNumPages = total;
    fHandle->curPagePos = 0;

    return RC_OK;
}

//close open page file and free bookkeeping 
RC closePageFile(SM_FileHandle *fHandle) {
    if (!fHandle) THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    FILE *fp = fh_fp(fHandle);
    if (!fp) THROW(RC_FILE_HANDLE_NOT_INIT, "not open");

    fclose(fp);
    ((SM_Internal*)fHandle->mgmtInfo)->fp = NULL;
    free(fHandle->mgmtInfo);
    fHandle->mgmtInfo = NULL;

    //free duplicated fileName (if set) 
    if (fHandle->fileName) {
        free(fHandle->fileName);
        fHandle->fileName = NULL;
    }

    //clear other fields for safety 
    fHandle->totalNumPages = 0;
    fHandle->curPagePos = 0;

    return RC_OK;
}

//destroy (delete) a page file from disk 
RC destroyPageFile(char *fileName) {
    if (!fileName || !*fileName) THROW(RC_FILE_NOT_FOUND, "file name is empty");
    if (remove(fileName) == 0) return RC_OK;
    THROW(RC_FILE_NOT_FOUND, "remove failed");
}

//reading helperss and APIs 

static RC readBlockAt(int pageNum, SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (!fHandle)                   THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    if (!memPage)                   THROW(RC_READ_NON_EXISTING_PAGE, "NULL memPage");
    FILE *fp = fh_fp(fHandle);
    if (!fp)                        THROW(RC_FILE_HANDLE_NOT_INIT, "not open");
    if (pageNum < 0 || pageNum >= fHandle->totalNumPages)
                                    THROW(RC_READ_NON_EXISTING_PAGE, "page out of bounds");

    long off = (long)pageNum * (long)PAGE_SIZE;
    if (fseek(fp, off, SEEK_SET) != 0)      THROW(RC_READ_NON_EXISTING_PAGE, "seek failed");

    size_t r = fread(memPage, 1, PAGE_SIZE, fp);
    if (r != PAGE_SIZE)                     THROW(RC_READ_NON_EXISTING_PAGE, "short read");

    fHandle->curPagePos = pageNum;
    return RC_OK;
}

RC readBlock(int pageNum, SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return readBlockAt(pageNum, fHandle, memPage);
}

int getBlockPos(SM_FileHandle *fHandle) {
    return fHandle ? fHandle->curPagePos : -1;
}

RC readFirstBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return readBlockAt(0, fHandle, memPage);
}

RC readPreviousBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (!fHandle) THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    return readBlockAt(fHandle->curPagePos - 1, fHandle, memPage);
}

RC readCurrentBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (!fHandle) THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    return readBlockAt(fHandle->curPagePos, fHandle, memPage);
}

RC readNextBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (!fHandle) THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    return readBlockAt(fHandle->curPagePos + 1, fHandle, memPage);
}

RC readLastBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (!fHandle) THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    return readBlockAt(fHandle->totalNumPages - 1, fHandle, memPage);
}

//writing helpers and APIs

static RC writeBlockAt(int pageNum, SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (!fHandle)                   THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    if (!memPage)                   THROW(RC_WRITE_FAILED, "NULL memPage");
    FILE *fp = fh_fp(fHandle);
    if (!fp)                        THROW(RC_FILE_HANDLE_NOT_INIT, "not open");
    if (pageNum < 0 || pageNum >= fHandle->totalNumPages)
                                    THROW(RC_WRITE_FAILED, "page out of bounds for write");

    long off = (long)pageNum * (long)PAGE_SIZE;
    if (fseek(fp, off, SEEK_SET) != 0)      THROW(RC_WRITE_FAILED, "seek failed for write");

    size_t w = fwrite(memPage, 1, PAGE_SIZE, fp);
    if (w != PAGE_SIZE)                     THROW(RC_WRITE_FAILED, "short write");

    fflush(fp);
    fHandle->curPagePos = pageNum;
    return RC_OK;
}

RC writeBlock(int pageNum, SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return writeBlockAt(pageNum, fHandle, memPage);
}

RC writeCurrentBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (!fHandle) THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    return writeBlockAt(fHandle->curPagePos, fHandle, memPage);
}

//growwing the file 

RC appendEmptyBlock(SM_FileHandle *fHandle) {
    if (!fHandle) THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    FILE *fp = fh_fp(fHandle);
    if (!fp) THROW(RC_FILE_HANDLE_NOT_INIT, "not open");

    if (fseek(fp, 0, SEEK_END) != 0) THROW(RC_WRITE_FAILED, "seek end failed");

    char *zero = (char*)calloc(PAGE_SIZE, 1);
    if (!zero) THROW(RC_WRITE_FAILED, "alloc failed");

    size_t w = fwrite(zero, 1, PAGE_SIZE, fp);
    free(zero);
    if (w != PAGE_SIZE) THROW(RC_WRITE_FAILED, "append write failed");

    fflush(fp);
    fHandle->totalNumPages += 1;
    fHandle->curPagePos = fHandle->totalNumPages - 1;

    return RC_OK;
}

RC ensureCapacity(int numberOfPages, SM_FileHandle *fHandle) {
    if (!fHandle) THROW(RC_FILE_HANDLE_NOT_INIT, "NULL handle");
    if (numberOfPages < 0) THROW(RC_FILE_HANDLE_NOT_INIT, "invalid requested capacity");

    while (fHandle->totalNumPages < numberOfPages) {
        RC rc = appendEmptyBlock(fHandle);
        if (rc != RC_OK) return rc;
    }
    return RC_OK;
}

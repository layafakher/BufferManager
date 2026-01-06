## 📌 Overview

This project implements a **Buffer Manager** for a simple Database Management System (DBMS).  
The buffer manager caches disk pages from a page file into main memory and manages them using page
replacement strategies.

It builds directly on **Assignment 1 (Storage Manager)** and reuses its page-based file I/O functionality.

---

## 🎯 Objectives

The Buffer Manager is responsible for:

- Managing a fixed number of **page frames** in memory
- Loading disk pages into memory on demand (**pinning**)
- Tracking page usage via **fix counts**
- Supporting page replacement strategies (**FIFO** and **LRU**)
- Writing modified (**dirty**) pages back to disk when evicted
- Collecting I/O statistics

Multiple buffer pools may exist at the same time, but **only one buffer pool per page file** is allowed.

---

## 🧠 Core Concepts

### Buffer Pool
A buffer pool consists of:
- A page file on disk
- A fixed number of page frames in memory
- A page replacement strategy

### Pinning and Unpinning
- **Pinning** a page loads it into memory and increments its fix count
- **Unpinning** a page decrements its fix count
- Only pages with fix count `0` can be replaced

### Dirty Pages
- Pages modified by clients must be marked as **dirty**
- Dirty pages must be written back to disk before eviction
- A page is no longer dirty after being flushed to disk

---

## 🗂️ Data Structures

### `BM_BufferPool`

```c
typedef struct BM_BufferPool {
    char *pageFile;
    int numPages;
    ReplacementStrategy strategy;
    void *mgmtData;
} BM_BufferPool;
````

* Represents a buffer pool
* `mgmtData` stores internal bookkeeping information (frames, metadata, replacement data)

### `BM_PageHandle`

```c
typedef struct BM_PageHandle {
    PageNumber pageNum;
    char *data;
} BM_PageHandle;
```

* Represents a pinned page
* `data` points to the page frame in memory

---

## 🔁 Replacement Strategies

The following strategies are implemented:

* **FIFO (First-In First-Out)**
* **LRU (Least Recently Used)**

Pages with non-zero fix counts are **never evicted**, regardless of strategy.

---

## 🔌 Buffer Manager Interface

The Buffer Manager implements the interface defined in `buffer_mgr.h`.

### Buffer Pool Management

* `initBufferPool`
* `shutdownBufferPool`
* `forceFlushPool`

### Page Management

* `pinPage`
* `unpinPage`
* `markDirty`
* `forcePage`

### Statistics

* `getFrameContents`
* `getDirtyFlags`
* `getFixCounts`
* `getNumReadIO`
* `getNumWriteIO`

---

## 📊 Statistics Tracking

The buffer manager tracks:

* Pages currently stored in each frame
* Dirty flags for each frame
* Fix counts per frame
* Number of disk read operations
* Number of disk write operations

These statistics are used by provided debug and print functions.

---

## 🧪 Testing

* Provided test files:

  * `test_assign2_1.c` (FIFO & LRU tests)
  * `test_assign2_2.c`
* Tests verify:

  * Correct page replacement behavior
  * Dirty page handling
  * Fix count correctness
  * I/O statistics accuracy

All required tests pass successfully.

---

## 📁 Project Structure

```
assign2/
├── README.md
├── Makefile
├── buffer_mgr.h
├── buffer_mgr_stat.c
├── buffer_mgr_stat.h
├── dberror.c
├── dberror.h
├── dt.h
├── storage_mgr.h
├── test_assign2_1.c
├── test_assign2_2.c
└── test_helper.h
```

---

## 🛠️ Build Instructions

```bash
make
```

This command builds the test executables for the buffer manager.

---

## ⚠️ Error Handling

* All functions return an `RC` (Return Code)
* Error codes are defined in `dberror.h`
* Dirty pages are always flushed before eviction
* It is an error to shut down a buffer pool with pinned pages

---

## ✅ Status

* FIFO and LRU strategies implemented
* Storage Manager reused from Assignment 1
* No memory leaks
* All required test cases pass

---

## 📚 Notes

This Buffer Manager closely resembles real DBMS buffer pool behavior and serves as the foundation
for higher-level components such as the **Record Manager**.

```



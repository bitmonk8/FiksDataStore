# MDB Environment Technical Documentation

## 1. Overview

The `env.h` and `env.cpp` files are the core of the FiksDataStore library's environment management. The "environment" is the highest-level object, representing a database location. It encapsulates the data file, the lock file, memory maps, and the overall context for transactions and concurrency control. It is the primary entry point for any application using FiksDataStore.

The key design principles evident in this module are:

*   **Memory-Mapped I/O:** The entire database is treated as a single memory-mapped file, delegating caching and page management to the operating system's virtual memory manager. This simplifies the code and often yields superior performance.
*   **Multi-Version Concurrency Control (MVCC):** FiksDataStore uses a non-locking MVCC model that allows for concurrent read transactions without blocking a single write transaction, and vice-versa. This is the cornerstone of its high read performance.
*   **Transactional Integrity:** The environment uses a double-buffered meta-page system to ensure that the database is always in a consistent state, making transactions atomic and durable (ACID).
*   **Copy-on-Write:** Data pages are never modified in place. When a write transaction needs to modify a page, it creates a copy, writes to the copy, and updates parent pages to point to the new version.

---

## 2. Core Data Structures

### 2.1. `struct FDS_env`

Defined in [`Lib/env.h:120`](Lib/env.h:120), this is the opaque handle that represents the entire database environment. It holds all state associated with an open database.

**Key Fields:**

*   `me_fd`, `me_lfd`, `me_mfd`: File handles for the data file, lock file, and a special write-through handle for meta pages, respectively.
*   `me_map`, `me_mapsize`: The base address and size of the memory-mapped data file.
*   `me_txns`: A pointer to the memory-mapped lock file, which contains the reader table and writer mutex.
*   `me_metas[NUM_METAS]`: An array of two pointers, each pointing to one of the two meta pages within the mapped data file. This is central to the MVCC mechanism.
*   `me_flags`: A bitmask of flags that define the environment's behavior (e.g., `FDS_RDONLY`, `FDS_WRITEMAP`, `FDS_NOSYNC`).
*   `me_psize`: The page size used by the database.
*   `me_txn`, `me_txn0`: Pointers related to the single, pre-allocated write transaction structure.
*   `me_free_pgs`, `me_dirty_list`: Data structures (`FDS_IDL`, `FDS_ID2L`) for managing free and dirty pages within a write transaction.
*   `me_rmutex`, `me_wmutex`: Mutexes for controlling access to the reader table and for serializing write transactions. Their implementation is platform-specific.

### 2.2. `struct FDS_meta`

Defined in [`Lib/env.h:18`](Lib/env.h:18), this structure represents the header of a database snapshot. Two instances of this structure exist at the beginning of the data file, on page 0 and page 1.

**Key Fields:**

*   `mm_magic`, `mm_version`: Constants (`0xFDFDC0DE` and `1`) that identify the file as a valid FiksDataStore data file.
*   `mm_txnid`: The transaction ID that committed this meta page. The meta page with the higher `txnid` represents the most recent, consistent state of the database.
*   `mm_dbs[CORE_DBS]`: An array of two `FDS_db` structures.
    *   `[FREE_DBI]` (index 0): The root and metadata for the B-tree that tracks free pages.
    *   `[MAIN_DBI]` (index 1): The root and metadata for the main user-created database.
*   `mm_last_pg`: The highest page number currently allocated in the data file.
*   `mm_mapsize`: The size of the data memory map.

### 2.3. `struct FDS_txninfo` & `struct FDS_txbody`

Defined in [`Lib/env.h:86`](Lib/env.h:86), these structures define the layout of the lock file (`lock.mdb`). The lock file is also memory-mapped and is crucial for coordinating concurrent access.

**Key Fields:**

*   `mtb_magic`, `mtb_format`: Magic and format identifiers for the lock file.
*   `mtb_txnid`: A volatile copy of the last committed transaction ID. This allows new transactions to quickly find the latest version without needing to acquire locks.
*   `mtb_numreaders`: The number of active read transaction slots currently in use.
*   `mti_readers[]`: A flexible array of `FDS_reader` structures. This is the **Reader Table**. Each active read transaction acquires a slot in this table and records its process ID and the `txnid` of the snapshot it is reading. This table is what allows the writer to determine which old pages are no longer in use and can be reclaimed.

---

## 3. Main Algorithms and Operations

### 3.1. Environment Opening (`fds_env_open`)

This is the primary function for initializing the database environment. It is a multi-step process orchestrated by [`fds_env_open()`](Lib/env.cpp:1566).

1.  **Path and Flag Handling:** It resolves the path to the database directory and validates the user-provided flags.
2.  **Lock File Setup (`fds_env_setup_locks`)**:
    *   It opens (or creates) the lock file (`lock.mdb`).
    *   It acquires a file-level lock (`fcntl` or `LockFileEx`) to determine if it is the first process initializing the environment.
    *   If it's the first, it initializes the lock file's header, reader table, and the platform-specific reader/writer mutexes.
    *   If not the first, it simply opens the existing mutexes/semaphores.
    *   Finally, it memory-maps the lock file, making the reader table and mutexes available in memory.
3.  **Data File Setup (`fds_env_open2`)**:
    *   It opens (or creates) the data file (`data.mdb`).
    *   **Header Reading (`fds_env_read_header`)**: It reads both meta pages (0 and 1) to determine which is newer by comparing their `mm_txnid` values.
    *   **Initialization**: If the data file is new, it calls `fds_env_init_meta` to create the initial two meta pages.
    *   **Memory Mapping (`fds_env_map`)**: It memory-maps the data file using `mmap` (POSIX) or `NtMapViewOfSection` (Windows). The `FDS_WRITEMAP` flag determines if the mapping is read-write or read-only.
    *   It sets up internal environment parameters (`me_maxpg`, `me_nodemax`, etc.) based on the page size.

### 3.2. Transactional Commit (`fds_env_write_meta`)

This function, defined in [`Lib/env.cpp:631`](Lib/env.cpp:631), is the final step of a successful write transaction.

1.  **Toggle Meta Page:** It determines which meta page to write to based on the new transaction ID (`txn->mt_txnid & 1`).
2.  **Construct New Meta:** It builds a new `FDS_meta` structure containing the updated state from the committed transaction, including the new root page of the main B-tree, the new root of the free-pages B-tree, and the new last-allocated page number.
3.  **Atomic Write:** It writes the new meta information to the inactive meta page. This operation is the atomic switch that makes the transaction's changes visible to all subsequent read transactions.
4.  **Update Lock File:** It updates the `mti_txnid` in the shared lock file memory to signal the new latest version to all processes.

### 3.3. Concurrency Control (MVCC)

The environment manages concurrency through a clever interplay between the data file, the lock file, and transaction states.

*   **Writer Serialization:** A single writer mutex (`me_wmutex`) ensures only one thread can perform a write transaction at a time.
*   **Reader Registration:** When a read transaction begins, it acquires the reader lock (`me_rmutex`), finds a free slot in the reader table (`mti_readers`), records its PID and the current latest `txnid`, and then releases the lock. The transaction is now "live" and operates on a fixed snapshot of the database.
*   **Page Reclamation:** When a write transaction commits, it needs to add old, now-unreferenced pages to the free list. To do this safely, it scans the reader table to find the oldest `txnid` being used by any active reader. Any page that was freed by a transaction *older* than this oldest reader is guaranteed to be no longer visible to any transaction and can be safely reclaimed.

---

## 4. Platform-Specific Implementations

The code is rich with `#ifdef` blocks to handle differences between operating systems, primarily for file I/O, memory mapping, and synchronization primitives.

*   **Windows (`FDS_WINDOWS`)**: Uses `CreateFile`, `NtCreateSection`, `NtMapViewOfSection`, named `Mutex` objects for synchronization, and `OVERLAPPED` I/O for certain operations.
*   **POSIX (`FDS_LINUX`)**: Uses `mmap` and process-shared, robust `pthread_mutex_t` mutexes stored directly in the lock file's shared memory.
*   **System V Semaphores (`FDS_MACOS`)**: Uses `semget` and `semctl`.

This cross-platform support is a major feature, but also a source of complexity within the codebase.
# MDB Locking (`lock.h`, `lock.cpp`)

## 1. Purpose

The `fds_lock` module implements the core concurrency control mechanism for the MDB database. It uses a multi-version concurrency control (MVCC) model combined with a reader-writer lock pattern. This allows multiple reader transactions to run concurrently without blocking each other, while ensuring that a single writer transaction has exclusive access to modify the database.

The primary challenge in this model is managing the lifecycle of read transactions, especially when processes or threads terminate abnormally without releasing their locks. This module provides the mechanisms to detect and clean up these "stale" readers to prevent them from indefinitely blocking writers from reusing old database pages.

## 2. Main Data Structures

### `FDS_reader`

Defined in [`Lib/fds_lock.h:43`](Lib/fds_lock.h:43), this is the fundamental structure for tracking a single read transaction. An array of these structures forms the "reader table" in shared memory.

```cpp
struct FDS_reader {
    union {
        struct FDS_rxbody {
            volatile txnid_t mrb_txnid; // The transaction ID when the read started
            volatile FDS_PID_T mrb_pid; // The process ID of the reader
            volatile FDS_THR_T mrb_tid; // The thread ID of the reader
        } mrx;
        // Cache-line padding
        char pad[...];
    } mru;
};
```

-   `mrb_txnid`: This is the most critical field for MVCC. It records the database version (transaction ID) that the reader is observing. A writer cannot reuse any database pages that are part of a transaction version newer than or equal to the oldest `mrb_txnid` among all active readers.
-   `mrb_pid` / `mrb_tid`: These fields identify the owner of the read lock. They are essential for the stale reader detection logic. When a slot is not in use, `mrb_pid` is 0.

The entire `FDS_reader` struct is padded to a full cache line to prevent "false sharing" performance issues when multiple cores are concurrently accessing different reader slots.

## 3. Core Algorithms

### 3.1. Stale Reader Detection and Cleanup

The main function for this is [`fds_reader_check0()`](Lib/lock.cpp:213). It is called to find and purge reader table entries left behind by dead processes.

**Algorithm:**

1.  **Collect Unique PIDs**: The function iterates through the entire reader table. For every active reader slot (`mr_pid != 0`) belonging to a process other than the current one, it adds the process ID (`PID`) to a temporary list. A helper function, [`fds_pid_insert()`](Lib/fds_lock.cpp:108), uses a binary search to efficiently build a sorted list of unique PIDs.
2.  **Check Liveness**: It then iterates through this list of unique PIDs. For each PID, it calls [`fds_reader_pid()`](Lib/lock.cpp:23) to determine if the process is still alive.
3.  **Clear Stale Entries**: If [`fds_reader_pid()`](Lib/lock.cpp:23) reports that a process is dead, the function must clear its entries from the reader table.
    -   It acquires the reader mutex (`me_rmutex`) to prevent race conditions with other processes that might be checking or modifying the table.
    -   As a safeguard, it re-checks the process liveness after acquiring the lock, in case a new process has reused the PID in the interim.
    -   If the process is still confirmed to be dead, it performs a final scan of the reader table and clears any slot where `mr_pid` matches the dead PID by setting it to 0.

### 3.2. Process Liveness Check (`fds_reader_pid`)

This function ([`Lib/lock.cpp:23`](Lib/lock.cpp:23)) provides a platform-specific way to check if a process is running.

-   **On POSIX systems (Linux, macOS):** It uses a clever file-locking trick. MDB maintains a lock on a single byte of the lock file at an offset corresponding to its own PID. To check if another process is alive, it attempts to acquire a write lock (`fcntl` with `F_GETLK`) on the byte at that process's PID offset.
    -   If the lock is already held (`lock_info.l_type != F_UNLCK`), the process is considered alive.
    -   If the lock can be acquired, it means the process is dead, as the operating system automatically releases file locks when a process terminates.
-   **On Windows:** The check is more direct. It uses `OpenProcess()` to get a handle to the process. If the handle is valid, it calls `WaitForSingleObject()` with a zero timeout. If the process has exited, the wait succeeds immediately. If it's still running, the wait times out.

### 3.3. Mutex Failure Recovery (`fds_mutex_failed`)

This function ([`Lib/lock.cpp:164`](Lib/lock.cpp:164)) handles the critical scenario where a process dies while holding a mutex.

-   When a mutex lock attempt returns `FDS_OWNERDEAD` (or a platform-specific equivalent like `EOWNERDEAD`), it means the caller has been granted ownership of the mutex, but the previous owner terminated abnormally.
-   The database is now in a potentially inconsistent state. The `fds_mutex_failed` function's job is to perform recovery.
-   Its primary action is to call [`fds_reader_check0()`](Lib/lock.cpp:213) to clean up any stale reader locks left by the dead process (or any other dead processes).
-   If the dead process was a writer, it also updates the shared transaction ID (`mt1.mtb.mtb_txnid`) to the latest meta page, ensuring the next writer starts from a consistent state.
-   If the dead thread belonged to the *current* process, the environment is considered irrecoverably corrupted (`FDS_FATAL_ERROR`), and a `FDS_PANIC` error is returned.

## 4. Usage by Developers

-   A developer modifying this code needs to be acutely aware of the distinction between POSIX and Windows platform-specific implementations, which are handled by `#ifdef` blocks.
-   The logic for stale reader cleanup is central to the stability of the database. Any changes must be carefully evaluated for potential race conditions. The use of the reader mutex (`me_rmutex`) is critical for atomicity.
-   The `fds_reader_pid` function is a key abstraction. Its contract is simple: return 0 if a process is certainly dead, and non-zero otherwise. The underlying implementation can be complex, but the interface is clean.
-   Understanding the `FDS_OWNERDEAD` recovery path is crucial for debugging rare and hard-to-reproduce deadlocks or corruption issues that can arise from process crashes.
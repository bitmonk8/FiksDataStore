# MDB Transaction Management (`txn.h`, `txn.cpp`)

This document provides a technical overview of the transaction management module in MDB. It covers the core data structures, functions, and internal mechanisms for handling database transactions.

## 1. Overview

The transaction module is central to MDB's operation, providing the ACID properties (Atomicity, Consistency, Isolation, Durability) for all database operations. Every read or write operation must occur within a transaction. The implementation supports a single-writer, multiple-reader model. It also allows for nested write transactions, providing a way to group operations that can be committed or rolled back as a single unit within a larger transaction.

-   **`txn.h`**: Defines the primary `FDS_txn` structure, transaction flags, and public function prototypes for transaction management.
-   **`txn.cpp`**: Implements the logic for creating, renewing, committing, and aborting transactions, including handling nested transactions, page management, and cursor lifecycle.

## 2. Core Data Structures

### `struct FDS_txn`

The `FDS_txn` structure represents a single transaction, either read-only or read-write. It holds all the necessary state for the duration of the transaction.

| Field | Description |
| :--- | :--- |
| `mt_parent` | A pointer to the parent transaction if this is a nested transaction. `NULL` for top-level transactions. |
| `mt_child` | A pointer to a single nested child transaction. Only one child is allowed at a time. |
| `mt_next_pgno` | The next available page number to be allocated. |
| `mt_txnid` | The unique ID for this transaction. For read transactions, it's the ID of the database state they are viewing. For write transactions, it's a new, incrementing ID. |
| `mt_env` | A pointer to the `FDS_env` environment to which this transaction belongs. |
| `mt_free_pgs` | An `FDS_IDL` (ID List) of page numbers that have been freed during this transaction and will be added to the global freelist upon commit. |
| `mt_loose_pgs` | A list of pages that were freed but can be reused within the same transaction. This helps reduce fragmentation and file growth. |
| `mt_loose_count` | The number of pages in the `mt_loose_pgs` list. |
| `mt_spill_pgs` | An `FDS_IDL` of dirty pages that have been "spilled" to disk temporarily because the dirty list was full. This is a mechanism to handle large transactions that modify more pages than can fit in memory. |
| `mt_u.dirty_list` | (Write Txn) An `FDS_ID2L` (ID-pointer List) of pages modified in this transaction that have not yet been written to the database file. |
| `mt_u.reader` | (Read Txn) A pointer to the transaction's slot in the reader table, which tracks active read transactions. |
| `mt_dbxs` | An array of `FDS_dbx` records, representing the named databases. |
| `mt_dbs` | An array of `FDS_db` records, holding the state (e.g., root page, flags) for each database instance (DBI) within the transaction. |
| `mt_dbiseqs` | An array of sequence numbers for each DBI handle to detect stale handles. |
| `mt_cursors` | (Write Txn) An array of linked lists of cursors associated with each DBI. |
| `mt_dbflags` | An array of flags (`DB_DIRTY`, `DB_STALE`, etc.) for each DBI. |
| `mt_numdbs` | The number of active DBI handles in the transaction. |
| `mt_flags` | Flags that define the state and behavior of the transaction (e.g., `FDS_TXN_RDONLY`, `FDS_TXN_DIRTY`). |
| `mt_dirty_room` | The remaining capacity in the dirty list. |

### `struct FDS_ntxn`

This structure is used for nested transactions. It embeds an `FDS_txn` and adds state to manage the parent's page state.

| Field | Description |
| :--- | :--- |
| `mnt_txn` | The embedded `FDS_txn` structure for the child transaction. |
| `mnt_pgstate` | Saves the parent transaction's page state (`me_pgstate.mf_pghead`, etc.) before the child transaction begins. This allows the parent's state to be restored if the child aborts. |

## 3. Transaction Lifecycle Functions

### `fds_txn_begin()`

-   **Purpose**: Starts a new transaction.
-   **Behavior**:
    -   Allocates memory for the `FDS_txn` structure.
    -   For **read-only transactions**, it finds an available slot in the reader table and sets the transaction ID to the current database transaction ID.
    -   For **top-level write transactions**, it reuses the pre-allocated `env->me_txn0` structure and increments the global transaction ID. It acquires a write lock on the environment.
    -   For **nested write transactions**, it allocates a new `FDS_ntxn`, saves the parent's page state, and "shadows" the parent's cursors. The child inherits the parent's transaction ID.

### `fds_txn_commit()`

-   **Purpose**: Commits the changes made in a transaction.
-   **Behavior**:
    -   **Nested Transaction**: Merges its state into the parent transaction. This includes merging the dirty list, free page list, loose page list, and spilled page list. Cursors are also merged back. The child transaction's memory is then freed. The commit is not durable until the top-level parent commits.
    -   **Top-level Transaction**:
        1.  **Update DB Roots**: If any named databases were modified, their root page info is updated in the main database.
        2.  **Save Freelist**: The list of freed pages (`mt_free_pgs`) is written to the free database (`FREE_DBI`).
        3.  **Flush Pages**: All pages in the dirty list are written to the database file via `fds_page_flush()`.
        4.  **Sync (Optional)**: If not disabled (`FDS_NOSYNC`), the memory-mapped file is synchronized to disk to ensure durability.
        5.  **Write Meta Pages**: The two meta pages are updated alternately with the new transaction ID and root page numbers. This is the atomic point of the commit.
        6.  **Release Locks**: The write lock is released.

### `fds_txn_abort()`

-   **Purpose**: Aborts a transaction, discarding all changes.
-   **Behavior**:
    -   **Nested Transaction**: The parent's page state is restored from `mnt_pgstate`. All pages in the child's dirty list are discarded. Cursors are reverted to their state before the child began. The child's memory is freed.
    -   **Top-level Transaction**: All pages in the dirty list are discarded. Any newly allocated pages are added back to the freelist. The write lock is released. The transaction structure is marked as finished.

### `fds_txn_renew()`

-   **Purpose**: "Resets" a read-only transaction handle, allowing it to be reused. This is more efficient than aborting and beginning a new transaction.
-   **Behavior**: The handle is reset and re-initialized with the latest transaction ID from the environment, effectively giving it a fresh, up-to-date view of the database.

## 4. Internal Mechanisms

### Page Management

-   **Dirty Pages (`mt_u.dirty_list`)**: When a page is modified for the first time in a write transaction, a copy is made, and a pointer to this copy is stored in the `dirty_list`. This list is sorted by page number to optimize disk writes. This is a core part of the Copy-on-Write (CoW) mechanism.
-   **Free Pages (`mt_free_pgs`)**: When a page is no longer needed (e.g., a B-tree node is deleted), its page number is added to the `mt_free_pgs` IDL. This list is merged into the main freelist when the top-level transaction commits.
-   **Loose Pages (`mt_loose_pgs`)**: Freed pages can be immediately reused within the same transaction. This avoids unnecessarily growing the database file if pages are freed and new pages are needed in short succession.
-   **Spilled Pages (`mt_spill_pgs`)**: If a transaction is very large and the in-memory `dirty_list` becomes full, the sorted list of dirty pages is written to a temporary "spill" file, and their page numbers are added to `mt_spill_pgs`. This allows transactions to be larger than available RAM. When the transaction commits, these spilled pages are flushed to the main database file along with the remaining dirty pages.

### Cursor Management in Nested Transactions

When a nested transaction begins, it needs its own set of cursors that can be modified without affecting the parent's cursors until the child commits.

-   **`fds_cursor_shadow()`**: This function is called when a nested transaction starts. For each cursor in the parent transaction, it:
    1.  Allocates a backup copy of the `FDS_cursor` structure.
    2.  The original cursor is moved to the child transaction's list of active cursors (`dst->mt_cursors`).
    3.  The original cursor's `mc_backup` pointer is set to point to the newly allocated backup.
    4.  The backup now holds the parent's original cursor state.

-   **Commit/Abort**:
    -   On **commit**, the child's modified cursor state is kept, and the backup is freed.
    -   On **abort**, the original cursor is restored from the backup copy, effectively discarding any navigational changes made in the child transaction.

### Freelist Saving (`fds_freelist_save`)

This is a critical and complex part of the commit process. It saves the transaction's accumulated freelist (`mt_free_pgs`) and the environment's reclaimed freelist (`env->me_pgstate.mf_pghead`) into the `FREE_DBI`. This ensures that if the application crashes, the freelist can be recovered on the next startup, preventing space leaks. The process is iterative to handle cases where saving the freelist itself consumes pages, which in turn modifies the freelist.
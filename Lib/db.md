# Technical Documentation for `db.h` and `db.cpp`

## 1. Overview

The files [`db.h`](Lib/db.h:1) and [`db.cpp`](Lib/db.cpp:1) are central to the management of individual databases within the FiksDataStore environment. They provide the API and implementation for creating, opening, closing, and clearing databases. These files also contain the primary functions for data manipulation at the database level, such as adding, retrieving, and deleting key-value pairs.

The core design revolves around a special "main database" that acts as a directory, storing the metadata for all other "named databases" within the environment. Operations are transaction-based, ensuring ACID properties. The implementation uses integer handles ([`FDS_dbi`](Lib/db.h:29)) rather than pointers to refer to databases, which enhances safety and simplifies management across multiple transactions.

## 2. Core Data Structures

Two primary structures define a database's state: one for persistent on-disk metadata ([`FDS_db`](Lib/db.h:6)) and one for transient in-memory data ([`FDS_dbx`](Lib/db.h:21)).

### 2.1. `struct FDS_db`

This structure represents the persistent, on-disk metadata for a single database. It is stored as the value associated with the database's name in the main database.

-   [`uint32_t md_pad`](Lib/db.h:8): Padding for memory alignment.
-   [`uint16_t md_flags`](Lib/db.h:9): Persistent flags for the database, such as `FDS_REVERSEKEY`. These are set when the database is opened via [`dbi_open()`](Lib/db.cpp:22).
-   [`uint16_t md_depth`](Lib/fds_db.h:10): The current depth of the database's B+ tree.
-   [`pgno_t md_branch_pages`](Lib/db.h:11): The total number of branch (internal) pages in the tree.
-   [`pgno_t md_leaf_pages`](Lib/db.h:12): The total number of leaf pages in the tree.
-   [`pgno_t md_overflow_pages`](Lib/db.h:13): The total number of overflow pages used for large data items.
-   [`size_t md_entries`](Lib/db.h:14): The total number of key-value pairs stored in the database.
-   [`pgno_t md_root`](Lib/db.h:15): The page number of the B+ tree's root page. A value of `P_INVALID` indicates an empty database.

### 2.2. `struct FDS_dbx`

This structure holds auxiliary, in-memory information about a database that is relevant for the duration of a transaction.

-   [`FDS_val md_name`](Lib/db.h:23): The name of the database.
-   [`FDS_cmp_func md_cmp`](Lib/db.h:24): A function pointer to the key-comparison function. This is determined by the `md_flags` (e.g., `FDS_REVERSEKEY`) but can be overridden by the user with [`set_compare()`](Lib/db.cpp:430).
-   [`FDS_cmp_func md_dcmp`](Lib/db.h:25): A function pointer for comparing data items in databases with sorted duplicates. In this version of the code, this is unused as duplicate support has been simplified out.

## 3. Key Concepts

### 3.1. Database Handles (`FDS_dbi`)

An `FDS_dbi` is an integer handle that uniquely identifies an open database within a transaction. It is an index into the transaction's arrays (`mt_dbs`, `mt_dbxs`, etc.). This design avoids the complexity and risks of sharing pointers across threads or transactions.

### 3.2. The Main Database

The environment contains a special "main database" accessible via the handle `MAIN_DBI`. This database acts as a directory or catalog, mapping database names (as keys) to their corresponding [`FDS_db`](Lib/db.h:6) metadata structures (as values). When a named database is opened, its metadata is read from this main database.

### 3.3. DBI Handle Lifecycle and Validation

-   **Opening**: A handle is obtained via [`fds_dbi_open()`](Lib/db.cpp:22). If the database is already open in the current transaction, the existing handle is returned. Otherwise, the function searches the main DB for the database's metadata. If not found and `FDS_CREATE` is specified, a new database is created.
-   **Validation**: The macro [`TXN_DBI_EXIST(txn, dbi, validity)`](Lib/db.h:29) is used throughout the code to ensure that a given `dbi` handle is valid for the current transaction.
-   **Stale Handle Detection**: The environment maintains a sequence number for each DBI slot (`me_dbiseqs`). When a DBI is closed ([`fds_dbi_close()`](Lib/db.cpp:151)) or dropped, this sequence number is incremented. The macro [`TXN_DBI_CHANGED(txn, dbi)`](Lib/db.h:32) compares the transaction's copy of the sequence number with the environment's master copy. If they differ, it means the handle is stale (i.e., the database was closed or dropped by another transaction after the current transaction started), and operations on it will fail with `FDS_BAD_DBI`.

## 4. Main Functions and Algorithms

### 4.1. `fds_dbi_open()`

This function is responsible for acquiring a database handle.

**Algorithm**:
1.  Validate input flags and transaction state.
2.  If the `name` is `nullptr`, return the handle for the main database (`MAIN_DBI`).
3.  Iterate through the transaction's already-open databases (`txn->mt_dbxs`). If a database with the same name is found, return its handle.
4.  If not found, check if the maximum number of databases has been reached. If so, return `FDS_DBS_FULL`.
5.  Use a cursor on the main database to search for the provided `name`.
6.  **If found**: The associated value is the database's [`FDS_db`](Lib/db.h:6) metadata.
7.  **If not found**:
    -   If the `FDS_CREATE` flag was not provided, return `FDS_NOTFOUND`.
    -   If `FDS_CREATE` was provided, create a new, empty [`FDS_db`](Lib/db.h:6) record and write it to the main database, using the `name` as the key.
8.  Register the new DBI handle in the transaction by populating a slot in the `mt_dbs` and `mt_dbxs` arrays with the retrieved or newly created information.
9.  Set the default comparison function using [`fds_default_cmp()`](Lib/db.cpp:13).

### 4.2. `fds_drop0()`

[`fds_drop0()`](Lib/db.cpp:181) is the internal function to perform the core work of freeing a database's pages.

**Algorithm (`fds_drop0`)**:
1.  Initialize a cursor at the beginning of the database's B+ tree.
2.  Perform a full traversal of the tree. The traversal is optimized to skip scanning leaf pages if the database has no sub-databases or overflow pages, as only these conditions require inspecting leaf node data.
3.  For each **branch page**, add the page numbers of all its child nodes to the transaction's free list (`txn->mt_free_pgs`).
4.  For each **leaf page**, iterate through its nodes:
    -   If a node represents a large item (`F_BIGDATA`), retrieve the chain of overflow pages and add them to the free list.
5.  After traversing all pages, add the database's root page itself to the free list.
6.  The database's metadata is then reset to an empty state.

### 4.3. Data Manipulation Functions (`fds_get`, `fds_put`, `fds_del`)

These functions provide the primary API for reading and writing data. They are essentially wrappers around the more powerful and flexible cursor API.

-   [`fds_get(txn, dbi, key, data)`](Lib/db.cpp:439): Initializes a cursor, seeks to the given `key` using `FDS_SET`, and returns the found `data`.
-   [`fds_put(txn, dbi, key, data, flags)`](Lib/db.cpp:399): Initializes a cursor and calls [`fds_cursor_put_impl()`](Lib/cursor.cpp) to perform the write operation, handling flags like `FDS_NOOVERWRITE` and `FDS_APPEND`.
-   [`fds_del(txn, dbi, key, data)`](Lib/db.cpp:320): Initializes a cursor, seeks to the item, and calls [`fds_cursor_del_impl()`](Lib/cursor.cpp) to perform the deletion.

## 5. Code Simplifications and Developer Notes

A developer modifying this code should be aware that it appears to be a simplified version of the standard LMDB library. Key features have been removed or stubbed out:

-   **No Duplicate Support**: The code consistently ignores the `data` parameter in deletion operations and sets the data comparison function `md_dcmp` to `nullptr`. This indicates that support for sorted duplicates (`FDS_DUPSORT`) is not implemented.
-   **No Sub-Databases**: The logic for handling sub-databases (`F_SUBDATA`) is present but appears incomplete or simplified.

These simplifications reduce complexity but also limit functionality. Any modifications or extensions should account for the absence of these features.
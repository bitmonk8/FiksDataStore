# MDB Cursor Technical Documentation

## 1. Overview

The `mdb_cursor` module is a fundamental component of the MDB database engine, providing the primary API for data navigation and manipulation. Cursors are central to all database operations, including reading, writing, and deleting key-value pairs. They encapsulate a specific position within the database's B+ tree structure, enabling efficient traversal and modification.

This document provides a technical description of the cursor implementation, based on the analysis of [`mdb_cursor.h`](Lib/mdb_cursor.h) and [`mdb_cursor.cpp`](Lib/mdb_cursor.cpp). It is intended for developers who need to understand, maintain, or extend the cursor functionality.

**Note:** This version of the MDB implementation has been simplified and notably **lacks support for duplicate keys**. Structures and code paths related to duplicates (`MDB_xcursor`, `MDB_DUPSORT`) have been removed or stubbed out for compatibility, but their functionality is not present.

## 2. Core Data Structures

### `struct MDB_cursor`

The `MDB_cursor` is the principal data structure, representing an active pointer into the database.

```c++
struct MDB_cursor
{
    // Transaction and Database context
    MDB_txn*      mc_txn;
    MDB_dbi       mc_dbi;
    MDB_db*       mc_db;
    MDB_dbx*      mc_dbx;
    unsigned char* mc_dbflag;

    // Cursor linking for the current transaction
    MDB_cursor*   mc_next;
    MDB_cursor*   mc_backup; // Used for shadow cursors

    // B+ Tree Path / Stack
    unsigned short mc_snum;  // Number of pages in the stack
    unsigned short mc_top;   // Index of the current page (top of the stack)
    std::array<MDB_page*, CURSOR_STACK> mc_pg; // Page stack
    std::array<indx_t, CURSOR_STACK>    mc_ki; // Key index stack for each page

    // State Flags
    unsigned int  mc_flags;

    // Compatibility field for removed duplicate support
    MDB_xcursor*  mc_xcursor;
};
```

**Key Fields:**

*   **Transaction and DB Context:** Pointers (`mc_txn`, `mc_db`, etc.) link the cursor to its owning transaction and the specific database it operates on.
*   **Cursor Stack (`mc_pg`, `mc_ki`):** This is the most critical part of the cursor. It stores the path from the B+ tree's root to the cursor's current position. `mc_pg` is an array of page pointers, and `mc_ki` is an array of indices, where `mc_ki[i]` is the index of the key/node on page `mc_pg[i]` that points to the child page `mc_pg[i+1]`. For the leaf page at the top of the stack (`mc_pg[mc_top]`), `mc_ki[mc_top]` is the index of the currently targeted key-value pair.
*   **Flags (`mc_flags`):** These flags track the cursor's state.
    *   `C_INITIALIZED`: The cursor points to a valid position in the database.
    *   `C_EOF`: The cursor is positioned after the last item (for forward iteration) or before the first (for backward iteration).
    *   `C_DEL`: The last operation was a deletion, which affects subsequent `MDB_NEXT` operations.
    *   `C_UNTRACK`: The cursor is dynamically allocated and needs to be tracked by the transaction for proper cleanup and state management during page splits/merges.

## 3. Core Algorithms and Operations

### 3.1. B+ Tree Traversal and Search

Traversal is the foundation of all cursor operations. It involves moving up and down the B+ tree.

*   **`mdb_page_search(MDB_cursor* mc, MDB_val* key, int flags)`:** (Located in `mdb_page.cpp`) This function is the entry point for finding a key. It starts from the database root and descends the B+ tree, pushing each page onto the cursor's stack (`mdb_cursor_push`) until a leaf page is reached.
*   **`mdb_node_search(MDB_cursor* mc, MDB_val* key, int* exactp)`:** Once on a target page (leaf or branch), this function performs a binary search on the nodes (keys) within that page. It uses the database's comparison function (`mc_dbx->md_cmp`) to locate the exact key or the smallest key greater than the target. It updates `mc->mc_ki[mc->mc_top]` with the resulting index.
*   **`mdb_cursor_sibling(MDB_cursor* mc, int move_right)`:** This function enables horizontal movement across the tree at the leaf level, which is essential for `MDB_NEXT` and `MDB_PREV`. It pops the current leaf page from the stack, moves to the next/previous index in the parent branch page, and then pushes the corresponding sibling leaf page onto the stack.

### 3.2. Cursor Positioning (`mdb_cursor_get`)

The `mdb_cursor_get` function acts as a dispatcher for all read/positioning operations.

*   **`MDB_SET`, `MDB_SET_KEY`, `MDB_SET_RANGE`:** These operations use `mdb_cursor_set` to position the cursor on a specific key. The function first performs a fast check on the current page. If the key is not on the current page, it calls `mdb_page_search` to traverse the tree from the root, followed by `mdb_node_search` on the resulting leaf page.
*   **`MDB_FIRST` / `MDB_LAST`:** These use `mdb_page_search` with special flags (`MDB_PS_FIRST`/`MDB_PS_LAST`) to navigate to the left-most or right-most leaf page, respectively. The cursor is then positioned at the first or last item on that page.
*   **`MDB_NEXT` / `MDB_PREV`:** These operations provide sequential access.
    1.  They first attempt to simply increment or decrement the key index (`mc_ki`) on the current page.
    2.  If the cursor moves past the edge of the current page, `mdb_cursor_sibling` is called to find the adjacent leaf page.
    3.  The cursor is then positioned at the first key of the new page (for `MDB_NEXT`) or the last key (for `MDB_PREV`).

### 3.3. Data Modification (Write Operations)

Write operations are significantly more complex as they can modify the structure of the B+ tree. All write operations are performed within a write transaction.

*   **`mdb_cursor_touch(MDB_cursor* mc)`:** Before any modification, this function is called to ensure all pages in the cursor's stack are writable within the current transaction. It requests a mutable copy of each page, marking them as "dirty." This is a key part of the engine's MVCC mechanism.

*   **`mdb_cursor_put_impl(MDB_cursor* mc, MDB_val* key, MDB_val* data, unsigned int flags)`:** This is the core write function.
    1.  **Positioning:** It first positions the cursor using logic similar to `mdb_cursor_set`. It handles flags like `MDB_NOOVERWRITE` (fail if key exists) and `MDB_APPEND` (a fast path for adding a key greater than all existing keys).
    2.  **Spilling:** It calls `mdb_page_spill` to ensure there is enough space in the transaction's dirty page list to accommodate the new data.
    3.  **Touching:** It calls `mdb_cursor_touch` to get writable pages.
    4.  **Overwrite vs. Insert:**
        *   If the key already exists, it handles the overwrite logic. For large data items (`F_BIGDATA`), this may involve freeing old overflow pages and allocating new ones.
        *   If the key is new, it proceeds with insertion.
    5.  **Node Addition:** It calculates the required size for the new node.
        *   If there is enough space on the current leaf page, `mdb_node_add` is called to insert the new key-value pair.
        *   If the page is full, `mdb_page_split` is called. This is a complex operation (in `mdb_page.cpp`) that allocates a new page, moves half the nodes to it, and inserts a new key/pointer into the parent branch page to reference the new sibling. This can cause splits to propagate up the tree.
    6.  **Cursor Updates:** After a node is added, the function iterates through all other cursors in the same transaction and updates their key indices if they point to the same page and are affected by the insertion.

*   **`mdb_cursor_del_impl(MDB_cursor* mc, unsigned int flags)`:** This function handles deletion.
    1.  **Positioning & Touching:** It ensures the cursor is on a valid, writable item.
    2.  **Overflow Pages:** If the item being deleted uses overflow pages (`F_BIGDATA`), it calls `mdb_ovpage_free` to add them to the transaction's free list.
    3.  **Node Deletion:** It calls `mdb_node_del` to remove the node from the leaf page.
    4.  **Cursor Updates:** It updates other cursors pointing to the same page, decrementing their key indices or marking them as deleted (`C_DEL`).
    5.  **Rebalancing:** It calls `mdb_rebalance` (in `mdb_page.cpp`). If deleting the node caused the page to become less than half full, this function will merge the page with a sibling, potentially causing merges to propagate up the tree and reducing its depth.

## 4. How to Use (Developer's Guide)

1.  **Obtain a Cursor:** Always obtain a cursor within an active transaction using `mdb_cursor_open()`.
2.  **Position the Cursor:** Use `mdb_cursor_get()` with an appropriate `MDB_cursor_op` (e.g., `MDB_SET`, `MDB_FIRST`, `MDB_NEXT`) to position the cursor. The key and data values are returned via pointers.
3.  **Perform Write Operations:** For write transactions, use `mdb_cursor_put()` to write data or `mdb_cursor_del()` to delete data at the cursor's current position.
4.  **Close the Cursor:** When finished, release the cursor with `mdb_cursor_close()`. This is crucial in write transactions to un-track the cursor.
5.  **Renewing Cursors:** For read-only transactions, a cursor can be efficiently reused with `mdb_cursor_renew()` after the transaction has been renewed.

A developer modifying this code must have a solid understanding of B+ tree algorithms, especially splitting and merging logic. The interaction between the cursor's state (the stack) and the physical page modifications (`mdb_page_split`, `mdb_rebalance`) is the most critical and complex aspect of the implementation.
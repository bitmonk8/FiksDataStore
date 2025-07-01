# MDB Cursor Technical Documentation

## 1. Overview

The `cursor` module is a fundamental component of the MDB database engine, providing the primary API for data navigation and manipulation. Cursors are central to all database operations, including reading, writing, and deleting key-value pairs. They encapsulate a specific position within the database's B+ tree structure, enabling efficient traversal and modification.

This document provides a technical description of the cursor implementation, based on the analysis of [`cursor.h`](Lib/cursor.h) and [`cursor.cpp`](Lib/cursor.cpp). It is intended for developers who need to understand, maintain, or extend the cursor functionality.

## 2. Core Data Structures

### `struct FDS_cursor`

The `FDS_cursor` is the principal data structure, representing an active pointer into the database.

```c++
struct FDS_cursor
{
    // Transaction and Database context
    FDS_txn*      mc_txn;
    FDS_dbi       mc_dbi;
    FDS_db*       mc_db;
    FDS_dbx*      mc_dbx;
    unsigned char* mc_dbflag;

    // Cursor linking for the current transaction
    FDS_cursor*   mc_next;
    FDS_cursor*   mc_backup; // Used for shadow cursors

    // B+ Tree Path / Stack
    unsigned short mc_snum;  // Number of pages in the stack
    unsigned short mc_top;   // Index of the current page (top of the stack)
    std::array<FDS_page*, CURSOR_STACK> mc_pg; // Page stack
    std::array<indx_t, CURSOR_STACK>    mc_ki; // Key index stack for each page

    // State Flags
    unsigned int  mc_flags;
};
```

**Key Fields:**

*   **Transaction and DB Context:** Pointers (`mc_txn`, `mc_db`, etc.) link the cursor to its owning transaction and the specific database it operates on.
*   **Cursor Stack (`mc_pg`, `mc_ki`):** This is the most critical part of the cursor. It stores the path from the B+ tree's root to the cursor's current position. `mc_pg` is an array of page pointers, and `mc_ki` is an array of indices, where `mc_ki[i]` is the index of the key/node on page `mc_pg[i]` that points to the child page `mc_pg[i+1]`. For the leaf page at the top of the stack (`mc_pg[mc_top]`), `mc_ki[mc_top]` is the index of the currently targeted key-value pair.
*   **Flags (`mc_flags`):** These flags track the cursor's state.
    *   `C_INITIALIZED`: The cursor points to a valid position in the database.
    *   `C_EOF`: The cursor is positioned after the last item (for forward iteration) or before the first (for backward iteration).
    *   `C_DEL`: The last operation was a deletion, which affects subsequent `FDS_NEXT` operations.
    *   `C_UNTRACK`: The cursor is dynamically allocated and needs to be tracked by the transaction for proper cleanup and state management during page splits/merges.

## 3. Core Algorithms and Operations

### 3.1. B+ Tree Traversal and Search

Traversal is the foundation of all cursor operations. It involves moving up and down the B+ tree.

*   **`fds_page_search(FDS_cursor* mc, FDS_val* key, int flags)`:** (Located in `fds_page.cpp`) This function is the entry point for finding a key. It starts from the database root and descends the B+ tree, pushing each page onto the cursor's stack (`fds_cursor_push`) until a leaf page is reached.
*   **`fds_node_search(FDS_cursor* mc, FDS_val* key, int* exactp)`:** Once on a target page (leaf or branch), this function performs a binary search on the nodes (keys) within that page. It uses the database's comparison function (`mc_dbx->md_cmp`) to locate the exact key or the smallest key greater than the target. It updates `mc->mc_ki[mc->mc_top]` with the resulting index.
*   **`fds_cursor_sibling(FDS_cursor* mc, int move_right)`:** This function enables horizontal movement across the tree at the leaf level, which is essential for `FDS_NEXT` and `FDS_PREV`. It pops the current leaf page from the stack, moves to the next/previous index in the parent branch page, and then pushes the corresponding sibling leaf page onto the stack.

### 3.2. Cursor Positioning (`fds_cursor_get`)

The `fds_cursor_get` function acts as a dispatcher for all read/positioning operations.

*   **`FDS_SET`, `FDS_SET_KEY`, `FDS_SET_RANGE`:** These operations use `fds_cursor_set` to position the cursor on a specific key. The function first performs a fast check on the current page. If the key is not on the current page, it calls `fds_page_search` to traverse the tree from the root, followed by `fds_node_search` on the resulting leaf page.
*   **`FDS_FIRST` / `FDS_LAST`:** These use `fds_page_search` with special flags (`FDS_PS_FIRST`/`FDS_PS_LAST`) to navigate to the left-most or right-most leaf page, respectively. The cursor is then positioned at the first or last item on that page.
*   **`FDS_NEXT` / `FDS_PREV`:** These operations provide sequential access.
    1.  They first attempt to simply increment or decrement the key index (`mc_ki`) on the current page.
    2.  If the cursor moves past the edge of the current page, `fds_cursor_sibling` is called to find the adjacent leaf page.
    3.  The cursor is then positioned at the first key of the new page (for `FDS_NEXT`) or the last key (for `FDS_PREV`).

### 3.3. Data Modification (Write Operations)

Write operations are significantly more complex as they can modify the structure of the B+ tree. All write operations are performed within a write transaction.

*   **`fds_cursor_touch(FDS_cursor* mc)`:** Before any modification, this function is called to ensure all pages in the cursor's stack are writable within the current transaction. It requests a mutable copy of each page, marking them as "dirty." This is a key part of the engine's MVCC mechanism.

*   **`fds_cursor_put_impl(FDS_cursor* mc, FDS_val* key, FDS_val* data, unsigned int flags)`:** This is the core write function.
    1.  **Positioning:** It first positions the cursor using logic similar to `fds_cursor_set`. It handles flags like `FDS_NOOVERWRITE` (fail if key exists) and `FDS_APPEND` (a fast path for adding a key greater than all existing keys).
    2.  **Spilling:** It calls `fds_page_spill` to ensure there is enough space in the transaction's dirty page list to accommodate the new data.
    3.  **Touching:** It calls `fds_cursor_touch` to get writable pages.
    4.  **Overwrite vs. Insert:**
        *   If the key already exists, it handles the overwrite logic. For large data items (`F_BIGDATA`), this may involve freeing old overflow pages and allocating new ones.
        *   If the key is new, it proceeds with insertion.
    5.  **Node Addition:** It calculates the required size for the new node.
        *   If there is enough space on the current leaf page, `fds_node_add` is called to insert the new key-value pair.
        *   If the page is full, `fds_page_split` is called. This is a complex operation (in `fds_page.cpp`) that allocates a new page, moves half the nodes to it, and inserts a new key/pointer into the parent branch page to reference the new sibling. This can cause splits to propagate up the tree.
    6.  **Cursor Updates:** After a node is added, the function iterates through all other cursors in the same transaction and updates their key indices if they point to the same page and are affected by the insertion.

*   **`fds_cursor_del_impl(FDS_cursor* mc, unsigned int flags)`:** This function handles deletion.
    1.  **Positioning & Touching:** It ensures the cursor is on a valid, writable item.
    2.  **Overflow Pages:** If the item being deleted uses overflow pages (`F_BIGDATA`), it calls `fds_ovpage_free` to add them to the transaction's free list.
    3.  **Node Deletion:** It calls `fds_node_del` to remove the node from the leaf page.
    4.  **Cursor Updates:** It updates other cursors pointing to the same page, decrementing their key indices or marking them as deleted (`C_DEL`).
    5.  **Rebalancing:** It calls `fds_rebalance` (in `fds_page.cpp`). If deleting the node caused the page to become less than half full, this function will merge the page with a sibling, potentially causing merges to propagate up the tree and reducing its depth.

## 4. How to Use (Developer's Guide)

1.  **Obtain a Cursor:** Always obtain a cursor within an active transaction using `fds_cursor_open()`.
2.  **Position the Cursor:** Use `fds_cursor_get()` with an appropriate `FDS_cursor_op` (e.g., `FDS_SET`, `FDS_FIRST`, `FDS_NEXT`) to position the cursor. The key and data values are returned via pointers.
3.  **Perform Write Operations:** For write transactions, use `fds_cursor_put()` to write data or `fds_cursor_del()` to delete data at the cursor's current position.
4.  **Close the Cursor:** When finished, release the cursor with `fds_cursor_close()`. This is crucial in write transactions to un-track the cursor.
5.  **Renewing Cursors:** For read-only transactions, a cursor can be efficiently reused with `fds_cursor_renew()` after the transaction has been renewed.

A developer modifying this code must have a solid understanding of B+ tree algorithms, especially splitting and merging logic. The interaction between the cursor's state (the stack) and the physical page modifications (`fds_page_split`, `fds_rebalance`) is the most critical and complex aspect of the implementation.
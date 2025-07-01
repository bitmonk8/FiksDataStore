# MDB B+ Tree Logic (`btree.h`, `btree.cpp`)

## 1. Overview

This module encapsulates the logical operations for MDB's B+ tree implementation. It is responsible for navigating the tree, searching for keys, and modifying the tree structure through operations like node splitting, merging, and rebalancing. This module relies on the `page_io` module for all low-level page allocation, deallocation, and disk I/O.

## 2. Core Responsibilities

*   **Tree Traversal:** Searching for keys within the B+ tree.
*   **Node Operations:** Adding, deleting, and reading key/value pairs (nodes) on pages.
*   **Structural Modifications:**
    *   **Splitting Pages:** Handling page overflows by splitting a page into two and updating parent pointers.
    *   **Merging Pages:** Combining underfilled pages to maintain tree balance and reclaim space.
    *   **Rebalancing:** Orchestrating splits and merges to ensure the B+ tree remains balanced after insertions and deletions.
*   **Copy-on-Write (CoW) Orchestration:** Utilizing the `mdb_page_touch()` mechanism (which in turn uses `mdb_page_io`) to ensure that modifications create new page versions, preserving MVCC.

## 3. Key Functions

*   `mdb_page_search()`: Main function to search for a key in the B+ tree.
*   `mdb_page_search_root()`: Core recursive search logic.
*   `mdb_node_search()`: Finds a specific node on a page.
*   `mdb_node_add()`: Adds a new key/value node to a page.
*   `mdb_node_del()`: Deletes a node from a page.
*   `mdb_node_move()`: Moves a node between sibling pages during rebalancing.
*   `mdb_page_split()`: Splits an overfull page.
*   `mdb_page_merge()`: Merges two underfull pages.
*   `mdb_rebalance()`: Orchestrates page merging or node borrowing to maintain tree balance.
*   `mdb_page_touch()`: Core CoW function; ensures a page is writable by creating a dirty copy (delegates actual allocation to `mdb_page_io`).
*   `mdb_page_new()`: Allocates and initializes a new typed page (leaf, branch, overflow) using `mdb_page_io` for allocation.
*   `mdb_leaf_size()` / `mdb_branch_size()`: Calculate node sizes.
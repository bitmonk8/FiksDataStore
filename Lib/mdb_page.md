# MDB Page Structures and Core Definitions (`mdb_page.h`)

## 1. Overview

The `mdb_page.h` header file now primarily serves as the central definition point for core data structures related to database pages, specifically `MDB_page` and `MDB_node`, along with their associated macros and type definitions.

The functional aspects of page management have been refactored into two separate modules:

*   **Low-Level Page I/O & Memory:** See [`Lib/mdb_page_io.md`](Lib/mdb_page_io.md) (and `Lib/mdb_page_io.h`, `Lib/mdb_page_io.cpp`)
*   **B+ Tree Logical Operations:** See [`Lib/mdb_btree.md`](Lib/mdb_btree.md) (and `Lib/mdb_btree.h`, `Lib/mdb_btree.cpp`)

This file, `mdb_page.h`, should be included by any module that needs to understand or interact with the in-memory representation of database pages and their constituent nodes.

## 2. Core Data Structures Retained in `mdb_page.h`

*   **`struct MDB_page`**: Defines the layout of a database page, including its header, flags, and pointers to nodes.
*   **`struct MDB_node`**: Defines the layout of a key/value node within a page.
*   **Associated Macros**: Various macros for accessing fields within `MDB_page` and `MDB_node` (e.g., `MP_FLAGS`, `NUMKEYS`, `NODEPTR`, `NODEKEY`, `NODEDATA`), page type flags (e.g., `P_LEAF`, `P_BRANCH`, `P_OVERFLOW`), and node flags (e.g., `F_BIGDATA`, `F_SUBDATA`).

Refer to the source code in `Lib/mdb_page.h` for the precise definitions.
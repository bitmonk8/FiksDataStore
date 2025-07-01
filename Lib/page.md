# MDB Page Structures and Core Definitions (`fds_page.h`)

## 1. Overview

The `fds_page.h` header file now primarily serves as the central definition point for core data structures related to database pages, specifically `FDS_page` and `FDS_node`, along with their associated macros and type definitions.

The functional aspects of page management have been refactored into two separate modules:

*   **Low-Level Page I/O & Memory:** See [`Lib/page_io.md`](Lib/page_io.md) (and `Lib/page_io.h`, `Lib/page_io.cpp`)
*   **B+ Tree Logical Operations:** See [`Lib/btree.md`](Lib/btree.md) (and `Lib/btree.h`, `Lib/btree.cpp`)

This file, `page.h`, should be included by any module that needs to understand or interact with the in-memory representation of database pages and their constituent nodes.

## 2. Core Data Structures Retained in `fds_page.h`

*   **`struct FDS_page`**: Defines the layout of a database page, including its header, flags, and pointers to nodes.
*   **`struct FDS_node`**: Defines the layout of a key/value node within a page.
*   **Associated Macros**: Various macros for accessing fields within `FDS_page` and `FDS_node` (e.g., `MP_FLAGS`, `NUMKEYS`, `NODEPTR`, `NODEKEY`, `NODEDATA`), page type flags (e.g., `P_LEAF`, `P_BRANCH`, `P_OVERFLOW`), and node flags (e.g., `F_BIGDATA`, `F_SUBDATA`).

Refer to the source code in `Lib/page.h` for the precise definitions.
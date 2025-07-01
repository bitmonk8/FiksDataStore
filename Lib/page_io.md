# MDB Page I/O & Memory (`page_io.h`, `page_io.cpp`)

## 1. Overview

This module is responsible for the low-level "physical" aspects of database page management in MDB. It handles memory allocation for pages, reading from and writing pages to disk, managing free lists, and tracking page states (e.g., dirty, spilled). It provides the foundational operations upon which the logical B+ tree structures are built.

## 2. Core Responsibilities

*   **Page Allocation:** Allocating new pages from the freelist or by extending the database file.
*   **Page Deallocation:** Returning freed pages to the system or internal freelists.
*   **Page I/O:** Reading pages from disk into memory and flushing modified (dirty) pages back to disk.
*   **State Management:** Tracking whether pages are dirty, have been spilled to disk during a long transaction, or are part of an overflow chain.
*   **Concurrency Primitives:** Interacting with transaction structures to ensure correct page visibility and CoW (Copy-on-Write) behavior at the physical level.

## 3. Key Functions

*   `fds_page_alloc()`: Allocates one or more pages for a transaction.
*   `fds_page_free()` / `fds_dpage_free()`: Frees a clean or dirty page.
*   `fds_ovpage_free()`: Frees a sequence of overflow pages.
*   `fds_page_get()`: Retrieves a specific page by its page number, handling dirty pages from the current or parent transactions.
*   `fds_page_flush()`: Writes all dirty pages for a transaction to disk.
*   `fds_page_spill()` / `fds_page_unspill()`: Manages the spilling of dirty pages to disk during long transactions to save memory, and their retrieval if needed again.
*   `fds_page_dirty()`: Marks a page as dirty within a transaction.
*   `fds_page_malloc()`: Low-level memory allocation for page structures.
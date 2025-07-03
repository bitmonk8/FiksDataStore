# MIDL (Memory ID List) Technical Documentation

## 1. Overview

The `midl.h` and `midl.cpp` files provide a specialized set of data structures and functions for managing sorted lists of unsigned integer IDs within the FiksDataStore library. The name "MIDL" stands for "Memory ID List." This component was originally part of OpenLDAP's `back-bdb` backend and has been adapted for internal use in `libmdb`.

The primary purpose of MIDL is to efficiently handle lists of page numbers, transaction IDs, and other identifiers that are crucial for the database's operation. It is designed for high performance, with optimized algorithms for searching, sorting, and merging ID lists.

## 2. Core Data Structures

MIDL defines two main data structures for managing lists of IDs: `FDS_IDL` for simple ID lists and `FDS_ID2L` for lists of ID-pointer pairs.

### 2.1. `FDS_IDL`: The ID List

An `FDS_IDL` is a sorted array of `FDS_ID`s (which is an alias for `size_t`). It is the fundamental structure for managing lists of numerical identifiers.

#### Memory Layout

The memory layout of an `FDS_IDL` is a key aspect of its design, enabling efficient dynamic resizing and access. The pointer to an `FDS_IDL` actually points to the first element of the ID array, but metadata is stored at negative and zero indices relative to this pointer.

-   `ids[-1]`: Stores the total allocated capacity of the array (i.e., the maximum number of IDs it can hold). This is used by functions like [`midl_need()`](Lib/midl.cpp:154) to determine if resizing is necessary.
-   `ids[0]`: Stores the current number of IDs in the list.
-   `ids[1]` to `ids[ids[0]]`: Contains the actual IDs. For `libmdb`, these are sorted in **descending order**.

This layout allows functions to access both the count and capacity of the list with simple pointer arithmetic, without needing to pass them as separate parameters.

### 2.2. `FDS_ID2L`: The ID-Pointer List

An `FDS_ID2L` is a sorted array of `FDS_ID2` structures. Each `FDS_ID2` is a pair containing an `FDS_ID` and a `void*` pointer, used for mapping an ID to a memory location or another data structure.

#### Memory Layout

The `FDS_ID2L` follows a similar design pattern to `FDS_IDL`, but since it's an array of structs, the metadata is stored in the first element of the array.

-   `ids[0].mid`: Stores the current number of `FDS_ID2` pairs in the list.
-   `ids[0].mptr`: This field is unused.
-   `ids[1]` to `ids[ids[0].mid]`: Contains the `FDS_ID2` pairs, sorted in **ascending order** by the `mid` field.

## 3. Key Algorithms and Operations

The `midl.cpp` file implements several important algorithms for managing these ID lists efficiently.

### 3.1. Search

-   **`midl_search(ids, id)`**: Performs a binary search on an `FDS_IDL` to find a specific `id`. It returns the index of the ID if found. If not found, it returns the index where the ID should be inserted to maintain the sort order. The complexity is O(log n).
-   **`mid2l_search(ids, id)`**: Performs a binary search on an `FDS_ID2L` based on the `mid` field of the `FDS_ID2` structs. It has the same characteristics as `midl_search`.

### 3.2. Sorting

-   **`midl_sort(ids)`**: Sorts an `FDS_IDL` in descending order. The implementation is a hybrid of Quicksort and Insertion Sort.
    -   **Quicksort**: Used for larger portions of the array. It employs a median-of-three pivot selection strategy (`ids[l]`, `ids[k]`, `ids[ir]`) to improve performance and avoid worst-case O(n²) behavior on already sorted or reverse-sorted data.
    -   **Insertion Sort**: When a partition in the Quicksort algorithm becomes smaller than a defined threshold (`SMALL = 8`), the algorithm switches to Insertion Sort, which is more efficient for small arrays.

### 3.3. Memory Management and List Manipulation

-   **Allocation and Deallocation**:
    -   [`midl_alloc(num)`](Lib/midl.cpp:108): Allocates memory for an `FDS_IDL` that can hold `num` IDs. It allocates `num + 2` elements to accommodate the capacity at `ids[-1]` and the count at `ids[0]`.
    -   [`midl_free(ids)`](Lib/midl.cpp:119): Frees the memory associated with an `FDS_IDL`.
-   **Dynamic Resizing**:
    -   [`midl_need(idp, num)`](Lib/midl.cpp:154): Checks if an `FDS_IDL` has enough space for `num` additional elements. If not, it reallocates the list, increasing its size with a 25% overhead and aligning the new size to a 256-element boundary for efficiency.
    -   [`midl_shrink(idp)`](Lib/midl.cpp:125): If an IDL has grown beyond a certain threshold (`FDS_IDL_UM_MAX`), this function shrinks it back to a default maximum size.
-   **Append Operations**:
    -   [`midl_append(idp, id)`](Lib/midl.cpp:170): Appends a single ID to an `FDS_IDL`, growing the list if necessary.
    -   [`midl_append_list(idp, app)`](Lib/midl.cpp:185): Appends all IDs from one `FDS_IDL` to another.
    -   [`midl_append_range(idp, id, n)`](Lib/midl.cpp:200): Appends a contiguous range of `n` IDs starting from `id`.
-   **Merging**:
    -   [`midl_xmerge(idl, merge)`](Lib/midl.cpp:218): Merges two sorted `FDS_IDL`s into the destination list (`idl`). The destination list must have enough pre-allocated capacity to hold the combined result. The merge is performed in-place from the end of the arrays backwards.

## 4. Usage in FiksDataStore Context

The MIDL functionality is a critical internal component of FiksDataStore, used for several core database operations:

-   **Free Page Management**: FiksDataStore maintains a list of database pages that have been freed and are available for reuse. This list is managed as an `FDS_IDL`, allowing for efficient tracking and retrieval of free pages.
-   **Transaction Management**: When multiple transactions are active, their IDs and associated data need to be tracked. MIDL structures can be used to manage these lists of transaction IDs.
-   **Index Operations**: In certain scenarios, MIDL can be used to maintain sorted lists of record identifiers that match a particular index query, which can then be merged or filtered.

## 5. Developer's Guide

-   **Internal Use Only**: The functions and data structures in `midl.h` are not part of the public FiksDataStore API and are subject to change. They should not be used directly by applications linking against `libmdb`.
-   **Memory Model**: Understanding the `ids[-1]` and `ids[0]` memory layout is crucial for anyone working on this part of the codebase. All memory management and list manipulation functions rely on this convention.
-   **Sort Order**: Be aware of the different sort orders. `FDS_IDL`s are sorted in **descending** order, while `FDS_ID2L`s are sorted in **ascending** order by ID.
-   **Performance**: The choice of algorithms (binary search, hybrid quicksort) reflects a focus on performance. Changes to these functions should be benchmarked to avoid performance regressions.
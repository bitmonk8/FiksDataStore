# MDB Comparison Functions (`compare.h`, `compare.cpp`)

## 1. Purpose

The `mdb_compare.h` and `mdb_compare.cpp` files provide a suite of comparison functions that are fundamental to the operation of the MDB database. MDB is a B+ tree-based database, and the correct ordering of keys is essential for all its operations, including insertion, deletion, and retrieval.

These files define the default comparison functions used to sort keys within the B+ tree. MDB allows users to specify custom comparison functions on a per-database basis, but the functions in `mdb_compare` serve as the standard, built-in comparators for common data types.

## 2. Core Data Structures

The comparison functions primarily operate on the `MDB_val` struct.

*   **`MDB_val`**: This is the standard structure for representing both keys and data in MDB. It is defined as:
    ```c
    typedef struct MDB_val {
        size_t mv_size; // size of the data item
        void *mv_data; // pointer to the data item
    } MDB_val;
    ```
    All comparison functions take two `const MDB_val*` arguments and return an integer indicating their relative order.

## 3. Main Algorithms and Functions

The comparison functions handle different data types and sorting requirements. The return value convention is standard:
*   `< 0` if `a` is less than `b`.
*   `0` if `a` is equal to `b`.
*   `> 0` if `a` is greater than `b`.

### Lexicographical Comparison

These functions perform byte-wise comparison of data.

*   **`mdb_cmp_memn(const MDB_val* a, const MDB_val* b)`**: This is the standard lexicographical (byte-wise) comparison function.
    *   **Algorithm**:
        1.  It determines the minimum length between the two `MDB_val` items.
        2.  It uses `memcmp` to compare the data up to that minimum length.
        3.  If `memcmp` finds a difference, that result is returned.
        4.  If the common parts are identical, the function compares their lengths. The shorter item is considered smaller.

*   **`mdb_cmp_memnr(const MDB_val* a, const MDB_val* b)`**: This function performs a reverse-order lexicographical comparison.
    *   **Algorithm**:
        1.  It compares the data byte-by-byte, starting from the end of each data buffer and moving towards the beginning.
        2.  If a difference is found, the result is returned.
        3.  If the data is identical, it uses the length difference to break the tie, similar to `mdb_cmp_memn`. This is useful for indexes that need to be sorted in descending order.

### Integer Comparison

These functions are optimized for comparing keys that are integer types. They are generally faster than lexicographical comparison for numeric data because they avoid byte-by-byte loops.

*   **`mdb_cmp_long(const MDB_val* a, const MDB_val* b)`**: Compares keys that are `mdb_size_t`. It assumes the `mv_data` pointers are correctly aligned.

### Dispatch and Helper Macros

*   **`mdb_cmp(MDB_txn* txn, MDB_dbi dbi, const MDB_val* a, const MDB_val* b)`**: This is a dispatch function. It looks up the correct key comparison function associated with the given database handle (`dbi`) within a transaction and calls it. The comparator is stored in `txn->mt_dbxs[dbi].md_cmp`.

## 4. Usage and Developer Guidance

*   **Choosing a Comparator**: For custom sorting logic, a developer can provide their own function pointer using `mdb_set_compare`.

*   **Portability**: The code in `compare.cpp`, demonstrates best practices for writing portable database code by being mindful of data alignment and system endianness. Any custom comparators that deal with multi-byte numeric types should take similar precautions if portability is a concern.
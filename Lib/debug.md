# MDB Debugging Utilities (`debug.h`, `debug.cpp`)

## 1. Purpose

The `debug.h` header and its corresponding implementation `debug.cpp` provide a suite of debugging tools for the MDB library. These utilities are designed for developers working with or on MDB to diagnose issues, inspect the internal state of the database, and validate its integrity.

The entire debugging framework is designed to be conditionally compiled. By default, it is disabled, ensuring that production builds do not carry any of the performance overhead associated with debugging checks and logging. Activation is controlled via preprocessor macros, primarily `FDS_DEBUG` and the standard `NDEBUG`.

## 2. Core Features

### 2.1. Assertions

A context-aware assertion mechanism provides robust runtime checks during development.

*   **Macros:**
    *   `fds_cassert(mc, expr)`: Asserts a condition within a cursor context (`FDS_cursor`).
    *   `fds_tassert(txn, expr)`: Asserts a condition within a transaction context (`FDS_txn`).
*   **Failure Handling:**
    *   When an assertion fails, the `fds_assert_fail()` function is invoked.
    *   It constructs a detailed error message including the failed expression, function name, file, and line number.
    *   If a custom assertion handler has been set on the environment (`FDS_env.me_assert_func`), it is called with the message.
    *   Finally, the message is printed to `stderr`, and the program is terminated via `abort()`.
*   **Activation:** This feature is active only when the `NDEBUG` macro is **not** defined.

### 2.2. Trace Logging

A multi-level trace logging system allows for detailed observation of the library's execution flow.

*   **Control Macro:** `FDS_DEBUG`
    *   `FDS_DEBUG 0` (Default): All logging is compiled out.
    *   `FDS_DEBUG 1`: Enables informational logging via the `DPRINTF` macro.
    *   `FDS_DEBUG 2`: Enables more verbose tracing via the `DPRINTF` macro, which is useful for replaying execution flows.
*   **Logging Macros:**
    *   `DPRINTF((format, ...))`: Prints a standard formatted debug message, prefixed with the function name and line number. Note the double parentheses required for multiple arguments.
    *   `DPRINTF((format, ...))`: Prints a more detailed trace message, prefixed with the process ID and function name.
    *   `DPUTS(string)`: A convenience macro to print a literal string.

### 2.3. Data Inspection

Utilities are provided to format and display internal MDB data structures in a human-readable format.

*   **Key Formatting:**
    *   `DKEY(fds_val)`: A macro that converts a key (`FDS_val`) into a hexadecimal string for printing.
    *   It uses the underlying function `fds_dkey()`, which handles the conversion.
    *   This requires a buffer named `kbuf` to be in scope, which is provided by the `DKBUF` macro.
*   **Page and Node Inspection:**
    *   `fds_page_list(FDS_page *mp)`: Prints a comprehensive summary of a single database page. It identifies the page type (Branch, Leaf, Overflow, Meta), its flags (e.g., `P_DIRTY`), and lists all nodes on the page with their keys and sizes.
    *   `fds_leafnode_type(FDS_node *n)`: Returns a descriptive string for a leaf node's type (e.g., if it contains sub-database data, overflow data, etc.).

### 2.4. Database Auditing

A high-level auditing function is available to perform an integrity check of the database's space management.

*   **Function:** `fds_audit(FDS_txn *txn)`
*   **Algorithm:**
    1.  It calculates the total number of pages marked as free by traversing the freelist database (`FREE_DBI`).
    2.  It iterates through all active databases in the transaction and sums the recorded number of branch, leaf, and overflow pages for each.
    3.  It compares the sum of `(free_pages + used_pages + meta_pages)` against the transaction's `mt_next_pgno` field, which tracks the total number of pages in the file.
    4.  If the counts do not match, it prints a detailed error to `stderr`, indicating a potential inconsistency in page allocation or tracking.

## 3. Data Structures

The debugging utilities do not introduce new primary data structures. Instead, they operate on and inspect the core internal structures of MDB, primarily:

*   `FDS_env`: The environment, which can hold a pointer to a custom assertion handler.
*   `FDS_txn`: The transaction, which provides the context for auditing and assertions.
*   `FDS_page`: A database page, the primary subject of `fds_page_list`.
*   `FDS_node`: A node within a page, representing a key/value entry.
*   `FDS_val`: The standard structure for representing keys and data.

## 4. How to Use

To leverage these debugging tools, a developer would typically perform the following steps:

1.  **Enable Debugging:** In the build configuration or before including MDB headers, define the `FDS_DEBUG` macro to the desired level (1, 2, or 3).
2.  **Disable NDEBUG:** Ensure that the `NDEBUG` macro is not defined to enable assertions.
3.  **Recompile:** Rebuild the MDB library and/or the application using it.
4.  **Instrument Code:**
    *   Insert `DPRINTF` or `DPRINTF` calls at critical points in the code to observe variable states or execution paths.
    *   When holding a pointer to a page (`FDS_page *`), call `fds_page_list()` to dump its contents to `stderr`.
    *   To run the integrity check, call `fds_audit()` on an open transaction.
5.  **Run and Observe:** Execute the program and monitor `stderr` for the debug output.
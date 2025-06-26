# `mdb_util.h` and `mdb_util.cpp`

These files provide various utility functions and macros for the LMDB library.

## `mdb_util.h`

This header file defines a collection of general-purpose macros and platform-specific definitions.

### Macros

*   `F_ISSET(w, f)`: Checks if a specific flag `f` is set in a flag word `w`.
*   `EVEN(n)`: Rounds an unsigned integer `n` up to the nearest even number.
*   `LOW_BIT(n)`: Gets the least significant 1-bit of an unsigned integer `n`.
*   `LOG2_MOD(p2, n)`: A complex macro to calculate `log2(p2) % n` for a power-of-2 `p2` and a small integer `n`.
*   `ALIGNOF2(type)`: Determines the memory alignment of a given `type`, ensuring it's a power of 2.
*   `mdb_strdup`: A platform-independent macro for string duplication, aliasing to `_strdup` on Windows and `strdup` on other systems.

### Constants

*   `MDB_COMMIT_PAGES`: Defines the maximum number of pages to commit in a single `writev()` system call. This value is capped by `IOV_MAX` if it is defined and smaller.

## `mdb_util.cpp`

This implementation file provides functions for versioning and error handling.

### Functions

#### `const char* mdb_version(int* major, int* minor, int* patch)`

This function returns the LMDB library version information.

*   **Parameters:**
    *   `major` (out): If not null, will be filled with the major version number.
    *   `minor` (out): If not null, will be filled with the minor version number.
    *   `patch` (out): If not null, will be filled with the patch version number.
*   **Returns:** A constant string representing the full version of the library (e.g., "0.9.29").

#### `const char* mdb_strerror(int err)`

This function returns a string description for a given LMDB error code.

*   **Parameters:**
    *   `err`: The error code.
*   **Returns:** A string describing the error.
*   **Details:**
    *   It handles standard LMDB error codes by looking them up in an internal `mdb_errstr` table.
    *   For system-dependent error codes, it uses `strerror_s` on Windows and `strerror` on other platforms to get the corresponding system error message.
    *   It includes special handling for common C-runtime error codes on Windows.
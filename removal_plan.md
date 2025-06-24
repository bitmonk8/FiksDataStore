# LMDB Duplicate Key Functionality Removal Plan

## I. Strategy and Dependencies

*   **Goal:** Remove all code related to duplicate keys (`MDB_DUPSORT`, `MDB_DUPFIXED`, etc.) while maintaining a compilable and functional codebase.
*   **Prioritization:**
    1.  Tests
    2.  Implementation details (internal functions, data structures)
    3.  Public API
    4.  Tools
*   **Dependencies:** The public API relies on the core implementation, which in turn uses specific data structures and page types. Tests depend on both the API and the implementation. Tools use the API.

## II. Removal Steps

1.  **Tests:**
    *   Remove test files: [`Tests/mtest3.cpp`](Tests/mtest3.cpp), [`Tests/mtest4.cpp`](Tests/mtest4.cpp), [`Tests/mtest5.cpp`](Tests/mtest5.cpp).
    *   Modify [`Tests/mtest.cpp`](Tests/mtest.cpp) and [`Tests/mtest2.cpp`](Tests/mtest2.cpp) to remove any tests related to duplicate keys.
2.  **Core Data Structures:**
    *   Remove `MDB_xcursor` and related fields from [`Lib/mdb_internal.h`](Lib/mdb_internal.h) and [`Lib/mdb_cursor.h`](Lib/mdb_cursor.h).
    *   Remove any duplicate-specific fields from other shared data structures.
3.  **Page Handling:**
    *   Remove `P_LEAF2` and `P_SUBP` page types from [`Lib/mdb_page.h`](Lib/mdb_page.h) and [`Lib/mdb_page.cpp`](Lib/mdb_page.cpp).
4.  **Implementation Functions:**
    *   Modify functions in [`Lib/mdb_cursor.cpp`](Lib/mdb_cursor.cpp), [`Lib/mdb_db.cpp`](Lib/mdb_db.cpp), [`Lib/mdb_page.cpp`](Lib/mdb_page.cpp), etc. to remove or simplify code related to duplicate keys.
5.  **Public API:**
    *   Remove flags, constants, and cursor operations related to duplicate keys from [`Lib/lmdb.h`](Lib/lmdb.h).
6.  **Tools:**
    *   Update [`Tools/mdb_dump.cpp`](Tools/mdb_dump.cpp) and [`Tools/mdb_load.cpp`](Tools/mdb_load.cpp) to remove any duplicate-specific functionality.

## III. Potential Complications

*   **Functions handling both duplicate and non-duplicate cases:** These functions will need to be modified to handle only the non-duplicate case.
*   **Shared data structures with duplicate-specific fields:** These data structures will need to be modified to remove the duplicate-specific fields.
*   **Error handling code referencing duplicate functionality:** This code will need to be removed or modified.
*   **Performance optimizations depending on duplicate key features:** Since performance is not a major concern, these optimizations can be removed.

## IV. Validation Strategy

*   **Compilation:** Ensure that the code compiles after each step.
*   **Functionality:** After removing each component, test the remaining functionality to ensure that it still works as expected.
*   **Code Review:** Review the code to ensure that no duplicate-related code remains.

## V. Expected Impact

*   **Functionality Loss:** The ability to store duplicate keys will be lost.
*   **API Changes:** Flags, constants, and cursor operations related to duplicate keys will be removed from the public API.
*   **Breaking Changes:** Existing users who rely on the duplicate key functionality will need to modify their code.

## VI. Mermaid Diagram

```mermaid
graph TD
    A[Start] --> B{Remove Test Files};
    B --> C{Remove Core Data Structures};
    C --> D{Remove Page Handling};
    D --> E{Modify Implementation Functions};
    E --> F{Remove Public API};
    F --> G{Update Tools};
    G --> H[Validation];
    H --> I[Complete];
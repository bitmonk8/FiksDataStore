### 1. Main Functionality

The project, **FiksStore**, is a modern C++ fork of the **Lightning Memory-Mapped Database (LMDB)**. As confirmed by the [`README.md`](README.md:3) and [`Lib/lmdb.h`](Lib/lmdb.h:6), it is a high-performance, embedded, transactional key-value store.

Its core purpose is to provide an extremely fast and memory-efficient database by directly mapping the database file into memory. This "zero-copy" architecture means that data is accessed directly from the memory map, eliminating the overhead of copying data between the database and application memory.

The core features, inherited from LMDB, include:

*   **ACID Transactions:** Full atomicity, consistency, isolation, and durability guarantees.
*   **Memory-Mapped I/O:** Data is accessed directly in memory, avoiding system call overhead for read operations.
*   **B+ Tree Data Structure:** Provides efficient key-value storage, retrieval, and sorted iteration.
*   **Multi-Version Concurrency Control (MVCC):** Readers operate on a consistent snapshot of the database and do not block writers, and writers do not block readers.
*   **Crash-Proof Design:** A copy-on-write strategy ensures that data pages are never overwritten, preventing database corruption from application or system crashes.
*   **Single Writer, Multiple Readers:** Allows for concurrent read access from multiple threads/processes, but serializes all write operations.

### 2. Implementation Details

*   **Language:** The project is being actively converted from its original **C** implementation to **Modern C++ (C++17/20/23)**. The codebase consists of `.cpp` and `.h` files, and the project goals explicitly state the intention to use modern C++ features like RAII, smart pointers, templates, and exceptions.
*   **Key Architectural Patterns:**
    *   **Memory-Mapped Files:** The core of the architecture. The entire database is mapped into the process's virtual address space. This is managed primarily within the environment component.
    *   **Copy-on-Write (COW):** When data is modified, a new copy of the affected page is created instead of modifying the existing one. The old page is kept for active read transactions. This is fundamental to providing MVCC and crash safety.
    *   **B+ Tree:** The database is structured as a B+ tree, which is highly efficient for both point queries and range scans. Pages (`FDS_page`) represent the nodes of the tree.
    *   **RAII (Resource Acquisition Is Initialization):** While the current API is still largely C-style, the stated goal is to move towards a C++ API where resources like environments, transactions, and cursors are managed by classes with constructors and destructors.

### 3. Functionality Location

The codebase is organized logically, with core functionalities separated into distinct files.

| Functionality | Files/Directories | Description |
| :--- | :--- | :--- |
| **Environment Management** | [`Lib/fds_env.h`](Lib/fds_env.h), [`Lib/fds_env.cpp`](Lib/fds_env.cpp) | Defines and implements the `FDS_env` structure, which represents the entire database environment. It handles file I/O, memory mapping, and management of the overall database structure. |
| **Transactions** | [`Lib/fds_txn.h`](Lib/fds_txn.h), [`Lib/fds_txn.cpp`](Lib/fds_txn.cpp) | Defines and implements the `FDS_txn` structure. This is the central component for ensuring ACID properties. It tracks read/write operations, manages page allocations, and handles commit/abort logic. |
| **Cursors** | [`Lib/fds_cursor.h`](Lib/fds_cursor.h), [`Lib/fds_cursor.cpp`](Lib/fds_cursor.cpp) | Defines and implements the `FDS_cursor` structure. Cursors are used to navigate the B+ tree, allowing for iteration, searching, and positioning within the database. |
| **Page & Node Management** | [`Lib/fds_page.h`](Lib/fds_page.h), [`Lib/fds_page.cpp`](Lib/fds_page.cpp) | Defines the low-level structures for data storage: `FDS_page` (the nodes of the B+ tree) and `FDS_node` (the key/value entries within a page). This is where the logic for splitting, merging, and searching pages resides. |
| **Database Operations** | [`Lib/fds_db.h`](Lib/fds_db.h), [`Lib/fds_db.cpp`](Lib/fds_db.cpp) | Manages individual databases within an environment. An environment can contain multiple named databases, which are themselves stored as key/value pairs in the main database. |
| **Internal Definitions** | [`Lib/fds_internal.h`](Lib/fds_internal.h) | A central header containing common type definitions, constants, and macros used across the entire library, providing a consistent set of internal APIs. |
| **Tests** | `Tests/` | Contains the test suite for verifying the correctness and performance of the database. |

### 4. Key Concepts for New Developers

A new developer working on this codebase would need to understand the following fundamental concepts:

1.  **Environment (`FDS_env`):** This is the top-level container for the entire database. It corresponds to a physical location on disk (a directory or a pair of files) and manages the memory map. All operations happen within the context of an environment.

2.  **Transactions (`FDS_txn`):** The unit of work in FiksStore. All database operations (reads and writes) must occur within a transaction.
    *   **Read-Only Transactions:** Provide a consistent, isolated view (snapshot) of the database at the time they are created. They are cheap to create and do not block writers.
    *   **Write Transactions:** Are serialized (only one can be active at a time) and are used to modify the database. They see the latest committed state of the database plus their own changes.

3.  **Cursors (`FDS_cursor`):** The primary mechanism for data access. A cursor is a pointer to a specific key/value pair within the database. It is used to seek to a key, retrieve its value, and iterate forwards or backwards through the data, which is kept sorted by key.

4.  **Pages (`FDS_page`):** The database file is a collection of fixed-size pages. These pages are the nodes of the B+ tree.
    *   **Branch Pages:** Internal nodes of the tree that contain keys and pointers to child pages.
    *   **Leaf Pages:** The leaves of the tree that contain the actual keys and their associated values.
    *   **Overflow Pages:** If a value is too large to fit on a single leaf page, it is stored in one or more overflow pages.

5.  **Memory Mapping:** Understanding that the database file is not read in the traditional sense but is mapped directly into memory is crucial. This means that pointers returned by the API (e.g., in an `FDS_val` struct) point directly into the memory map. This data is immutable in read transactions and must not be held onto after the transaction closes, as the underlying memory may be reused by a subsequent write transaction.
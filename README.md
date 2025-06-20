# FiksStore

**A Modern C++ Fork of LMDB (Lightning Memory-Mapped Database)**

[![License](https://img.shields.io/badge/License-OpenLDAP-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C++-Modern-orange.svg)]()
[![Status](https://img.shields.io/badge/Status-In%20Development-yellow.svg)]()

## Overview

FiksStore is a modern C++ reimplementation of the Lightning Memory-Mapped Database (LMDB), originally created by Howard Chu at Symas Corporation. This project aims to modernize the LMDB codebase while maintaining its exceptional performance characteristics and reliability.

## About LMDB

LMDB is a Btree-based database management library modeled loosely on the BerkeleyDB API, but much simplified. The entire database is exposed in a memory map, and all data fetches return data directly from the mapped memory, so no malloc's or memcpy's occur during data fetches. Key features include:

- **Zero-copy architecture**: Data is accessed directly from memory-mapped files
- **ACID compliance**: Full transactional semantics with durability guarantees
- **Multi-version concurrency**: Readers never block writers, writers never block readers
- **Crash-proof**: Copy-on-write strategy prevents corruption
- **Maintenance-free**: No periodic checkpointing or compaction required
- **Cross-platform**: Works on Windows, Linux, macOS, and other Unix systems

## Project Goals

FiksStore aims to modernize LMDB by:

- **Modern C++ Standards**: Converting from C to modern C++ (C++17/20/23)
- **Type Safety**: Leveraging C++ type system for better safety
- **RAII**: Proper resource management with constructors/destructors
- **STL Integration**: Native support for STL containers and algorithms
- **Template-based API**: Generic programming for better performance
- **Exception Safety**: Modern error handling with exceptions
- **Smart Pointers**: Automatic memory management where appropriate
- **Namespace Organization**: Proper C++ namespace structure

## Current Status

🚧 **This project is currently in early development phase** 🚧

The original LMDB C code is being systematically converted to modern C++. The current codebase represents the baseline LMDB 0.9.70 implementation.

### Roadmap

- [ ] **Phase 1**: Core API modernization
  - [ ] Convert core data structures to C++ classes
  - [ ] Implement RAII for resource management
  - [ ] Add proper exception handling
- [ ] **Phase 2**: STL integration
  - [ ] STL-compatible iterators
  - [ ] Container-like interfaces
  - [ ] Algorithm compatibility
- [ ] **Phase 3**: Template-based optimizations
  - [ ] Generic key/value types
  - [ ] Compile-time optimizations
  - [ ] Type-safe database handles
- [ ] **Phase 4**: Advanced features
  - [ ] Async I/O support
  - [ ] Custom allocators
  - [ ] Coroutine integration

## Original LMDB Features

### Performance Characteristics

- **Extremely fast**: Often outperforms other embedded databases
- **Memory efficient**: Minimal memory overhead
- **Scalable**: Handles databases larger than RAM efficiently
- **Concurrent**: Multiple readers, single writer model

### Key Capabilities

- **Memory-mapped I/O**: Direct access to data without copying
- **B+ tree storage**: Efficient key-value storage and retrieval
- **Transactions**: Full ACID properties with nested transaction support
- **Multiple databases**: Single environment can host multiple named databases
- **Duplicate keys**: Optional support for multiple values per key
- **Cursor operations**: Efficient iteration and range queries

## Building

### Prerequisites

- Modern C++ compiler (GCC 9+, Clang 10+, MSVC 2019+)
- XMake 2.5+
- Git

### Current Build

The project uses XMake as the build system:

```bash
# Build the project
xmake

# Build and run tests
xmake build tests
xmake run tests

# Clean build artifacts
xmake clean
```

### Configuration

```bash
# Configure build options
xmake config --mode=debug    # Debug build
xmake config --mode=release  # Release build

# Show configuration
xmake show
```

## Usage Example

### Current API (C-style)

```c
#include "lmdb.h"

MDB_env *env;
MDB_dbi dbi;
MDB_val key, data;
MDB_txn *txn;

// Create environment
mdb_env_create(&env);
mdb_env_open(env, "./testdb", 0, 0664);

// Open database
mdb_txn_begin(env, NULL, 0, &txn);
mdb_dbi_open(txn, NULL, 0, &dbi);

// Store data
key.mv_size = sizeof(int);
key.mv_data = &some_key;
data.mv_size = sizeof(some_data);
data.mv_data = &some_data;
mdb_put(txn, dbi, &key, &data, 0);

mdb_txn_commit(txn);
mdb_env_close(env);
```

### Planned API (Modern C++)

```cpp
#include <fiksstore/database.hpp>

// Planned modern C++ API
fiksstore::Environment env("./testdb");
auto db = env.open_database();

// RAII transaction
{
    auto txn = env.begin_transaction();
    db.put(txn, 42, "Hello, World!");
    txn.commit();
}

// STL-style iteration
for (const auto& [key, value] : db) {
    std::cout << key << ": " << value << std::endl;
}
```

## Testing

The project includes the original LMDB test suite:

```bash
# Build and run all tests
xmake build tests
xmake run tests

# Run individual tests
xmake run mtest
xmake run mtest2
xmake run mtest3
# ... etc
```

## Tools

FiksStore includes the standard LMDB command-line tools:

- **mdb_stat**: Display database statistics
- **mdb_copy**: Copy/backup databases
- **mdb_dump**: Export database contents
- **mdb_load**: Import database contents
- **mdb_drop**: Delete databases

## Documentation

- [Original LMDB Documentation](http://www.lmdb.tech/doc/)
- [API Reference](http://www.lmdb.tech/doc/group__mdb.html)
- [Getting Started Guide](http://www.lmdb.tech/doc/starting.html)

## Contributing

Contributions are welcome! This project is in active development and we're looking for:

- C++ modernization expertise
- Performance optimization
- Testing and validation
- Documentation improvements

### Development Guidelines

- Follow modern C++ best practices
- Maintain backward compatibility during transition
- Comprehensive testing for all changes
- Clear documentation for new APIs
- **Adhere to [Design Constraints](DESIGN_CONSTRAINTS.md)** - Mandatory design standards for the project
- **Follow [Coding Conventions](CODING_CONVENTIONS.md)** - Mandatory coding style and formatting standards

## License

FiksStore maintains the same licensing as the original LMDB:

- **OpenLDAP Public License 2.8** - See [LICENSE](LICENSE) file
- Copyright 2011-2021 Howard Chu, Symas Corp.
- Additional modernization work: Copyright 2025 FiksStore contributors

## Acknowledgments

- **Howard Chu** and **Symas Corporation** for creating and maintaining LMDB
- The **OpenLDAP Project** for the foundational work
- **Martin Hedenfalk** for the original btree implementation
- The LMDB community for years of testing and feedback

## Related Projects

- [Original LMDB](https://github.com/LMDB/lmdb) - The source project
- [py-lmdb](https://github.com/jnwatson/py-lmdb) - Python bindings
- [lmdb-rs](https://github.com/danburkert/lmdb-rs) - Rust bindings
- [node-lmdb](https://github.com/Venemo/node-lmdb) - Node.js bindings

## Contact

- **Issues**: [GitHub Issues](https://github.com/yourusername/FiksStore/issues)
- **Discussions**: [GitHub Discussions](https://github.com/yourusername/FiksStore/discussions)

---

**Note**: This is a fork and modernization effort. For production use of the stable, battle-tested LMDB, please use the [original LMDB project](https://github.com/LMDB/lmdb).
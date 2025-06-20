# LMDB Design Constraints

This document outlines the mandatory design constraints for the LMDB codebase to ensure consistency, maintainability, and proper dependency management.

## Header File Constraints

### 1. Include Guard Standard
All `Lib/*.h` files MUST use `#pragma once` as their include guard mechanism.

**Rationale**: `#pragma once` is more reliable than traditional include guards, prevents macro name conflicts, and is supported by all modern compilers.

### 2. Internal Header Dependency
All `Lib/*.h` files except `midl.h`, `mdb_internal.h`, and `lmdb.h` MUST include `mdb_internal.h` as their first `#include`.

**Rationale**: Ensures consistent access to internal definitions and maintains proper dependency hierarchy.

**Exceptions**:
- `midl.h`: Standalone utility, should not depend on LMDB internals
- `mdb_internal.h`: Base internal header, cannot include itself
- `lmdb.h`: Public API header, must not include internal headers

### 3. Source File Header Inclusion
All `Lib/*.cpp` files MUST `#include` their corresponding `.h` file as their first `#include`.

**Rationale**: Ensures header files are self-contained and can be compiled independently, catching missing dependencies early.

## Enforcement

These constraints are enforced by the custom Refactor mode in RooCode and should be verified during code reviews.

## Validation

To validate compliance:
1. Check all `.h` files use `#pragma once`
2. Verify `.h` files include `mdb_internal.h` first (except exceptions)
3. Confirm `.cpp` files include their corresponding `.h` first
4. Ensure project compiles and tests pass

## Future Constraints

Additional design constraints may be added to this document as the project evolves.
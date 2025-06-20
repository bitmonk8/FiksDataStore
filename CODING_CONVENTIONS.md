# LMDB Coding Conventions

This document outlines the mandatory coding conventions for the LMDB codebase to ensure consistency, readability, and maintainability across all source files.

## C++ Language Conventions

### 1. Struct Declaration Style
**MANDATORY**: Use modern C++ struct declarations without typedef patterns.

**Rule**: Use `struct Foo {};` instead of the legacy C pattern `typedef struct Foo {} Foo;`.

**Rationale**: The typedef pattern is a legacy C style that is not relevant for C++ projects. Modern C++ allows direct struct declarations without typedef.

**Examples**:
```cpp
// ✅ Correct - Modern C++ style
struct MDB_env
{
    HANDLE me_fd;
    HANDLE me_lfd;
    // ... members
};

// ❌ Incorrect - Legacy C style
typedef struct MDB_env
{
    HANDLE me_fd;
    HANDLE me_lfd;
    // ... members
} MDB_env;
```

### 2. Comment Style
**MANDATORY**: Use clear, human-readable comments to document the code. Avoid using any specific comment tags or annotations like Doxygen.

**Rule**: Focus on writing comments that clearly explain the purpose, logic, and usage of the code. Use single-line `//` comments for most explanations. Reserve C-style multiline `/* */` comments exclusively for temporarily commenting out blocks of code during development and experimentation.

**Rationale**: Human-readable comments are easier to understand and maintain. They allow developers to quickly grasp the intent of the code without needing to parse special tags or annotations. Using `/* */` comments only for temporary code removal prevents conflicts when commenting out large blocks of code.

**Examples**:
```cpp
// ✅ Correct - Human-readable comment
// Initialize the environment
int mdb_env_create(MDB_env **env)
{
    // Allocate memory for the environment
    MDB_env *e = (MDB_env*) calloc(1, sizeof(MDB_env));

    // Set default values
    e->me_maxreaders = DEFAULT_READERS;
    return MDB_SUCCESS;
}

// ✅ Correct - Multiline comments for temporary code removal
int mdb_env_create(MDB_env **env)
{
    /*
    // Temporarily disabled debug code
    printf("Creating environment\n");
    debug_print_state();
    */

    MDB_env *e = (MDB_env*) calloc(1, sizeof(MDB_env));
    return MDB_SUCCESS;
}

// ✅ Correct - No special tags or annotations needed
// This function initializes the environment
int mdb_env_create(MDB_env **env);
```

### 3. Brace Placement
**MANDATORY**: Opening and closing curly brackets for definitions and compound statements must be on separate lines.

**Rule**: Place opening `{` and closing `}` braces on their own lines for function definitions, struct definitions, class definitions, and compound statements (if, for, while, etc.).

**Rationale**: This style improves code readability and makes it easier to match opening and closing braces, especially in complex nested structures.

**Examples**:
```cpp
// ✅ Correct - Braces on separate lines
struct MDB_env
{
    HANDLE me_fd;
    HANDLE me_lfd;
    uint32_t me_flags;
};

int mdb_env_create(MDB_env **env)
{
    MDB_env *e = (MDB_env*) calloc(1, sizeof(MDB_env));
    if (!e)
        return ENOMEM;
    
    for (int i = 0; i < MAX_READERS; i++)
        e->readers[i] = NULL;
    
    *env = e;
    return MDB_SUCCESS;
}

// ❌ Incorrect - Opening braces on same line
struct MDB_env {
    HANDLE me_fd;
    HANDLE me_lfd;
};

int mdb_env_create(MDB_env **env) {
    if (!env) {
        return EINVAL;
    }
    return MDB_SUCCESS;
}
```

## Code Organization

### 1. Header Inclusion Order
**MANDATORY**: Follow the established header inclusion pattern as defined in DESIGN_CONSTRAINTS.md.

**Rules**:
- All `.cpp` files must include their corresponding `.h` file first
- **Exception**: `.cpp` files containing a `main` function do not need a corresponding header file.
- All `Lib/*.h` files (except exceptions) must include `mdb_internal.h` first
- **Exception**: `Lib/midl.h` does not need a to include include `mdb_internal.h`.
- Use `#pragma once` for include guards

## Validation

To validate compliance with coding conventions:
1. Check struct declarations use modern C++ style (no typedef patterns)
2. Verify comment style follows single-line `//` preference and ensure no C style `/* */` comments are used for regular documentation
3. Confirm brace placement follows separate-line rule
4. Ensure consistency with existing codebase patterns
5. Run `xmake test` to verify functionality is preserved

### Comment Style Validation
To check for prohibited C style multiline comments in regular code:
```bash
# Search for C style multiline comments (excluding temporary code blocks)
grep -r "/\*" Lib/ Tools/ Tests/ --include="*.cpp" --include="*.h"
```
Any `/* */` comments found should be converted to single-line `//` comments unless they are clearly used for temporary code commenting during development.

## Future Conventions

Additional coding conventions may be added to this document as the project evolves and new patterns emerge.

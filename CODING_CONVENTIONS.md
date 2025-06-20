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
struct MDB_env {
    HANDLE me_fd;
    HANDLE me_lfd;
    // ... members
};

// ❌ Incorrect - Legacy C style
typedef struct MDB_env {
    HANDLE me_fd;
    HANDLE me_lfd;
    // ... members
} MDB_env;
```

### 2. Comment Style
**MANDATORY**: Use single-line `//` comments for all regular code documentation. C style multiline `/* */` comments are to be avoided at all cost.

**Rule**: Use `//` for all regular comments, documentation, and code explanations. Reserve `/* */` comments exclusively for temporarily commenting out blocks of code during development and experimentation.

**Rationale**: Single-line comments are more readable, easier to maintain, and allow for easier temporary code commenting during development. Since C/C++ does not support nested multiline comments, avoiding `/* */` comments in regular code makes it much easier to temporarily comment out large blocks of code using `/* */` without conflicts. This greatly improves the development and debugging experience.

**Examples**:
```cpp
// ✅ Correct - Single line comments
// This function initializes the environment
int mdb_env_create(MDB_env **env)
{
    // Allocate memory for environment
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

// ❌ Incorrect - Multiline comments for regular documentation
/*
 * This function initializes the environment
 * and sets up default values
 */
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
    {
        return ENOMEM;
    }
    
    for (int i = 0; i < MAX_READERS; i++)
    {
        e->readers[i] = NULL;
    }
    
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

## Documentation Standards

### 1. Function Documentation
**RECOMMENDED**: Use Doxygen-style comments for all public functions.

**Guidelines**:
- Include `@brief` descriptions for all major functions
- Document all parameters with `@param`
- Document return values with `@return`
- Include usage examples for complex functions

**Example**:
```cpp
/** @brief Create an LMDB environment handle.
 * @param[out] env The address where the new handle will be stored
 * @return A non-zero error value on failure and 0 on success.
 */
int mdb_env_create(MDB_env **env);
```

### 2. Inline Comments
**RECOMMENDED**: Use clear, concise inline comments to explain complex logic.

**Guidelines**:
- Explain the "why" not the "what"
- Use comments to clarify non-obvious code sections
- Keep comments up-to-date with code changes

## Code Organization

### 1. Header Inclusion Order
**MANDATORY**: Follow the established header inclusion pattern as defined in DESIGN_CONSTRAINTS.md.

**Rules**:
- All `.cpp` files must include their corresponding `.h` file first
- All `Lib/*.h` files (except exceptions) must include `mdb_internal.h` first
- Use `#pragma once` for include guards

### 2. Variable and Function Naming
**RECOMMENDED**: Follow existing naming conventions in the codebase.

**Guidelines**:
- Use descriptive names for variables and functions
- Follow the existing `mdb_` prefix pattern for public functions
- Use consistent naming patterns within modules

## Memory Management

### 1. Resource Management
**MANDATORY**: Follow LMDB's established memory allocation patterns.

**Guidelines**:
- Ensure proper cleanup and error handling
- Maintain existing memory layout optimizations
- Document any changes to memory usage patterns
- Use consistent error handling patterns

### 2. Error Handling
**MANDATORY**: Maintain consistent error handling throughout the codebase.

**Guidelines**:
- Use established error codes and patterns
- Ensure proper cleanup on error paths
- Document error conditions and recovery strategies

## Performance Considerations

### 1. Code Efficiency
**RECOMMENDED**: Maintain or improve performance characteristics.

**Guidelines**:
- Be mindful of memory allocation patterns
- Consider cache locality and data structure efficiency
- Profile before and after significant changes when relevant
- Optimize only when necessary and measurable

### 2. Thread Safety
**MANDATORY**: Maintain existing thread safety considerations.

**Guidelines**:
- Follow established locking patterns
- Document any changes to thread safety guarantees
- Ensure compatibility with existing concurrency model

## Enforcement

These conventions are enforced by:
1. Code reviews
2. The custom Refactor mode in RooCode
3. Automated testing and validation

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

## Examples from Codebase

The following files demonstrate proper adherence to these conventions:
- [`mdb_env.h`](Lib/mdb_env.h) - Proper struct declarations and brace placement
- [`mdb_env.cpp`](Lib/mdb_env.cpp) - Consistent comment style and formatting
- [`mdb_cursor.h`](Lib/mdb_cursor.h) - Good documentation and structure organization
- [`mdb_cursor.cpp`](Lib/mdb_cursor.cpp) - Proper function formatting and error handling
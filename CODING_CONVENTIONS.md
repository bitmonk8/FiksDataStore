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
struct FDS_env
{
    HANDLE me_fd;
    HANDLE me_lfd;
    // ... members
};

// ❌ Incorrect - Legacy C style
typedef struct FDS_env
{
    HANDLE me_fd;
    HANDLE me_lfd;
    // ... members
} FDS_env;
```

### 2. Comment Style
**MANDATORY**: Use clear, human-readable comments to document the code. Avoid using any specific comment tags or annotations like Doxygen.

**Rule**: Focus on writing comments that clearly explain the purpose, logic, and usage of the code. Use single-line `//` comments for most explanations. Reserve C-style multiline `/* */` comments exclusively for temporarily commenting out blocks of code during development and experimentation.

**Rationale**: Human-readable comments are easier to understand and maintain. They allow developers to quickly grasp the intent of the code without needing to parse special tags or annotations. Using `/* */` comments only for temporary code removal prevents conflicts when commenting out large blocks of code.

**Examples**:
```cpp
// ✅ Correct - Human-readable comment
// Initialize the environment
int fds_env_create(FDS_env **env)
{
    // Allocate memory for the environment
    FDS_env *e = (FDS_env*) calloc(1, sizeof(FDS_env));

    // Set default values
    e->me_maxreaders = DEFAULT_READERS;
    return FDS_SUCCESS;
}

// ✅ Correct - Multiline comments for temporary code removal
int fds_env_create(FDS_env **env)
{
    /*
    // Temporarily disabled debug code
    printf("Creating environment\n");
    debug_print_state();
    */

    FDS_env *e = (FDS_env*) calloc(1, sizeof(FDS_env));
    return FDS_SUCCESS;
}

// ✅ Correct - No special tags or annotations needed
// This function initializes the environment
int fds_env_create(FDS_env **env);
```

### 3. Brace Placement
**MANDATORY**: Opening and closing curly brackets for definitions and compound statements must be on separate lines.

**Rule**: Place opening `{` and closing `}` braces on their own lines for function definitions, struct definitions, class definitions, and compound statements (if, for, while, etc.).

**Rationale**: This style improves code readability and makes it easier to match opening and closing braces, especially in complex nested structures.

**Examples**:
```cpp
// ✅ Correct - Braces on separate lines
struct FDS_env
{
    HANDLE me_fd;
    HANDLE me_lfd;
    uint32_t me_flags;
};

int fds_env_create(FDS_env **env)
{
    FDS_env *e = (FDS_env*) calloc(1, sizeof(FDS_env));
    if (!e)
        return ENOMEM;
    
    for (int i = 0; i < MAX_READERS; i++)
        e->readers[i] = NULL;
    
    *env = e;
    return FDS_SUCCESS;
}

// ❌ Incorrect - Opening braces on same line
struct FDS_env {
    HANDLE me_fd;
    HANDLE me_lfd;
};

int fds_env_create(FDS_env **env) {
    if (!env) {
        return EINVAL;
    }
    return FDS_SUCCESS;
}
```

### 4. Variable Declaration Style
**MANDATORY**: Declare only a single variable per declaration statement.

**Rule**: Each variable declaration must be on its own separate statement. Multiple variable declarations in a single statement are prohibited.

**Rationale**: Single variable declarations improve code readability, make debugging easier by allowing breakpoints on individual variable declarations, and reduce the likelihood of initialization errors. This style also makes it clearer when variables have different types or initialization patterns.

**Examples**:
```cpp
// ✅ Correct - Single variable per declaration
int status;
int count;
FDS_env *env;
FDS_txn *txn;

// ✅ Correct - Each variable clearly initialized
int readers = 0;
int writers = 0;
bool is_valid = false;

// ✅ Correct - Different types clearly separated
size_t data_size = sizeof(FDS_val);
void *data_ptr = nullptr;
uint32_t flags = FDS_RDONLY;

// ❌ Incorrect - Multiple variables in single declaration
int status, count;
FDS_env *env, *backup_env;

// ❌ Incorrect - Mixed initialization patterns
int readers = 0, writers, max_readers = DEFAULT_READERS;

// ❌ Incorrect - Pointer declarations can be confusing
char *buffer, filename[256];  // Only buffer is a pointer!
```

## Variables and Immutability

### Single-Purpose, Single-Assignment Variables
**MANDATORY**: A local (automatic) variable represents exactly one logical value/purpose in its lifetime. Once that value has been defined, the identifier must not be reassigned to hold a different, unrelated value. If a second, independent value is needed, declare a new variable—preferably const—with its own descriptive name.

**Rationale**:
- Improves readability: the meaning of every identifier is stable and obvious
- Reduces bugs: accidental reuse or stale data cannot occur
- Enables more const correctness and compiler optimisations
- Simplifies maintenance and code reviews; you can reason about a variable by reading only its declaration

**Guidelines**:
a. Prefer const for every variable that is not meant to change
b. If an algorithm really requires mutation (e.g. a loop counter), the variable may be reassigned, but its semantic role must remain the same
c. Do not "re-purpose" a variable, even if scopes do not overlap. Instead, create a new identifier with a clear name
d. Nested scopes may shadow an outer const variable with a new one of the same name only if the meaning is identical but a narrower, more precise type/value is needed
e. Temporary throw-away names such as t1, t2, tmp are discouraged; give meaningful identifiers

**Exceptions**:
- Loop indices and accumulators that naturally mutate as part of their single purpose are exempt
- Performance-critical code that demonstrably benefits from reuse may be allowed, but must be documented with a comment and reviewed by a peer

**Enforcement**:
- During code review, flag any variable whose meaning visibly changes after its first assignment
- Static-analysis checks can warn on multiple unrelated writes
- Unit tests must not rely on variable reuse for side effects

**Examples**:
```cpp
// ✅ Correct - Single-purpose variables with const preference
int fds_page_search(FDS_page *page, FDS_val *key)
{
    const int num_keys = NUMKEYS(page);
    const char *base_ptr = NODEPTR(page, 0);
    
    for (int index = 0; index < num_keys; index++)
    {
        const FDS_node *current_node = NODEPTR(page, index);
        const int comparison_result = fds_cmp(key, &current_node->mn_data);
        
        if (comparison_result == 0)
            return index;
    }
    
    return -1;
}

// ✅ Correct - Loop counter mutation is acceptable (single purpose)
int fds_cursor_count(FDS_cursor *cursor)
{
    int total_count = 0;
    
    for (int page_index = 0; page_index < cursor->mc_snum; page_index++)
    {
        const FDS_page *current_page = cursor->mc_pg[page_index];
        total_count += NUMKEYS(current_page);
    }
    
    return total_count;
}

// ❌ Incorrect - Variable repurposed for different meanings
int fds_bad_example(FDS_env *env)
{
    int result = fds_env_open(env, "/tmp/db", 0, 0644);  // result = status code
    if (result != FDS_SUCCESS)
        return result;
    
    result = env->me_maxreaders;  // ❌ Now result = reader count (different purpose!)
    
    if (result > 100)
    {
        result = EINVAL;  // ❌ Now result = error code again (confusing!)
        return result;
    }
    
    return FDS_SUCCESS;
}

// ✅ Correct - Separate variables for different purposes
int fds_good_example(FDS_env *env)
{
    const int open_status = fds_env_open(env, "/tmp/db", 0, 0644);
    if (open_status != FDS_SUCCESS)
        return open_status;
    
    const int max_readers = env->me_maxreaders;
    
    if (max_readers > 100)
        return EINVAL;
    
    return FDS_SUCCESS;
}
```

## Code Organization

### 1. Header Inclusion Order
**MANDATORY**: Follow the established header inclusion pattern as defined in DESIGN_CONSTRAINTS.md.

**Rules**:
- All `.cpp` files must include their corresponding `.h` file first
- **Exception**: `.cpp` files containing a `main` function do not need a corresponding header file.
- All `Lib/*.h` files (except exceptions) must include `fds_internal.h`
- **Exception**: `Lib/midl.h` does not need a to include include `fds_internal.h`.
- Use `#pragma once` for include guards

## Validation

To validate compliance with coding conventions:
1. Check struct declarations use modern C++ style (no typedef patterns)
2. Verify comment style follows single-line `//` preference and ensure no C style `/* */` comments are used for regular documentation
3. Confirm brace placement follows separate-line rule
4. Verify variable declarations use single variable per statement
5. Ensure consistency with existing codebase patterns
6. Run `xmake test` to verify functionality is preserved

### Comment Style Validation
To check for prohibited C style multiline comments in regular code:
```bash
# Search for C style multiline comments (excluding temporary code blocks)
grep -r "/\*" Lib/ Tests/ --include="*.cpp" --include="*.h"
```
Any `/* */` comments found should be converted to single-line `//` comments unless they are clearly used for temporary code commenting during development.

## Future Conventions

Additional coding conventions may be added to this document as the project evolves and new patterns emerge.

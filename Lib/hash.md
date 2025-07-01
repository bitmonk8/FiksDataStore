# MDB Hash Utilities

This document provides a technical overview of the hashing and encoding utilities found in [`hash.h`](hash.h) and [`hash.cpp`](hash.cpp). These utilities are used within the MDB library for generating hash values and for encoding numbers into a compact string representation.

## Purpose

The primary purpose of these files is to provide:
1.  A robust and fast hashing function for arbitrary data.
2.  A specialized function to encode 64-bit integers into a human-readable ASCII string format, used for naming databases.

## Functions

### `mdb_hash`

```cpp
auto mdb_hash(const void* val, size_t len) -> mdb_hash_t;
```

This function computes a 64-bit hash of a given block of data.

#### Algorithm: FNV-1a

The function implements the **Fowler/Noll/Vo (FNV-1a)** hash algorithm. This is a non-cryptographic hash function known for its speed and good distribution properties.

The core of the algorithm is as follows:

1.  Initialize a 64-bit hash value to an initial prime basis: `0xcbf29ce484222325ULL`.
2.  For each byte in the input data, XOR the hash value with the byte's value.
3.  Multiply the result by another prime number: `0x100000001b3ULL`.
4.  Repeat for all bytes in the input data.

The implementation is based on public domain code by Landon Curt Noll.

#### Parameters

-   `val`: A `const void*` pointer to the data that needs to be hashed.
-   `len`: A `size_t` representing the length of the data in bytes.

#### Return Value

-   Returns a `mdb_hash_t` (which is a 64-bit unsigned integer) representing the computed hash value.

### `mdb_pack85`

```cpp
void mdb_pack85(unsigned long long l, char* out);
```

This function encodes a 64-bit unsigned integer into a custom ASCII-85 string representation.

#### Algorithm: Custom Base-85 Encoding

This is a custom implementation and is **not** compliant with standard ASCII-85 or Z85 specifications. It is specifically used for generating printable names for databases from 64-bit values.

The algorithm works by repeatedly taking the input number modulo 85 to select a character from a custom character set, and then dividing the number by 85, until the number becomes zero.

The custom 85-character set is:
`"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~"`

#### Parameters

-   `l`: The `unsigned long long` value to be encoded.
-   `out`: A pointer to a character buffer where the resulting null-terminated string will be stored. The buffer must be large enough to hold the result (at least 11 bytes for a 64-bit number).

## Usage

A developer can use these functions to hash keys for internal data structures or to generate human-readable identifiers from numerical values.

**Hashing Example:**
```cpp
#include "hash.h"
#include <string.h>
#include <stdio.h>

int main() {
    const char* my_key = "some_data_to_hash";
    size_t key_len = strlen(my_key);
    mdb_hash_t hash_value = mdb_hash(my_key, key_len);
    printf("Hash: %llx\n", hash_value);
    return 0;
}
```

**Encoding Example:**
```cpp
#include "hash.h"
#include <stdio.h>

int main() {
    unsigned long long value = 1234567890123456789ULL;
    char buffer[11];
    mdb_pack85(value, buffer);
    printf("Encoded value: %s\n", buffer);
    return 0;
}
```

## Notes for Developers

-   The `mdb_hash` function is suitable for general-purpose hashing but should not be used for cryptographic purposes.
-   The `mdb_pack85` function is highly specific to MDB's internal use case for naming. If a standard Base-85 encoding is required, a different implementation should be used.
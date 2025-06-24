#pragma once

#include "mdb_internal.h"

#include <stddef.h>

mdb_hash_t mdb_hash(const void* val, size_t len);
void mdb_pack85(unsigned long long l, char* out);

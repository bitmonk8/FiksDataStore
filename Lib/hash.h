#pragma once

#include "internal.h"

#include <stddef.h>

auto mdb_hash(const void* val, size_t len) -> mdb_hash_t;
void mdb_pack85(unsigned long long l, char* out);

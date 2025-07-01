#pragma once

#include "internal.h"

#include <stddef.h>

auto fds_hash(const void* val, size_t len) -> fds_hash_t;
void fds_pack85(unsigned long long l, char* out);

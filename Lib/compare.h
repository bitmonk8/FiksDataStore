#pragma once

#include "internal.h"

auto mdb_cmp_memn(const MDB_val* a, const MDB_val* b) -> int;
auto mdb_cmp_memnr(const MDB_val* a, const MDB_val* b) -> int;
auto mdb_cmp_long(const MDB_val* a, const MDB_val* b) -> int;

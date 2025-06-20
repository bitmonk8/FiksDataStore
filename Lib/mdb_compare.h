#pragma once

#include "mdb_internal.h"

int mdb_cmp_memn(const MDB_val *a, const MDB_val *b);
int mdb_cmp_memnr(const MDB_val *a, const MDB_val *b);
int mdb_cmp_int(const MDB_val *a, const MDB_val *b);
int mdb_cmp_cint(const MDB_val *a, const MDB_val *b);
int mdb_cmp_long(const MDB_val *a, const MDB_val *b);

// Compare two items pointing at '#mdb_size_t's of unknown alignment.
#ifdef MISALIGNED_OK
# define mdb_cmp_clong mdb_cmp_long
#else
# define mdb_cmp_clong mdb_cmp_cint
#endif

// True if we need #mdb_cmp_clong() instead of \b cmp for #MDB_INTEGERDUP
#define NEED_CMP_CLONG(cmp, ksize) \
	(UINT_MAX < MDB_SIZE_MAX && \
	 (cmp) == mdb_cmp_int && (ksize) == sizeof(mdb_size_t))

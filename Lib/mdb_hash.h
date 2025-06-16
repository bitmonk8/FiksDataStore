#ifndef MDB_HASH_H
#define MDB_HASH_H

#include <stddef.h> /* for size_t */

typedef unsigned long long	mdb_hash_t;

mdb_hash_t mdb_hash(const void *val, size_t len);
void mdb_pack85(unsigned long long l, char *out);

#endif /* MDB_HASH_H */
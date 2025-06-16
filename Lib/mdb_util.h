#ifndef MDB_UTIL_H
#define MDB_UTIL_H

#include "lmdb.h"

char *mdb_version(int *major, int *minor, int *patch);
char *mdb_strerror(int err);

#endif /* MDB_UTIL_H */
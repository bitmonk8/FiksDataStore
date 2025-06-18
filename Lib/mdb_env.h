#pragma once

#include "mdb_internal.h"

int ESECT mdb_env_share_locks(MDB_env *env, int *excl);
int mdb_env_sync0(MDB_env *env, int force, pgno_t numpgs);

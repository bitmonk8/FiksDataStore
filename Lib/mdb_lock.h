#pragma once

#include "mdb_internal.h"

/** @} */
/** Lock mutex, handle any error, set rc = result.
 *	Return 0 on success, nonzero (not rc) on error.
*/
#define LOCK_MUTEX(rc, env, mutex) \
	(((rc) = LOCK_MUTEX0(mutex)) && \
	 ((rc) = mdb_mutex_failed(env, mutex, rc)))
     
int mdb_mutex_failed(MDB_env *env, mdb_mutexref_t mutex, int rc);

#pragma once

#include "mdb_internal.h"


#if !(MDB_PIDLOCK)		/* Currently the same as defined(_WIN32) */
enum Pidlock_op {
	Pidset, Pidcheck
};
#else
enum Pidlock_op {
	Pidset = F_SETLK, Pidcheck = F_GETLK
};
#endif

/** @} */
/** Lock mutex, handle any error, set rc = result.
 *	Return 0 on success, nonzero (not rc) on error.
*/
#define LOCK_MUTEX(rc, env, mutex) \
	(((rc) = LOCK_MUTEX0(mutex)) && \
	 ((rc) = mdb_mutex_failed(env, mutex, rc)))
    
int mdb_mutex_failed(MDB_env *env, mdb_mutexref_t mutex, int rc);
int mdb_reader_pid(MDB_env *env, enum Pidlock_op op, MDB_PID_T pid);

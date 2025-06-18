#pragma once

#include "mdb_internal.h"

/** assert(3) variant in cursor context */
#define mdb_cassert(mc, expr)	mdb_assert0((mc)->mc_txn->mt_env, expr, #expr)
/** assert(3) variant in transaction context */
#define mdb_tassert(txn, expr)	mdb_assert0((txn)->mt_env, expr, #expr)

#ifndef NDEBUG
#define mdb_assert0(env, expr, expr_txt) ((expr) ? (void)0 : \
		mdb_assert_fail(env, expr_txt, __func__, __FILE__, __LINE__))
void ESECT mdb_assert_fail(MDB_env *env, const char *expr_txt, const char *func, const char *file, int line);
#else
#define mdb_assert0(env, expr, expr_txt) ((void) 0)
#endif /* NDEBUG */

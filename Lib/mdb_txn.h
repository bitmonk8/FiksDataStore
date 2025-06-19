#pragma once

#include "mdb_internal.h"

enum {
	/* mdb_txn_end operation number, for logging */
	MDB_END_COMMITTED, MDB_END_EMPTY_COMMIT, MDB_END_ABORT, MDB_END_RESET,
	MDB_END_RESET_TMP, MDB_END_FAIL_BEGIN, MDB_END_FAIL_BEGINCHILD
};
#define MDB_END_OPMASK	0x0F	/**< mask for #mdb_txn_end() operation number */
#define MDB_END_UPDATE	0x10	/**< update env state (DBIs) */
#define MDB_END_FREE	0x20	/**< free txn unless it is #MDB_env.%me_txn0 */
#define MDB_END_SLOT MDB_NOTLS	/**< release any reader slot if #MDB_NOTLS */
void mdb_txn_end(MDB_txn *txn, unsigned mode);

void _mdb_txn_abort(MDB_txn *txn);
int mdb_txn_renew0(MDB_txn *txn);

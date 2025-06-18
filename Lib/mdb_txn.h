#pragma once

#include "mdb_internal.h"

void _mdb_txn_abort(MDB_txn *txn);
int mdb_txn_renew0(MDB_txn *txn);

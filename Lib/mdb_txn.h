#pragma once

#include "mdb_internal.h"

// A database transaction.
// Every operation requires a transaction handle.
struct MDB_txn {
	MDB_txn		*mt_parent;		// parent of a nested txn
	// Nested txn under this txn, set together with flag MDB_TXN_HAS_CHILD
	MDB_txn		*mt_child;
	pgno_t		mt_next_pgno;	// next unallocated page
	// The ID of this transaction. IDs are integers incrementing from 1.
	// Only committed write transactions increment the ID. If a transaction
	// aborts, the ID may be re-used by the next writer.
	txnid_t		mt_txnid;
	MDB_env		*mt_env;		// the DB environment
	// The list of pages that became unused during this transaction.
	MDB_IDL		mt_free_pgs;
	// The list of loose pages that became unused and may be reused
	// in this transaction, linked through NEXT_LOOSE_PAGE(page).
	MDB_page	*mt_loose_pgs;
	// Number of loose pages (mt_loose_pgs)
	int			mt_loose_count;
	// The sorted list of dirty pages we temporarily wrote to disk
	// because the dirty list was full. page numbers in here are
	// shifted left by 1, deleted slots have the LSB set.
	MDB_IDL		mt_spill_pgs;
	union {
		// For write txns: Modified pages. Sorted when not MDB_WRITEMAP.
		MDB_ID2L	dirty_list;
		// For read txns: This thread/txn's reader table slot, or NULL.
		MDB_reader	*reader;
	} mt_u;
	// Array of records for each DB known in the environment.
	MDB_dbx		*mt_dbxs;
	// Array of MDB_db records for each known DB
	MDB_db		*mt_dbs;
	// Array of sequence numbers for each DB handle
	unsigned int	*mt_dbiseqs;
// Transaction DB Flags
// internal

#define DB_DIRTY	0x01		// DB was written in this txn
#define DB_STALE	0x02		// Named-DB record is older than txnID
#define DB_NEW		0x04		// Named-DB handle opened in this txn
#define DB_VALID	0x08		// DB handle is valid, see also MDB_VALID
#define DB_USRVALID	0x10		// As DB_VALID, but not set for FREE_DBI
#define DB_DUPDATA	0x20		// DB is MDB_DUPSORT data

	// In write txns, array of cursors for each DB
	MDB_cursor	**mt_cursors;
	// Array of flags for each DB
	unsigned char	*mt_dbflags;
	//	Number of DB records in use, or 0 when the txn is finished.
	//	This number only ever increments until the txn finishes; we
	//	don't decrement it when individual DB handles are closed.
	MDB_dbi		mt_numdbs;

// Transaction Flags
// internal

	// mdb_txn_begin() flags
#define MDB_TXN_BEGIN_FLAGS	(MDB_NOMETASYNC|MDB_NOSYNC|MDB_RDONLY)
#define MDB_TXN_NOMETASYNC	MDB_NOMETASYNC	// don't sync meta for this txn on commit
#define MDB_TXN_NOSYNC		MDB_NOSYNC	// don't sync this txn on commit
#define MDB_TXN_RDONLY		MDB_RDONLY	// read-only transaction
	// internal txn flags
#define MDB_TXN_WRITEMAP	MDB_WRITEMAP	// copy of MDB_env flag in writers
#define MDB_TXN_FINISHED	0x01		// txn is finished or never began
#define MDB_TXN_ERROR		0x02		// txn is unusable after an error
#define MDB_TXN_DIRTY		0x04		// must write, even if dirty list is empty
#define MDB_TXN_SPILLS		0x08		// txn or a parent has spilled pages
#define MDB_TXN_HAS_CHILD	0x10		// txn has an MDB_txn.mt_child
	// most operations on the txn are currently illegal
#define MDB_TXN_BLOCKED		(MDB_TXN_FINISHED|MDB_TXN_ERROR|MDB_TXN_HAS_CHILD)

	unsigned int	mt_flags;		// Transaction Flags
	// dirty_list room: Array size - #dirty pages visible to this txn.
	// Includes ancestor txns' dirty pages not hidden by other txns'
	// dirty/spilled pages. Thus commit(nested txn) has room to merge
	// dirty_list into mt_parent after freeing hidden mt_parent pages.
	unsigned int	mt_dirty_room;
};

enum {
	// mdb_txn_end operation number, for logging
	MDB_END_COMMITTED, MDB_END_EMPTY_COMMIT, MDB_END_ABORT, MDB_END_RESET,
	MDB_END_RESET_TMP, MDB_END_FAIL_BEGIN, MDB_END_FAIL_BEGINCHILD
};
#define MDB_END_OPMASK	0x0F	// mask for mdb_txn_end() operation number
#define MDB_END_UPDATE	0x10	// update env state (DBIs)
#define MDB_END_FREE	0x20	// free txn unless it is MDB_env.me_txn0
#define MDB_END_SLOT MDB_NOTLS	// release any reader slot if MDB_NOTLS
void mdb_txn_end(MDB_txn *txn, unsigned mode);

void mdb_txn_abort(MDB_txn *txn);
int mdb_txn_renew0(MDB_txn *txn);

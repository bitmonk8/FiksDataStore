#pragma once

#include "internal.h"

// A database transaction.
// Every operation requires a transaction handle.
struct FDS_txn
{
    FDS_txn* mt_parent;  // parent of a nested txn
    // Nested txn under this txn, set together with flag FDS_TXN_HAS_CHILD
    FDS_txn* mt_child;
    pgno_t mt_next_pgno;  // next unallocated page
    // The ID of this transaction. IDs are integers incrementing from 1.
    // Only committed write transactions increment the ID. If a transaction
    // aborts, the ID may be re-used by the next writer.
    txnid_t mt_txnid;
    FDS_env* mt_env;  // the DB environment
    // The list of pages that became unused during this transaction.
    FDS_IDL mt_free_pgs;
    // The list of loose pages that became unused and may be reused
    // in this transaction, linked through NEXT_LOOSE_PAGE(page).
    FDS_page* mt_loose_pgs;
    // Number of loose pages (mt_loose_pgs)
    int mt_loose_count;
    // The sorted list of dirty pages we temporarily wrote to disk
    // because the dirty list was full. page numbers in here are
    // shifted left by 1, deleted slots have the LSB set.
    FDS_IDL mt_spill_pgs;
    union
    {
        // For write txns: Modified pages. Sorted when not FDS_WRITEMAP.
        FDS_ID2L dirty_list;
        // For read txns: This thread/txn's reader table slot, or NULL.
        FDS_reader* reader;
    } mt_u;
    // Array of records for each DB known in the environment.
    FDS_dbx* mt_dbxs;
    // Array of FDS_db records for each known DB
    FDS_db* mt_dbs;
    // Array of sequence numbers for each DB handle
    unsigned int* mt_dbiseqs;
    // Transaction DB Flags
    // internal

#define DB_DIRTY 0x01     // DB was written in this txn
#define DB_STALE 0x02     // Named-DB record is older than txnID
#define DB_NEW 0x04       // Named-DB handle opened in this txn
#define DB_VALID 0x08     // DB handle is valid, see also FDS_VALID
#define DB_USRVALID 0x10  // As DB_VALID, but not set for FREE_DBI

    // In write txns, array of cursors for each DB
    FDS_cursor** mt_cursors;
    // Array of flags for each DB
    unsigned char* mt_dbflags;
    //	Number of DB records in use, or 0 when the txn is finished.
    //	This number only ever increments until the txn finishes; we
    //	don't decrement it when individual DB handles are closed.
    FDS_dbi mt_numdbs;

    // Transaction Flags
    // internal

// fds_txn_begin() flags
#define FDS_TXN_BEGIN_FLAGS (FDS_NOMETASYNC | FDS_NOSYNC | FDS_RDONLY)
#define FDS_TXN_NOMETASYNC FDS_NOMETASYNC  // don't sync meta for this txn on commit
#define FDS_TXN_NOSYNC FDS_NOSYNC          // don't sync this txn on commit
#define FDS_TXN_RDONLY FDS_RDONLY          // read-only transaction

// internal txn flags
#define FDS_TXN_WRITEMAP FDS_WRITEMAP  // copy of FDS_env flag in writers
#define FDS_TXN_FINISHED 0x01          // txn is finished or never began
#define FDS_TXN_ERROR 0x02             // txn is unusable after an error
#define FDS_TXN_DIRTY 0x04             // must write, even if dirty list is empty
#define FDS_TXN_SPILLS 0x08            // txn or a parent has spilled pages
#define FDS_TXN_HAS_CHILD 0x10         // txn has an FDS_txn.mt_child

// most operations on the txn are currently illegal
#define FDS_TXN_BLOCKED (FDS_TXN_FINISHED | FDS_TXN_ERROR | FDS_TXN_HAS_CHILD)

    unsigned int mt_flags;  // Transaction Flags
    // dirty_list room: Array size - #dirty pages visible to this txn.
    // Includes ancestor txns' dirty pages not hidden by other txns'
    // dirty/spilled pages. Thus commit(nested txn) has room to merge
    // dirty_list into mt_parent after freeing hidden mt_parent pages.
    unsigned int mt_dirty_room;
};

// fds_txn_end operation number, for logging
constexpr int FDS_END_COMMITTED = 0;
constexpr int FDS_END_EMPTY_COMMIT = 1;
constexpr int FDS_END_ABORT = 2;
constexpr int FDS_END_RESET = 3;
constexpr int FDS_END_RESET_TMP = 4;
constexpr int FDS_END_FAIL_BEGIN = 5;
constexpr int FDS_END_FAIL_BEGINCHILD = 6;
constexpr unsigned FDS_END_OPMASK = 0x0F;     // mask for fds_txn_end() operation number
constexpr unsigned FDS_END_UPDATE = 0x10;     // update env state (DBIs)
constexpr unsigned FDS_END_FREE = 0x20;       // free txn unless it is FDS_env.me_txn0
constexpr unsigned FDS_END_SLOT = FDS_NOTLS;  // release any reader slot if FDS_NOTLS

void fds_txn_end(FDS_txn* txn, unsigned mode);

auto fds_txn_renew0(FDS_txn* txn) -> int;

// Internal implementation function
void fds_txn_abort_impl(FDS_txn* txn);

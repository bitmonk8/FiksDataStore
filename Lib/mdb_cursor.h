#pragma once

#include "mdb_db.h"
#include "mdb_internal.h"

#include <array>

// Forward declaration for MDB_xcursor
struct MDB_xcursor;

// Cursors are used for all DB operations.
// A cursor holds a path of (page pointer, key index) from the DB
// root to a position in the DB, plus other state. Write txns
// track their cursors and keep them up to date when data moves.
struct MDB_cursor
{
    // Next cursor on this DB in this txn
    MDB_cursor* mc_next;
    // Backup of the original cursor if this cursor is a shadow
    MDB_cursor* mc_backup;
    // The transaction that owns this cursor
    MDB_txn* mc_txn;
    // The database handle this cursor operates on
    MDB_dbi mc_dbi;
    // The database record for this cursor
    MDB_db* mc_db;
    // The database auxiliary record for this cursor
    MDB_dbx* mc_dbx;
    // The mt_dbflag for this database
    unsigned char* mc_dbflag;
    unsigned short mc_snum;  // number of pushed pages
    unsigned short mc_top;   // index of top page, normally mc_snum-1
// Cursor Flags
// Cursor state flags.
#define C_INITIALIZED 0x01           // cursor has been initialized and is valid
#define C_EOF 0x02                   // No more data
#define C_SUB 0x04                   // Cursor is a sub-cursor
#define C_DEL 0x08                   // last op was a cursor_del
#define C_UNTRACK 0x40               // Un-track cursor when closing
#define C_WRITEMAP MDB_TXN_WRITEMAP  // Copy of txn flag
// Read-only cursor into the txn's original snapshot in the map.
// Set for read-only txns. Only implements code which is necessary for this.
#define C_ORIG_RDONLY MDB_TXN_RDONLY
    unsigned int mc_flags;                      // mdb_cursor
    std::array<MDB_page*, CURSOR_STACK> mc_pg;  // stack of pushed pages
    std::array<indx_t, CURSOR_STACK> mc_ki;     // stack of page indices
    // Extended cursor for duplicate data (removed but kept for compatibility)
    MDB_xcursor* mc_xcursor;
#define MC_OVPG(mc) ((MDB_page*)0)
#define MC_SET_OVPG(mc, pg) ((void)0)
};

// Extended cursor structure (removed but kept for compatibility)
struct MDB_xcursor
{
    MDB_cursor mx_cursor;
};

// Macros for duplicate support (removed but kept for compatibility)
#define XCURSOR_REFRESH(mc, top, mp) ((void)0)
#define IS_SUBP(mp) (0)

// Perform act while tracking temporary cursor mn
#define WITH_CURSOR_TRACKING(mn, act)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        MDB_cursor *tracked, **tp = &(mn).mc_txn->mt_cursors[(mn).mc_dbi];                                             \
        tracked = &(mn);                                                                                               \
        tracked->mc_next = *tp;                                                                                        \
        *tp = tracked;                                                                                                 \
        {                                                                                                              \
            act;                                                                                                       \
        }                                                                                                              \
        *tp = tracked->mc_next;                                                                                        \
    } while (0)

void mdb_cursor_init(MDB_cursor* mc, MDB_txn* txn, MDB_dbi dbi, MDB_xcursor* mx = nullptr);

void mdb_cursor_copy(const MDB_cursor* csrc, MDB_cursor* cdst);
void mdb_cursor_pop(MDB_cursor* mc);
auto mdb_cursor_push(MDB_cursor* mc, MDB_page* mp) -> int;

// Internal implementation functions
auto mdb_cursor_del_impl(MDB_cursor* mc, unsigned int flags) -> int;
auto mdb_cursor_put_impl(MDB_cursor* mc, MDB_val* key, MDB_val* data, unsigned int flags) -> int;

auto mdb_cursor_del0(MDB_cursor* mc) -> int;
auto mdb_cursor_sibling(MDB_cursor* mc, int move_right) -> int;
auto mdb_cursor_next(MDB_cursor* mc, MDB_val* key, MDB_val* data, MDB_cursor_op op) -> int;
auto mdb_cursor_prev(MDB_cursor* mc, MDB_val* key, MDB_val* data, MDB_cursor_op op) -> int;
auto mdb_cursor_set(MDB_cursor* mc, MDB_val* key, MDB_val* data, MDB_cursor_op op, int* exactp) -> int;
auto mdb_cursor_first(MDB_cursor* mc, MDB_val* key, MDB_val* data) -> int;
auto mdb_cursor_last(MDB_cursor* mc, MDB_val* key, MDB_val* data) -> int;

auto mdb_rebalance(MDB_cursor* mc) -> int;
auto mdb_update_key(MDB_cursor* mc, MDB_val* key) -> int;

#pragma once

#include "db.h"
#include "internal.h"

#include <array>

// Cursors are used for all DB operations.
// A cursor holds a path of (page pointer, key index) from the DB
// root to a position in the DB, plus other state. Write txns
// track their cursors and keep them up to date when data moves.
struct FDS_cursor
{
    // Next cursor on this DB in this txn
    FDS_cursor* mc_next;
    // Backup of the original cursor if this cursor is a shadow
    FDS_cursor* mc_backup;
    // The transaction that owns this cursor
    FDS_txn* mc_txn;
    // The database handle this cursor operates on
    FDS_dbi mc_dbi;
    // The database record for this cursor
    FDS_db* mc_db;
    // The database auxiliary record for this cursor
    FDS_dbx* mc_dbx;
    // The mt_dbflag for this database
    unsigned char* mc_dbflag;
    unsigned short mc_snum;  // number of pushed pages
    unsigned short mc_top;   // index of top page, normally mc_snum-1
// Cursor Flags
// Cursor state flags.
#define C_INITIALIZED 0x01           // cursor has been initialized and is valid
#define C_EOF 0x02                   // No more data
#define C_DEL 0x08                   // last op was a cursor_del
#define C_UNTRACK 0x40               // Un-track cursor when closing
#define C_WRITEMAP FDS_TXN_WRITEMAP  // Copy of txn flag
// Read-only cursor into the txn's original snapshot in the map.
// Set for read-only txns. Only implements code which is necessary for this.
#define C_ORIG_RDONLY FDS_TXN_RDONLY
    unsigned int mc_flags;                      // fds_cursor
    std::array<FDS_page*, CURSOR_STACK> mc_pg;  // stack of pushed pages
    std::array<indx_t, CURSOR_STACK> mc_ki;     // stack of page indices
#define MC_OVPG(mc) ((FDS_page*)0)
#define MC_SET_OVPG(mc, pg) ((void)0)
};

// Macros for duplicate support (removed but kept for compatibility)
#define XCURSOR_REFRESH(mc, top, mp) ((void)0)
#define IS_SUBP(mp) (0)

// Perform act while tracking temporary cursor mn
#define WITH_CURSOR_TRACKING(mn, act)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        FDS_cursor* tracked;                                                                                           \
        FDS_cursor** tp = &(mn).mc_txn->mt_cursors[(mn).mc_dbi];                                                       \
        tracked = &(mn);                                                                                               \
        tracked->mc_next = *tp;                                                                                        \
        *tp = tracked;                                                                                                 \
        {                                                                                                              \
            act;                                                                                                       \
        }                                                                                                              \
        *tp = tracked->mc_next;                                                                                        \
    } while (0)

void fds_cursor_init(FDS_cursor* mc, FDS_txn* txn, FDS_dbi dbi);

void fds_cursor_copy(const FDS_cursor* csrc, FDS_cursor* cdst);
void fds_cursor_pop(FDS_cursor* mc);
auto fds_cursor_push(FDS_cursor* mc, FDS_page* mp) -> int;

// Internal implementation functions
auto fds_cursor_del_impl(FDS_cursor* mc, unsigned int flags) -> int;
auto fds_cursor_put_impl(FDS_cursor* mc, FDS_val* key, FDS_val* data, unsigned int flags) -> int;

auto fds_cursor_del0(FDS_cursor* mc) -> int;
auto fds_cursor_sibling(FDS_cursor* mc, int move_right) -> int;
auto fds_cursor_next(FDS_cursor* mc, FDS_val* key, FDS_val* data, FDS_cursor_op op) -> int;
auto fds_cursor_prev(FDS_cursor* mc, FDS_val* key, FDS_val* data, FDS_cursor_op op) -> int;
auto fds_cursor_set(FDS_cursor* mc, FDS_val* key, FDS_val* data, FDS_cursor_op op, int* exactp) -> int;
auto fds_cursor_first(FDS_cursor* mc, FDS_val* key, FDS_val* data) -> int;
auto fds_cursor_last(FDS_cursor* mc, FDS_val* key, FDS_val* data) -> int;
auto fds_update_key(FDS_cursor* mc, FDS_val* key) -> int;

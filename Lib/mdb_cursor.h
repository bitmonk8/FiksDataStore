#pragma once

#include "mdb_internal.h"
#include "mdb_db.h"

// Cursors are used for all DB operations.
// A cursor holds a path of (page pointer, key index) from the DB
// root to a position in the DB, plus other state. MDB_DUPSORT
// cursors include an xcursor to the current data item. Write txns
// track their cursors and keep them up to date when data moves.
// Exception: An xcursor's pointer to a P_SUBP page can be stale.
// (A node with F_DUPDATA but no F_SUBDATA contains a subpage).
struct MDB_cursor {
// Next cursor on this DB in this txn
MDB_cursor	*mc_next;
// Backup of the original cursor if this cursor is a shadow
MDB_cursor	*mc_backup;
// Context used for databases with MDB_DUPSORT, otherwise NULL
struct MDB_xcursor	*mc_xcursor;
// The transaction that owns this cursor
MDB_txn		*mc_txn;
// The database handle this cursor operates on
MDB_dbi		mc_dbi;
// The database record for this cursor
MDB_db		*mc_db;
// The database auxiliary record for this cursor
MDB_dbx		*mc_dbx;
// The mt_dbflag for this database
unsigned char	*mc_dbflag;
unsigned short 	mc_snum;	// number of pushed pages
unsigned short	mc_top;		// index of top page, normally mc_snum-1
// Cursor Flags
// Cursor state flags.
#define C_INITIALIZED	0x01	// cursor has been initialized and is valid
#define C_EOF	0x02			// No more data
#define C_SUB	0x04			// Cursor is a sub-cursor
#define C_DEL	0x08			// last op was a cursor_del
#define C_UNTRACK	0x40		// Un-track cursor when closing
#define C_WRITEMAP	MDB_TXN_WRITEMAP // Copy of txn flag
// Read-only cursor into the txn's original snapshot in the map.
// Set for read-only txns. Only implements code which is necessary for this.
#define C_ORIG_RDONLY	MDB_TXN_RDONLY
unsigned int	mc_flags;	// mdb_cursor
MDB_page	*mc_pg[CURSOR_STACK];	// stack of pushed pages
indx_t		mc_ki[CURSOR_STACK];	// stack of page indices
#define MC_OVPG(mc)			((MDB_page *)0)
#define MC_SET_OVPG(mc, pg)	((void)0)

};

// Context for sorted-dup records.
// We could have gone to a fully recursive design, with arbitrarily
// deep nesting of sub-databases. But for now we only handle these
// levels - main DB, optional sub-DB, sorted-duplicate DB.
struct MDB_xcursor {
// A sub-cursor for traversing the Dup DB
MDB_cursor mx_cursor;
// The database record for this Dup DB
MDB_db	mx_db;
// The auxiliary DB record for this Dup DB
MDB_dbx	mx_dbx;
// The mt_dbflag for this Dup DB
unsigned char mx_dbflag;
};

// Check if there is an inited xcursor
#define XCURSOR_INITED(mc) \
	((mc)->mc_xcursor && ((mc)->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED))

// Update the xcursor's sub-page pointer, if any, in mc.  Needed
// when the node which contains the sub-page may have moved.  Called
// with leaf page mp = mc->mc_pg[top].
#define XCURSOR_REFRESH(mc, top, mp) do { \
	MDB_page *xr_pg = (mp); \
	MDB_node *xr_node; \
	if (!XCURSOR_INITED(mc) || (mc)->mc_ki[top] >= NUMKEYS(xr_pg)) break; \
	xr_node = NODEPTR(xr_pg, (mc)->mc_ki[top]); \
	if ((xr_node->mn_flags & (F_DUPDATA|F_SUBDATA)) == F_DUPDATA) \
		(mc)->mc_xcursor->mx_cursor.mc_pg[0] = reinterpret_cast<MDB_page*>(reinterpret_cast<char*>(xr_node->mn_data) + xr_node->mn_ksize); \
} while (0)

// Perform act while tracking temporary cursor mn
#define WITH_CURSOR_TRACKING(mn, act) do { \
	MDB_cursor dummy, *tracked, **tp = &(mn).mc_txn->mt_cursors[(mn).mc_dbi]; \
	if ((mn).mc_flags & C_SUB) { \
		dummy.mc_flags =  C_INITIALIZED; \
		dummy.mc_xcursor = (MDB_xcursor *)&(mn);	\
		tracked = &dummy; \
	} else { \
		tracked = &(mn); \
	} \
	tracked->mc_next = *tp; \
	*tp = tracked; \
	{ act; } \
	*tp = tracked->mc_next; \
} while (0)

void mdb_cursor_init(MDB_cursor *mc, MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx);
void mdb_xcursor_init0(MDB_cursor *mc);
void mdb_xcursor_init1(MDB_cursor *mc, MDB_node *node);
void mdb_xcursor_init2(MDB_cursor *mc, MDB_xcursor *src_mx, int force);

void mdb_cursor_copy(const MDB_cursor *csrc, MDB_cursor *cdst);
void mdb_cursor_pop(MDB_cursor *mc);
int	mdb_cursor_push(MDB_cursor *mc, MDB_page *mp);

int	mdb_cursor_del(MDB_cursor *mc, unsigned int flags);
int	mdb_cursor_put(MDB_cursor *mc, MDB_val *key, MDB_val *data, unsigned int flags);

// Internal implementation functions
int	mdb_cursor_del_impl(MDB_cursor *mc, unsigned int flags);
int	mdb_cursor_put_impl(MDB_cursor *mc, MDB_val *key, MDB_val *data, unsigned int flags);

int	mdb_cursor_del0(MDB_cursor *mc);
int	mdb_cursor_sibling(MDB_cursor *mc, int move_right);
int	mdb_cursor_next(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op);
int	mdb_cursor_prev(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op);
int	mdb_cursor_set(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op, int *exactp);
int	mdb_cursor_first(MDB_cursor *mc, MDB_val *key, MDB_val *data);
int	mdb_cursor_last(MDB_cursor *mc, MDB_val *key, MDB_val *data);

int	mdb_rebalance(MDB_cursor *mc);
int	mdb_update_key(MDB_cursor *mc, MDB_val *key);

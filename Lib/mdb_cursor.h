#pragma once

#include "mdb_internal.h"

/** Perform \b act while tracking temporary cursor \b mn */
#define WITH_CURSOR_TRACKING(mn, act) do { \
	MDB_cursor dummy, *tracked, **tp = &(mn).mc_txn->mt_cursors[mn.mc_dbi]; \
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

int	_mdb_cursor_del(MDB_cursor *mc, unsigned int flags);
int	_mdb_cursor_put(MDB_cursor *mc, MDB_val *key, MDB_val *data, unsigned int flags);

int	mdb_cursor_del0(MDB_cursor *mc);
int	mdb_cursor_sibling(MDB_cursor *mc, int move_right);
int	mdb_cursor_next(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op);
int	mdb_cursor_prev(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op);
int	mdb_cursor_set(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op, int *exactp);
int	mdb_cursor_first(MDB_cursor *mc, MDB_val *key, MDB_val *data);
int	mdb_cursor_last(MDB_cursor *mc, MDB_val *key, MDB_val *data);

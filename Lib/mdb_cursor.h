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


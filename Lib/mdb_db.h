#pragma once

#include "mdb_internal.h"

	/** Information about a single database in the environment. */
struct MDB_db {
	uint32_t	md_pad;		/**< also ksize for LEAF2 pages */
	uint16_t	md_flags;	/**< @ref mdb_dbi_open */
	uint16_t	md_depth;	/**< depth of this tree */
	pgno_t		md_branch_pages;	/**< number of internal pages */
	pgno_t		md_leaf_pages;		/**< number of leaf pages */
	pgno_t		md_overflow_pages;	/**< number of overflow pages */
	mdb_size_t	md_entries;		/**< number of data items */
	pgno_t		md_root;		/**< the root page of this tree */
};

	/** Check \b txn and \b dbi arguments to a function */
#define TXN_DBI_EXIST(txn, dbi, validity) \
	((txn) && (dbi)<(txn)->mt_numdbs && ((txn)->mt_dbflags[dbi] & (validity)))

	/** Check for misused \b dbi handles */
#define TXN_DBI_CHANGED(txn, dbi) \
	((txn)->mt_dbiseqs[dbi] != (txn)->mt_env->me_dbiseqs[dbi])

int	mdb_drop0(MDB_cursor *mc, int subs);

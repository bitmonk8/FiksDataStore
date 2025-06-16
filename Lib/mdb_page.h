#ifndef MDB_PAGE_H
#define MDB_PAGE_H

#include "lmdb.h"
#include "midl.h"

/* Forward declarations for opaque structs */
typedef struct MDB_env MDB_env;
typedef struct MDB_txn MDB_txn;
typedef struct MDB_cursor MDB_cursor;

typedef MDB_ID pgno_t;
typedef uint16_t indx_t;

/** Common header for all page types. The page type depends on #mp_flags.
 */
typedef struct MDB_page {
#define	mp_pgno	mp_p.p_pgno
#define	mp_next	mp_p.p_next
	union {
		pgno_t		p_pgno;	/**< page number */
		struct MDB_page *p_next; /**< for in-memory list of freed pages */
	} mp_p;
	uint16_t	mp_pad;			/**< key size if this is a LEAF2 page */
	uint16_t	mp_flags;		/**< @ref mdb_page */
#define mp_lower	mp_pb.pb.pb_lower
#define mp_upper	mp_pb.pb.pb_upper
#define mp_pages	mp_pb.pb_pages
	union {
		struct {
			indx_t		pb_lower;		/**< lower bound of free space */
			indx_t		pb_upper;		/**< upper bound of free space */
		} pb;
		uint32_t	pb_pages;	/**< number of overflow pages */
	} mp_pb;
	indx_t		mp_ptrs[0];		/**< dynamic size */
} MDB_page;

#define	P_BRANCH	 0x01		/**< branch page */
#define	P_LEAF		 0x02		/**< leaf page */
#define	P_OVERFLOW	 0x04		/**< overflow page */
#define	P_META		 0x08		/**< meta page */
#define	P_DIRTY		 0x10		/**< dirty page, also set for #P_SUBP pages */
#define	P_LEAF2		 0x20		/**< for #MDB_DUPFIXED records */
#define	P_SUBP		 0x40		/**< for #MDB_DUPSORT sub-pages */
#define	P_LOOSE		 0x4000		/**< page was dirtied then freed, can be reused */
#define	P_KEEP		 0x8000		/**< leave this page alone during spill */

/* Page search flags */
#define MDB_PS_MODIFY	1
#define MDB_PS_ROOTONLY	2
#define MDB_PS_FIRST	4
#define MDB_PS_LAST		8

#define MDB_SPLIT_REPLACE	MDB_APPENDDUP	/**< newkey is not new */

/* from mdb.c, for MDB_cursor */
#define C_INITIALIZED	0x01	/**< cursor has been initialized and is valid */
#define C_EOF	0x02			/**< No more data */
#define C_SUB	0x04			/**< Cursor is a sub-cursor */
#define C_DEL	0x08			/**< last op was a cursor_del */
#define C_UNTRACK	0x40		/**< Un-track cursor when closing */
#define C_WRITEMAP	MDB_TXN_WRITEMAP /**< Copy of txn flag */
#define C_ORIG_RDONLY	MDB_TXN_RDONLY

/* from mdb.c, for MDB_txn */
#define MDB_TXN_WRITEMAP	MDB_WRITEMAP	/**< copy of #MDB_env flag in writers */
#define MDB_TXN_FINISHED	0x01		/**< txn is finished or never began */
#define MDB_TXN_ERROR		0x02		/**< txn is unusable after an error */
#define MDB_TXN_DIRTY		0x04		/**< must write, even if dirty list is empty */
#define MDB_TXN_SPILLS		0x08		/**< txn or a parent has spilled pages */
#define MDB_TXN_HAS_CHILD	0x10		/**< txn has an #MDB_txn.%mt_child */
#define MDB_TXN_BLOCKED		(MDB_TXN_FINISHED|MDB_TXN_ERROR|MDB_TXN_HAS_CHILD)

/* from mdb.c, for MDB_node */
#define F_BIGDATA	 0x01			/**< data put on overflow page */
#define F_SUBDATA	 0x02			/**< data is a sub-database */
#define F_DUPDATA	 0x04			/**< data has duplicates */
#define	NODE_ADD_FLAGS	(F_DUPDATA|F_SUBDATA|MDB_RESERVE|MDB_APPEND)

/* Page Management Functions */
int  mdb_page_alloc(MDB_cursor *mc, int num, MDB_page **mp);
int  mdb_page_new(MDB_cursor *mc, uint32_t flags, int num, MDB_page **mp);
int  mdb_page_touch(MDB_cursor *mc);
int  mdb_page_get(MDB_cursor *mc, pgno_t pgno, MDB_page **mp, int *lvl);
int  mdb_page_search_root(MDB_cursor *mc, MDB_val *key, int modify);
int  mdb_page_search(MDB_cursor *mc, MDB_val *key, int flags);
int	 mdb_page_merge(MDB_cursor *csrc, MDB_cursor *cdst);
int	 mdb_page_split(MDB_cursor *mc, MDB_val *newkey, MDB_val *newdata, pgno_t newpgno, unsigned int nflags);
void mdb_page_copy(MDB_page *dst, MDB_page *src, unsigned int psize);
int  mdb_page_flush(MDB_txn *txn, int keep);
size_t mdb_leaf_size(MDB_env *env, MDB_val *key, MDB_val *data);
size_t mdb_branch_size(MDB_env *env, MDB_val *key);
int  mdb_node_add(MDB_cursor *mc, indx_t indx, MDB_val *key, MDB_val *data, pgno_t pgno, unsigned int flags);
void mdb_node_del(MDB_cursor *mc, int ksize);
void mdb_node_shrink(MDB_page *mp, indx_t indx);
int  mdb_ovpage_free(MDB_cursor *mc, MDB_page *mp);
void mdb_dlist_free(MDB_txn *txn);
int mdb_page_spill(MDB_cursor *m0, MDB_val *key, MDB_val *data);
void mdb_page_dirty(MDB_txn *txn, MDB_page *mp);
MDB_page *mdb_page_malloc(MDB_txn *txn, unsigned num);
void mdb_dpage_free(MDB_env *env, MDB_page *dp);

#endif /* MDB_PAGE_H */
#pragma once

#include "mdb_internal.h"

	/** The database environment. */
struct MDB_env {
	HANDLE		me_fd;		/**< The main data file */
	HANDLE		me_lfd;		/**< The lock file */
	HANDLE		me_mfd;		/**< For writing and syncing the meta pages */
#ifdef _WIN32
	HANDLE		me_ovfd;	/**< Overlapped/async with write-through file handle */
#endif /* _WIN32 */
	/** Failed to update the meta page. Probably an I/O error. */
#define	MDB_FATAL_ERROR	0x80000000U
	/** Some fields are initialized. */
#define	MDB_ENV_ACTIVE	0x20000000U
	/** me_txkey is set */
#define	MDB_ENV_TXKEY	0x10000000U
	/** fdatasync is unreliable */
#define	MDB_FSYNCONLY	0x08000000U
	uint32_t 	me_flags;		/**< @ref mdb_env */
	unsigned int	me_psize;	/**< DB page size, inited from me_os_psize */
	unsigned int	me_os_psize;	/**< OS page size, from #GET_PAGESIZE */
	unsigned int	me_maxreaders;	/**< size of the reader table */
	/** Max #MDB_txninfo.%mti_numreaders of interest to #mdb_env_close() */
	volatile int	me_close_readers;
	MDB_dbi		me_numdbs;		/**< number of DBs opened */
	MDB_dbi		me_maxdbs;		/**< size of the DB table */
	MDB_PID_T	me_pid;		/**< process ID of this env */
	char		*me_path;		/**< path to the DB files */
	char		*me_map;		/**< the memory map of the data file */
	MDB_txninfo	*me_txns;		/**< the memory map of the lock file or NULL */
	MDB_meta	*me_metas[NUM_METAS];	/**< pointers to the two meta pages */
	void		*me_pbuf;		/**< scratch area for DUPSORT put() */
	MDB_txn		*me_txn;		/**< current write transaction */
	MDB_txn		*me_txn0;		/**< prealloc'd write transaction */
	mdb_size_t	me_mapsize;		/**< size of the data memory map */
	MDB_OFF_T	me_size;		/**< current file size */
	pgno_t		me_maxpg;		/**< me_mapsize / me_psize */
	MDB_dbx		*me_dbxs;		/**< array of static DB info */
	uint16_t	*me_dbflags;	/**< array of flags from MDB_db.md_flags */
	unsigned int	*me_dbiseqs;	/**< array of dbi sequence numbers */
	pthread_key_t	me_txkey;	/**< thread-key for readers */
	txnid_t		me_pgoldest;	/**< ID of oldest reader last time we looked */
	MDB_pgstate	me_pgstate;		/**< state of old pages from freeDB */
#	define		me_pglast	me_pgstate.mf_pglast
#	define		me_pghead	me_pgstate.mf_pghead
	MDB_page	*me_dpages;		/**< list of malloc'd blocks for re-use */
	/** IDL of pages that became unused in a write txn */
	MDB_IDL		me_free_pgs;
	/** ID2L of pages written during a write txn. Length MDB_IDL_UM_SIZE. */
	MDB_ID2L	me_dirty_list;
	/** Max number of freelist items that can fit in a single overflow page */
	int			me_maxfree_1pg;
	/** Max size of a node on a page */
	unsigned int	me_nodemax;
#if !(MDB_MAXKEYSIZE)
	unsigned int	me_maxkey;	/**< max size of a key */
#endif
	int		me_live_reader;		/**< have liveness lock in reader table */
#ifdef _WIN32
	int		me_pidquery;		/**< Used in OpenProcess */
	OVERLAPPED	*ov;			/**< Used for for overlapping I/O requests */
	int		ovs;				/**< Count of OVERLAPPEDs */
#endif
#ifdef MDB_USE_POSIX_MUTEX	/* Posix mutexes reside in shared mem */
#	define		me_rmutex	me_txns->mti_rmutex /**< Shared reader lock */
#	define		me_wmutex	me_txns->mti_wmutex /**< Shared writer lock */
#else
	mdb_mutex_t	me_rmutex;
	mdb_mutex_t	me_wmutex;
# if defined(_WIN32) || defined(MDB_USE_POSIX_SEM)
	/** Half-initialized name of mutexes, to be completed by #MUTEXNAME() */
	char		me_mutexname[sizeof(MUTEXNAME_PREFIX) + 11];
# endif
#endif
	void		*me_userctx;	 /**< User-settable context */
	MDB_assert_func *me_assert_func; /**< Callback for assertion failures */
};

int ESECT mdb_env_share_locks(MDB_env *env, int *excl);
int mdb_env_sync0(MDB_env *env, int force, pgno_t numpgs);

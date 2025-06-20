#pragma once

#include "mdb_internal.h"

#include "mdb_lock.h"
#include "mdb_db.h"

// Initial part of #MDB_env.me_mutexname[].
//	Changes to this code must be reflected in #MDB_LOCK_FORMAT.
#ifdef _WIN32
#define MUTEXNAME_PREFIX		"Global\\MDB"
#elif defined MDB_USE_POSIX_SEM
#define MUTEXNAME_PREFIX		"/MDB"
#endif

// Meta page content.
//	A meta page is the start point for accessing a database snapshot.
//	Pages 0-1 are meta pages. Transaction N writes meta page #(N % 2).
struct MDB_meta {
		// Stamp identifying this as an LMDB file. It must be set
		//	to #MDB_MAGIC.
	uint32_t	mm_magic;
		// Version number of this file. Must be set to #MDB_DATA_VERSION.
	uint32_t	mm_version;
	void		*mm_address;		//< address for fixed mapping
	mdb_size_t	mm_mapsize;			//< size of mmap region
	MDB_db		mm_dbs[CORE_DBS];	//< first is free space, 2nd is main db
	// The size of pages used in this DB
#define	mm_psize	mm_dbs[FREE_DBI].md_pad
	// Any persistent environment flags. @ref mdb_env
#define	mm_flags	mm_dbs[FREE_DBI].md_flags
	// Last used page in the datafile.
	//	Actually the file may be shorter if the freeDB lists the final pages.
	pgno_t		mm_last_pg;
	volatile txnid_t	mm_txnid;	//< txnid that committed this page
};

// State of FreeDB old pages, stored in the MDB_env
struct MDB_pgstate {
	pgno_t		*mf_pghead;	//< Reclaimed freeDB pages, or NULL before use
	txnid_t		mf_pglast;	//< ID of last used record, or 0 if !mf_pghead
};

// The header for the reader table.
//	The table resides in a memory-mapped file. (This is a different file
//	than is used for the main database.)
//
//	For POSIX the actual mutexes reside in the shared memory of this
//	mapped file. On Windows, mutexes are named objects allocated by the
//	kernel; we store the mutex names in this mapped file so that other
//	processes can grab them. This same approach is also used on
//	MacOSX/Darwin (using named semaphores) since MacOSX doesn't support
//	process-shared POSIX mutexes. For these cases where a named object
//	is used, the object name is derived from a 64 bit FNV hash of the
//	environment pathname. As such, naming collisions are extremely
//	unlikely. If a collision occurs, the results are unpredictable.
struct MDB_txbody {
		// Stamp identifying this as an LMDB file. It must be set
		//	to #MDB_MAGIC.
	uint32_t	mtb_magic;
		// Format of this lock file. Must be set to #MDB_LOCK_FORMAT.
	uint32_t	mtb_format;
		//	The ID of the last transaction committed to the database.
		//	This is recorded here only for convenience; the value can always
		//	be determined by reading the main database meta pages.
	volatile txnid_t		mtb_txnid;
		// The number of slots that have been used in the reader table.
		//	This always records the maximum count, it is not decremented
		//	when readers release their slots.
	volatile unsigned	mtb_numreaders;
#if defined(_WIN32) || defined(MDB_USE_POSIX_SEM)
		// Binary form of names of the reader/writer locks
	mdb_hash_t			mtb_mutexid;
#elif defined(MDB_USE_SYSV_SEM)
	int 	mtb_semid;
	int		mtb_rlocked;
#else
		// Mutex protecting access to this table.
		//	This is the reader table lock used with LOCK_MUTEX().
	mdb_mutex_t	mtb_rmutex;
#endif
};

// The actual reader table definition.
struct MDB_txninfo {
	union {
		MDB_txbody mtb;
#define mti_magic	mt1.mtb.mtb_magic
#define mti_format	mt1.mtb.mtb_format
#define mti_rmutex	mt1.mtb.mtb_rmutex
#define mti_txnid	mt1.mtb.mtb_txnid
#define mti_numreaders	mt1.mtb.mtb_numreaders
#define mti_mutexid	mt1.mtb.mtb_mutexid
#ifdef MDB_USE_SYSV_SEM
#define	mti_semid	mt1.mtb.mtb_semid
#define	mti_rlocked	mt1.mtb.mtb_rlocked
#endif
		char pad[(sizeof(MDB_txbody)+CACHELINE-1) & ~(CACHELINE-1)];
	} mt1;
#if !(defined(_WIN32) || defined(MDB_USE_POSIX_SEM))
	union {
#ifdef MDB_USE_SYSV_SEM
		int mt2_wlocked;
#define mti_wlocked	mt2.mt2_wlocked
#else
		mdb_mutex_t	mt2_wmutex;
#define mti_wmutex	mt2.mt2_wmutex
#endif
		char pad[(MNAME_LEN+CACHELINE-1) & ~(CACHELINE-1)];
	} mt2;
#endif
	MDB_reader	mti_readers[1];
};

// The database environment.
struct MDB_env {
	HANDLE		me_fd;		//< The main data file
	HANDLE		me_lfd;		//< The lock file
	HANDLE		me_mfd;		//< For writing and syncing the meta pages
#ifdef _WIN32
	HANDLE		me_ovfd;	//< Overlapped/async with write-through file handle
#endif /* _WIN32 */
	// Failed to update the meta page. Probably an I/O error.
#define	MDB_FATAL_ERROR	0x80000000U
	// Some fields are initialized.
#define	MDB_ENV_ACTIVE	0x20000000U
	// me_txkey is set
#define	MDB_ENV_TXKEY	0x10000000U
	// fdatasync is unreliable
#define	MDB_FSYNCONLY	0x08000000U
	uint32_t 	me_flags;		//< @ref mdb_env
	unsigned int	me_psize;	//< DB page size, inited from me_os_psize
	unsigned int	me_os_psize;	//< OS page size, from #GET_PAGESIZE
	unsigned int	me_maxreaders;	//< size of the reader table
	// Max #MDB_txninfo.%mti_numreaders of interest to #mdb_env_close()
	volatile int	me_close_readers;
	MDB_dbi		me_numdbs;		//< number of DBs opened
	MDB_dbi		me_maxdbs;		//< size of the DB table
	MDB_PID_T	me_pid;		//< process ID of this env
	char		*me_path;		//< path to the DB files
	char		*me_map;		//< the memory map of the data file
	MDB_txninfo	*me_txns;		//< the memory map of the lock file or NULL
	MDB_meta	*me_metas[NUM_METAS];	//< pointers to the two meta pages
	void		*me_pbuf;		//< scratch area for DUPSORT put()
	MDB_txn		*me_txn;		//< current write transaction
	MDB_txn		*me_txn0;		//< prealloc'd write transaction
	mdb_size_t	me_mapsize;		//< size of the data memory map
	MDB_OFF_T	me_size;		//< current file size
	pgno_t		me_maxpg;		//< me_mapsize / me_psize
	MDB_dbx		*me_dbxs;		//< array of static DB info
	uint16_t	*me_dbflags;	//< array of flags from MDB_db.md_flags
	unsigned int	*me_dbiseqs;	//< array of dbi sequence numbers
	pthread_key_t	me_txkey;	//< thread-key for readers
	txnid_t		me_pgoldest;	//< ID of oldest reader last time we looked
	MDB_pgstate	me_pgstate;		//< state of old pages from freeDB
#	define		me_pglast	me_pgstate.mf_pglast
#	define		me_pghead	me_pgstate.mf_pghead
	MDB_page	*me_dpages;		//< list of malloc'd blocks for re-use
	// IDL of pages that became unused in a write txn
	MDB_IDL		me_free_pgs;
	// ID2L of pages written during a write txn. Length MDB_IDL_UM_SIZE.
	MDB_ID2L	me_dirty_list;
	// Max number of freelist items that can fit in a single overflow page
	int			me_maxfree_1pg;
	// Max size of a node on a page
	unsigned int	me_nodemax;
#if !(MDB_MAXKEYSIZE)
	unsigned int	me_maxkey;	//< max size of a key
#endif
	int		me_live_reader;		//< have liveness lock in reader table
#ifdef _WIN32
	int		me_pidquery;		//< Used in OpenProcess
	OVERLAPPED	*ov;			//< Used for for overlapping I/O requests
	int		ovs;				//< Count of OVERLAPPEDs
#endif
#ifdef MDB_USE_POSIX_MUTEX	/* Posix mutexes reside in shared mem */
#	define		me_rmutex	me_txns->mti_rmutex // Shared reader lock
#	define		me_wmutex	me_txns->mti_wmutex // Shared writer lock
#else
	mdb_mutex_t	me_rmutex;
	mdb_mutex_t	me_wmutex;
# if defined(_WIN32) || defined(MDB_USE_POSIX_SEM)
	// Half-initialized name of mutexes, to be completed by #MUTEXNAME()
	char		me_mutexname[sizeof(MUTEXNAME_PREFIX) + 11];
# endif
#endif
	void		*me_userctx;	 // User-settable context
	MDB_assert_func *me_assert_func; // Callback for assertion failures
};

int ESECT mdb_env_share_locks(MDB_env *env, int *excl);
int mdb_env_sync0(MDB_env *env, int force, pgno_t numpgs);

int  mdb_env_read_header(MDB_env *env, int prev, MDB_meta *meta);
MDB_meta *mdb_env_pick_meta(const MDB_env *env);
int  mdb_env_write_meta(MDB_txn *txn);
void mdb_env_close0(MDB_env *env, int excl);

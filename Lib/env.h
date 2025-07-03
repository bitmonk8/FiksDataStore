#pragma once

#include "db.h"
// DESIGN_VIOLATION: `Lib/*.h` files must include `internal.h` first.
#include "internal.h"
#include "lock.h"

// Initial part of FDS_env.me_mutexname[].
// Changes to this code must be reflected in FDS_LOCK_FORMAT.
#ifdef FDS_WINDOWS
#define MUTEXNAME_PREFIX "Global\\MDB"
#endif

// Meta page content.
// A meta page is the start point for accessing a database snapshot.
// Pages 0-1 are meta pages. Transaction N writes meta page #(N % 2).
struct FDS_meta
{
    // Stamp identifying this as an FiksDataStore file. It must be set to FDS_MAGIC.
    uint32_t mm_magic;

    // Version number of this file. Must be set to FDS_DATA_VERSION.
    uint32_t mm_version;

    // size of mmap region
    size_t mm_mapsize;

    // first is free space, 2nd is main db
    // The size of pages used in this DB
    FDS_db mm_dbs[CORE_DBS];

    // Last used page in the datafile.
    // Actually the file may be shorter if the freeDB lists the final pages.
    pgno_t mm_last_pg;

    // txnid that committed this page
    volatile txnid_t mm_txnid;
};

// State of FreeDB old pages, stored in the FDS_env
struct FDS_pgstate
{
    pgno_t* mf_pghead;  // Reclaimed freeDB pages, or NULL before use
    txnid_t mf_pglast;  // ID of last used record, or 0 if !mf_pghead
};

// The header for the reader table.
// The table resides in a memory-mapped file. (This is a different file
// than is used for the main database.)
//
// For POSIX the actual mutexes reside in the shared memory of this
// mapped file. On Windows, mutexes are named objects allocated by the
// kernel; we store the mutex names in this mapped file so that other
// processes can grab them. This same approach is also used on
// MacOSX/Darwin (using named semaphores) since MacOSX doesn't support
// process-shared POSIX mutexes. For these cases where a named object
// is used, the object name is derived from a 64 bit FNV hash of the
// environment pathname. As such, naming collisions are extremely
// unlikely. If a collision occurs, the results are unpredictable.
struct FDS_txbody
{
    // Stamp identifying this as an FiksDataStore file. It must be set
    // to FDS_MAGIC.
    uint32_t mtb_magic;
    // Format of this lock file. Must be set to FDS_LOCK_FORMAT.
    uint32_t mtb_format;
    // The ID of the last transaction committed to the database.
    // This is recorded here only for convenience; the value can always
    // be determined by reading the main database meta pages.
    volatile txnid_t mtb_txnid;
    // The number of slots that have been used in the reader table.
    // This always records the maximum count, it is not decremented
    // when readers release their slots.
    volatile unsigned mtb_numreaders;
#if defined(FDS_WINDOWS)
    // Binary form of names of the reader/writer locks
    fds_hash_t mtb_mutexid;
#elif defined(FDS_MACOS)
    int mtb_semid;
    int mtb_rlocked;
#else
    // Mutex protecting access to this table.
    // This is the reader table lock used with LOCK_MUTEX().
    fds_mutex_t mtb_rmutex;
#endif
};

// The actual reader table definition.
struct FDS_txninfo
{
    union
    {
        FDS_txbody mtb;
        char pad[(sizeof(FDS_txbody) + CACHELINE - 1) & ~(CACHELINE - 1)];
    };

#if !(defined(FDS_WINDOWS))
    union
    {
#ifdef FDS_MACOS
        int mt2_wlocked;
#else
        fds_mutex_t mt2_wmutex;
#endif
        char pad[(MNAME_LEN + CACHELINE - 1) & ~(CACHELINE - 1)];
    };
#endif

    FDS_reader mti_readers[1];
};

// The database environment.
struct FDS_env
{
    HANDLE me_fd;   // The main data file
    HANDLE me_lfd;  // The lock file
    HANDLE me_mfd;  // For writing and syncing the meta pages
#ifdef FDS_WINDOWS
    HANDLE me_ovfd;  // Overlapped/async with write-through file handle
#endif               /* FDS_WINDOWS */
                     // Failed to update the meta page. Probably an I/O error.
#define FDS_FATAL_ERROR 0x80000000U
    // Some fields are initialized.
#define FDS_ENV_ACTIVE 0x20000000U
    // me_txkey is set
#define FDS_ENV_TXKEY 0x10000000U
    // fdatasync is unreliable
#define FDS_FSYNCONLY 0x08000000U
    uint32_t me_flags;           // Environment flags
    unsigned int me_psize;       // DB page size, inited from me_os_psize
    unsigned int me_os_psize;    // OS page size, from GET_PAGESIZE
    unsigned int me_maxreaders;  // size of the reader table
    // Max FDS_txninfo.mtb.mtb_numreaders of interest to fds_env_close()
    volatile int me_close_readers;
    FDS_dbi me_numdbs;              // number of DBs opened
    FDS_dbi me_maxdbs;              // size of the DB table
    FDS_PID_T me_pid;               // process ID of this env
    char* me_path;                  // path to the DB files
    char* me_map;                   // the memory map of the data file
    FDS_txninfo* me_txns;           // the memory map of the lock file or NULL
    FDS_meta* me_metas[NUM_METAS];  // pointers to the two meta pages
    void* me_pbuf;                  // scratch area for put operations
    FDS_txn* me_txn;                // current write transaction
    FDS_txn* me_txn0;               // prealloc'd write transaction
    size_t me_mapsize;              // size of the data memory map
    FDS_OFF_T me_size;              // current file size
    pgno_t me_maxpg;                // me_mapsize / me_psize
    FDS_dbx* me_dbxs;               // array of static DB info
    uint16_t* me_dbflags;           // array of flags from FDS_db.md_flags
    unsigned int* me_dbiseqs;       // array of dbi sequence numbers
    pthread_key_t me_txkey;         // thread-key for readers
    txnid_t me_pgoldest;            // ID of oldest reader last time we looked
    FDS_pgstate me_pgstate;         // state of old pages from freeDB
    FDS_page* me_dpages;            // list of malloc'd blocks for re-use
    // IDL of pages that became unused in a write txn
    FDS_IDL me_free_pgs;
    // ID2L of pages written during a write txn. Length FDS_IDL_UM_SIZE.
    FDS_ID2L me_dirty_list;
    // Max number of freelist items that can fit in a single overflow page
    int me_maxfree_1pg;
    // Max size of a node on a page
    unsigned int me_nodemax;
#if !(FDS_MAXKEYSIZE)
    unsigned int me_maxkey;  // max size of a key
#endif
    int me_live_reader;  // have liveness lock in reader table
#ifdef FDS_WINDOWS
    int me_pidquery;  // Used in OpenProcess
    OVERLAPPED* ov;   // Used for for overlapping I/O requests
    int ovs;          // Count of OVERLAPPEDs
#endif
#ifdef FDS_LINUX                           /* Posix mutexes reside in shared mem */
#define me_rmutex me_txns->mtb.mtb_rmutex  // Shared reader lock
#define me_wmutex me_txns->mt2_wmutex      // Shared writer lock
#else
    fds_mutex_t me_rmutex;
    fds_mutex_t me_wmutex;
#if defined(FDS_WINDOWS)
    // Half-initialized name of mutexes, to be completed by MUTEXNAME()
    char me_mutexname[sizeof(MUTEXNAME_PREFIX) + 11];
#endif
#endif
    void* me_userctx;                 // User-settable context
    FDS_assert_func* me_assert_func;  // Callback for assertion failures
};

auto ESECT fds_env_share_locks(FDS_env* env, int* excl) -> int;
auto fds_env_sync0(FDS_env* env, int force, pgno_t numpgs) -> int;

auto fds_env_read_header(FDS_env* env, int prev, FDS_meta* meta) -> int;
auto fds_env_pick_meta(const FDS_env* env) -> FDS_meta*;
auto fds_env_write_meta(FDS_txn* txn) -> int;
void fds_env_close0(FDS_env* env, int excl);

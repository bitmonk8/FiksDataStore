#pragma once

#include "lmdb.h"
#include "midl.h"

// Forward declarations of types defined in Lib/*.h files
struct MDB_cursor;
struct MDB_db;
struct MDB_dbx;
struct MDB_env;
struct MDB_meta;
struct MDB_node;
struct MDB_page;
struct MDB_page2;
struct MDB_pgstate;
struct MDB_reader;
struct MDB_rxbody;
struct MDB_txbody;
struct MDB_txn;
struct MDB_txninfo;
enum Pidlock_op : int;

#ifndef _GNU_SOURCE
#define GNU_SOURCE 1
#endif

#if defined(__WIN64__)
#define _FILE_OFFSET_BITS 64
#endif

typedef unsigned long long mdb_hash_t;

#ifdef _WIN32

#include <malloc.h>
#include <wchar.h>  // get wcscpy()
#include <windows.h>

// getpid() returns int; MinGW defines pid_t but MinGW64 typedefs it
// as int64 which is wrong. MSVC doesn't define it at all, so just
// don't use it.
#define MDB_PID_T int
#define MDB_THR_T DWORD

#include <sys/stat.h>
#include <sys/types.h>

#ifdef __GNUC__
#include <sys/param.h>
#else
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#define BYTE_ORDER LITTLE_ENDIAN
#ifndef SSIZE_MAX
#define SSIZE_MAX INT_MAX
#endif
#endif

#define MDB_OFF_T int64_t

#else

#include <sys/stat.h>
#include <sys/types.h>
#define MDB_PID_T pid_t
#define MDB_THR_T pthread_t
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/uio.h>
#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#include <fcntl.h>
#define MDB_OFF_T off_t

#endif

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _MSC_VER
#include <io.h>
typedef SSIZE_T ssize_t;
#else
#include <unistd.h>
#endif

#if !(defined(BYTE_ORDER) || defined(__BYTE_ORDER))
#include <netinet/in.h>
#include <resolv.h>  // defines BYTE_ORDER on HPUX and Solaris
#endif

#if defined(__FreeBSD__) && defined(__FreeBSD_version) && __FreeBSD_version >= 1100110
#define MDB_USE_POSIX_MUTEX 1
#elif defined(__APPLE__) || defined(BSD) || defined(__FreeBSD_kernel__)
#if !(defined(MDB_USE_POSIX_MUTEX) || defined(MDB_USE_POSIX_SEM))
#define MDB_USE_SYSV_SEM 1
#endif
#endif

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#ifdef MDB_USE_POSIX_SEM
#include <semaphore.h>
#elif defined(MDB_USE_SYSV_SEM)
#include <sys/ipc.h>
#include <sys/sem.h>
#ifdef _SEM_SEMUN_UNDEFINED
union semun
{
    int val;
    struct semid_ds* buf;
    unsigned short* array;
};
#endif  // _SEM_SEMUN_UNDEFINED
#else
#define MDB_USE_POSIX_MUTEX 1
#endif  // MDB_USE_POSIX_SEM
#endif  // !_WIN32

#if defined(_WIN32) + defined(MDB_USE_POSIX_SEM) + defined(MDB_USE_SYSV_SEM) + defined(MDB_USE_POSIX_MUTEX) != 1
#error "Ambiguous shared-lock implementation"
#endif

#ifdef USE_VALGRIND
#include <valgrind/memcheck.h>
#define VGMEMP_CREATE(h, r, z) VALGRIND_CREATE_MEMPOOL(h, r, z)
#define VGMEMP_ALLOC(h, a, s) VALGRIND_MEMPOOL_ALLOC(h, a, s)
#define VGMEMP_FREE(h, a) VALGRIND_MEMPOOL_FREE(h, a)
#define VGMEMP_DESTROY(h) VALGRIND_DESTROY_MEMPOOL(h)
#define VGMEMP_DEFINED(a, s) VALGRIND_MAKE_MEM_DEFINED(a, s)
#else
#define VGMEMP_CREATE(h, r, z)
#define VGMEMP_ALLOC(h, a, s)
#define VGMEMP_FREE(h, a)
#define VGMEMP_DESTROY(h)
#define VGMEMP_DEFINED(a, s)
#endif

#ifndef BYTE_ORDER
#if (defined(_LITTLE_ENDIAN) || defined(_BIG_ENDIAN)) && !(defined(_LITTLE_ENDIAN) && defined(_BIG_ENDIAN))
// Solaris just defines one or the other
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#ifdef _LITTLE_ENDIAN
#define BYTE_ORDER LITTLE_ENDIAN
#else
#define BYTE_ORDER BIG_ENDIAN
#endif
#else
#define BYTE_ORDER __BYTE_ORDER
#endif
#endif

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#endif

#if defined(__i386) || defined(__x86_64) || defined(_M_IX86)
#define MISALIGNED_OK 1
#endif

#if (BYTE_ORDER == LITTLE_ENDIAN) == (BYTE_ORDER == BIG_ENDIAN)
#error "Unknown or unsupported endianness (BYTE_ORDER)"
#elif (-6 & 5) || CHAR_BIT != 8 || UINT_MAX != 0xffffffff || MDB_SIZE_MAX % UINT_MAX
#error "Two's complement, reasonably sized integer types, please"
#endif

#if (((__clang_major__ << 8) | __clang_minor__) >= 0x0302) || (((__GNUC__ << 8) | __GNUC_MINOR__) >= 0x0403)
// Mark infrequently used env functions as cold. This puts them in a separate
// section, and optimizes them for size
#define ESECT __attribute__((cold))
#else
// On older compilers, use a separate section
#ifdef __GNUC__
#ifdef __APPLE__
#define ESECT __attribute__((section("__TEXT,text_env")))
#else
#define ESECT __attribute__((section("text_env")))
#endif
#else
#define ESECT
#endif
#endif

#ifdef _WIN32
#define CALL_CONV WINAPI
#else
#define CALL_CONV
#endif

// LMDB Internals
//
// Compatibility Macros
// A bunch of macros to minimize the amount of platform-specific ifdefs
// needed throughout the rest of the code. When the features this library
// needs are similar enough to POSIX to be hidden in a one-or-two line
// replacement, this macro approach is used.
//

#ifdef __GLIBC__
#define GLIBC_VER ((__GLIBC__ << 16) | __GLIBC_MINOR__)
#endif

#define Z MDB_FMT_Z     // printf/scanf format modifier for size_t
#define Yu MDB_PRIy(u)  // printf format for mdb_size_t
#define Yd MDB_PRIy(d)  // printf format for 'signed mdb_size_t'

#if defined(MDB_USE_POSIX_MUTEX)
// glibc < 2.12 only provided _np API
#if (defined(__GLIBC__) && GLIBC_VER < 0x02000c) || (defined(PTHREAD_MUTEX_ROBUST_NP) && !defined(PTHREAD_MUTEX_ROBUST))
#define PTHREAD_MUTEX_ROBUST PTHREAD_MUTEX_ROBUST_NP
#define pthread_mutexattr_setrobust(attr, flag) pthread_mutexattr_setrobust_np(attr, flag)
#define pthread_mutex_consistent(mutex) pthread_mutex_consistent_np(mutex)
#endif
#endif  // MDB_USE_POSIX_MUTEX

#ifdef _WIN32
#define MDB_PIDLOCK 0
#define THREAD_RET DWORD
#define pthread_t HANDLE
#define pthread_mutex_t HANDLE
#define pthread_cond_t HANDLE
typedef HANDLE mdb_mutex_t, mdb_mutexref_t;
#define pthread_key_t DWORD
#define pthread_self() GetCurrentThreadId()
#define pthread_key_create(x, y) ((*(x) = TlsAlloc()) == TLS_OUT_OF_INDEXES ? ErrCode() : 0)
#define pthread_key_delete(x) TlsFree(x)
#define pthread_getspecific(x) TlsGetValue(x)
#define pthread_setspecific(x, y) (TlsSetValue(x, y) ? 0 : ErrCode())
#define pthread_mutex_unlock(x) ReleaseMutex(*(x))
#define pthread_mutex_lock(x) WaitForSingleObject(*(x), INFINITE)
#define pthread_cond_signal(x) SetEvent(*(x))
#define pthread_cond_wait(cond, mutex)                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        SignalObjectAndWait(*(mutex), *(cond), INFINITE, FALSE);                                                       \
        WaitForSingleObject(*(mutex), INFINITE);                                                                       \
    } while (0)
#define THREAD_CREATE(thr, start, arg) (((thr) = CreateThread(NULL, 0, start, arg, 0, NULL)) ? 0 : ErrCode())
#define THREAD_FINISH(thr) (WaitForSingleObject(thr, INFINITE) ? ErrCode() : 0)
#define LOCK_MUTEX0(mutex) WaitForSingleObject(mutex, INFINITE)
#define UNLOCK_MUTEX(mutex) ReleaseMutex(mutex)
#define mdb_mutex_consistent(mutex) 0
#define getpid() GetCurrentProcessId()
#define ErrCode() GetLastError()
#define GET_PAGESIZE(x)                                                                                                \
    {                                                                                                                  \
        SYSTEM_INFO si;                                                                                                \
        GetSystemInfo(&si);                                                                                            \
        (x) = si.dwPageSize;                                                                                           \
    }
#define close(fd) (CloseHandle(fd) ? 0 : -1)
#define munmap(ptr, len) UnmapViewOfFile(ptr)
#ifdef PROCESS_QUERY_LIMITED_INFORMATION
#define MDB_PROCESS_QUERY_LIMITED_INFORMATION PROCESS_QUERY_LIMITED_INFORMATION
#else
#define MDB_PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif
#else
#define THREAD_RET void*
#define THREAD_CREATE(thr, start, arg) pthread_create(&thr, NULL, start, arg)
#define THREAD_FINISH(thr) pthread_join(thr, NULL)

// For MDB_LOCK_FORMAT: True if readers take a pid lock in the lockfile
#define MDB_PIDLOCK 1

#ifdef MDB_USE_POSIX_SEM

typedef sem_t *mdb_mutex_t, *mdb_mutexref_t;
#define LOCK_MUTEX0(mutex) mdb_sem_wait(mutex)
#define UNLOCK_MUTEX(mutex) sem_post(mutex)

int mdb_sem_wait(sem_t* sem);

#elif defined MDB_USE_SYSV_SEM

struct mdb_mutex
{
    int semid;
    int semnum;
    int* locked;
};

typedef mdb_mutex mdb_mutex_t[1];
typedef mdb_mutex* mdb_mutexref_t;

#define LOCK_MUTEX0(mutex) mdb_sem_wait(mutex)
#define UNLOCK_MUTEX(mutex)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        struct sembuf sb = {0, 1, SEM_UNDO};                                                                           \
        sb.sem_num = (mutex)->semnum;                                                                                  \
        *(mutex)->locked = 0;                                                                                          \
        semop((mutex)->semid, &sb, 1);                                                                                 \
    } while (0)

int mdb_sem_wait(mdb_mutexref_t sem);

#define mdb_mutex_consistent(mutex) 0

#else  // MDB_USE_POSIX_MUTEX:
// Shared mutex/semaphore as the original is stored.
//
// Not for copies. Instead it can be assigned to an mdb_mutexref_t.
// When mdb_mutexref_t is a pointer and mdb_mutex_t is not, then it
// is array[size 1] so it can be assigned to the pointer.
typedef pthread_mutex_t mdb_mutex_t[1];
// Reference to an mdb_mutex_t
typedef pthread_mutex_t* mdb_mutexref_t;
// Lock the reader or writer mutex.
// Returns 0 or a code to give mdb_mutex_failed(), as in LOCK_MUTEX().
//
#define LOCK_MUTEX0(mutex) pthread_mutex_lock(mutex)
// Unlock the reader or writer mutex.
//
#define UNLOCK_MUTEX(mutex) pthread_mutex_unlock(mutex)
// Mark mutex-protected data as repaired, after death of previous owner.
//
#define mdb_mutex_consistent(mutex) pthread_mutex_consistent(mutex)
#endif  // MDB_USE_POSIX_SEM || MDB_USE_SYSV_SEM

// Get the error code for the last failed system function.
//
#define ErrCode() errno

// An abstraction for a file handle.
// On POSIX systems file handles are small integers. On Windows
// they're opaque pointers.
//
#define HANDLE int

// A value for an invalid file handle.
// Mainly used to initialize file variables and signify that they are
// unused.
//
#define INVALID_HANDLE_VALUE (-1)

// Get the size of a memory page for the system.
// This is the basic size that the platform's memory manager uses, and is
// fundamental to the use of memory-mapped files.
//
#define GET_PAGESIZE(x) ((x) = sysconf(_SC_PAGE_SIZE))
#endif

#ifdef MDB_USE_SYSV_SEM
#define MNAME_LEN (sizeof(int))
#else
#define MNAME_LEN (sizeof(pthread_mutex_t))
#endif

//
// The version number for a database's lockfile format.
#define MDB_LOCK_VERSION 2
// Number of bits representing MDB_LOCK_VERSION in MDB_LOCK_FORMAT.
// The remaining bits must leave room for MDB_lock_desc.
//
#define MDB_LOCK_VERSION_BITS 12

// The max size of a key we can write, or 0 for computed max.
//
// This macro should normally be left alone or set to 0.
// Note that a database with big keys cannot be
// reliably modified by a liblmdb which uses a smaller max.
// The default is 511 for backwards compat.
//
// Other values are allowed, for backwards compat. However:
// A value bigger than the computed max can break if you do not
// know what you are doing, and liblmdb <= 0.9.10 can break when
// modifying a DB with keys bigger than its max.
//
// Keys must fit on a node in a regular page.
//
#ifndef MDB_MAXKEYSIZE
#define MDB_MAXKEYSIZE 511
#endif

// The maximum size of a key we can write to the environment.
#if MDB_MAXKEYSIZE
#define ENV_MAXKEY(env) (MDB_MAXKEYSIZE)
#else
#define ENV_MAXKEY(env) ((env)->me_maxkey)
#endif

// The maximum size of a data item.
//
// We only store a 32 bit value for node sizes.
//
#define MAXDATASIZE 0xffffffffUL

// An invalid page number.
// Mainly used to denote an empty tree.
//
#define P_INVALID (~(pgno_t)0)

// Default size of memory map.
// This is certainly too small for any actual applications. Apps should always set
// the size explicitly using mdb_env_set_mapsize().
//
#define DEFAULT_MAPSIZE 1048576

// Reader Lock Table
// Readers don't acquire any locks for their data access. Instead, they
// simply record their transaction ID in the reader table. The reader
// mutex is needed just to find an empty slot in the reader table. The
// slot's address is saved in thread-specific data so that subsequent read
// transactions started by the same thread need no further locking to proceed.
//
// If MDB_NOTLS is set, the slot address is not saved in thread-specific data.
//
// No reader table is used if the database is on a read-only filesystem, or
// if MDB_NOLOCK is set.
//
// Since the database uses multi-version concurrency control, readers don't
// actually need any locking. This table is used to keep track of which
// readers are using data from which old transactions, so that we'll know
// when a particular old transaction is no longer in use. Old transactions
// that have discarded any data pages can then have those pages reclaimed
// for use by a later write transaction.
//
// The lock table is constructed such that reader slots are aligned with the
// processor's cache line size. Any slot is only ever used by one thread.
// This alignment guarantees that there will be no contention or cache
// thrashing as threads update their own slot info, and also eliminates
// any need for locking when accessing a slot.
//
// A writer thread will scan every slot in the table to determine the oldest
// outstanding reader transaction. Any freed pages older than this will be
// reclaimed by the writer. The writer doesn't use any locks when scanning
// this table. This means that there's no guarantee that the writer will
// see the most up-to-date reader info, but that's not required for correct
// operation - all we need is to know the upper bound on the oldest reader,
// we don't care at all about the newest reader. So the only consequence of
// reading stale information here is that old pages might hang around a
// while longer before being reclaimed. That's actually good anyway, because
// the longer we delay reclaiming old pages, the more likely it is that a
// string of contiguous pages can be found after coalescing old pages from
// many old transactions together.
//
// Number of slots in the reader table.
// This value was chosen somewhat arbitrarily. 126 readers plus a
// couple mutexes fit exactly into 8KB on my development machine.
// Applications should set the table size using mdb_env_set_maxreaders().
//
#define DEFAULT_READERS 126

// The size of a CPU cache line in bytes. We want our lock structures
// aligned to this size to avoid false cache line sharing in the
// lock table.
// This value works for most CPUs. For Itanium this should be 128.
//
#ifndef CACHELINE
#define CACHELINE 64
#endif

// Enough space for 2^32 nodes with minimum of 2 keys per node. I.e., plenty.
// At 4 keys per node, enough for 2^64 nodes, so there's probably no need to
// raise this on a 64 bit machine.
//
#define CURSOR_STACK 32

// Lockfile format signature: version, features and field layout
#define MDB_LOCK_FORMAT                                                                                                \
    ((uint32_t)(((MDB_LOCK_VERSION) % (1U << MDB_LOCK_VERSION_BITS)) + MDB_lock_desc * (1U << MDB_LOCK_VERSION_BITS)))

// Lock type and layout. Values 0-119. _WIN32 implies MDB_PIDLOCK.
// Some low values are reserved for future tweaks.
//
#ifdef _WIN32
#define MDB_LOCK_TYPE (0 + ALIGNOF2(mdb_hash_t) / 8 % 2)
#elif defined MDB_USE_POSIX_SEM
#define MDB_LOCK_TYPE (4 + ALIGNOF2(mdb_hash_t) / 8 % 2)
#elif defined MDB_USE_SYSV_SEM
#define MDB_LOCK_TYPE (8)
#elif defined MDB_USE_POSIX_MUTEX
// We do not know the inside of a POSIX mutex and how to check if mutexes
// used by two executables are compatible. Just check alignment and size.
//
#define MDB_LOCK_TYPE (10 + LOG2_MOD(ALIGNOF2(pthread_mutex_t), 5) + sizeof(pthread_mutex_t) / 4U % 22 * 5)
#endif

enum
{
    // Magic number for lockfile layout and features.
    MDB_lock_desc = 42
};
//

#define MDB_VALID 0x8000  // DB handle is valid, for me_dbflags
#define PERSISTENT_FLAGS (0xffff & ~(MDB_VALID))
// mdb_dbi_open() flags
#define VALID_FLAGS (MDB_REVERSEKEY | MDB_CREATE)

// Handle for the DB used to track free pages.
#define FREE_DBI 0
// Handle for the default DB.
#define MAIN_DBI 1
// Number of DBs in metapage (free and main) - also hardcoded elsewhere
#define CORE_DBS 2

// Number of meta pages - also hardcoded elsewhere
#define NUM_METAS 2

// A transaction ID.
// See struct MDB_txn.mt_txnid for details.
//
typedef MDB_ID txnid_t;

// Used for offsets within a single page.
// Since memory pages are typically 4 or 8KB in size, 12-13 bits,
// this is plenty.
//
typedef uint16_t indx_t;

// max bytes to write in one call
static_assert(sizeof(ssize_t) == 8);  // MAX_WRITE depends on 64 bit architecture
#define MAX_WRITE 0x40000000U

// A page number in the database.
// Note that 64 bit page numbers are overkill, since pages themselves
// already represent 12-13 bits of addressable memory, and the OS will
// always limit applications to a maximum of 63 bits of address space.
//
// In the MDB_node structure, we only store 48 bits of this value,
// which thus limits us to only 60 bits of addressable data.
//
typedef MDB_ID pgno_t;

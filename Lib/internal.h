#pragma once

#include "fds.h"
#include "midl.h"

#if defined(FDS_WINDOWS) + defined(FDS_LINUX) + defined(FDS_MACOS) != 1
#error "Ambiguous target operating system"
#endif

// Forward declarations of types defined in Lib/*.h files
struct FDS_cursor;
struct FDS_db;
struct FDS_dbx;
struct FDS_env;
struct FDS_meta;
struct FDS_node;
struct FDS_page;
struct FDS_page2;
struct FDS_pgstate;
struct FDS_reader;
struct FDS_rxbody;
struct FDS_txbody;
struct FDS_txn;
struct FDS_txninfo;
enum Pidlock_op : int;

using fds_hash_t = unsigned long long;

#ifdef FDS_WINDOWS

#include <malloc.h>
#include <wchar.h>
#include <windows.h>

// getpid() returns int; MinGW defines pid_t but MinGW64 typedefs it
// as int64 which is wrong. MSVC doesn't define it at all, so just
// don't use it.
#define FDS_PID_T int
#define FDS_THR_T DWORD

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

#define FDS_OFF_T int64_t

#else

#include <sys/stat.h>
#include <sys/types.h>
#define FDS_PID_T pid_t
#define FDS_THR_T pthread_t
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/uio.h>
#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#include <fcntl.h>
#define FDS_OFF_T off_t

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
using ssize_t = SSIZE_T;
#else
#include <unistd.h>
#endif

#if !(defined(BYTE_ORDER) || defined(__BYTE_ORDER))
#include <netinet/in.h>
#include <resolv.h>  // defines BYTE_ORDER on HPUX and Solaris
#endif

#ifndef FDS_WINDOWS
#include <pthread.h>
#include <signal.h>
#endif  // !FDS_WINDOWS

#if defined(FDS_MACOS)
#include <sys/ipc.h>
#include <sys/sem.h>
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
constexpr int MISALIGNED_OK = 1;
#endif

#if (BYTE_ORDER == LITTLE_ENDIAN) == (BYTE_ORDER == BIG_ENDIAN)
#error "Unknown or unsupported endianness (BYTE_ORDER)"
#elif (-6 & 5) || CHAR_BIT != 8 || UINT_MAX != 0xffffffff || SIZE_MAX % UINT_MAX
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

#ifdef FDS_WINDOWS
#define CALL_CONV WINAPI
#else
#define CALL_CONV
#endif

// FiksDataStore Internals
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

#ifdef _WIN32
#define FDS_FMT_Z "I"
#else
#define FDS_FMT_Z "z"  // printf/scanf format modifier for size_t
#endif

// #size_t printf formats, \b t = one of [diouxX] without quotes
#define FDS_PRIy(t) FDS_FMT_Z #t

#define Z FDS_FMT_Z     // printf/scanf format modifier for size_t
#define Yu FDS_PRIy(u)  // printf format for size_t
#define Yd FDS_PRIy(d)  // printf format for 'signed size_t'

#if defined(FDS_LINUX)
// glibc < 2.12 only provided _np API
#if (defined(__GLIBC__) && GLIBC_VER < 0x02000c) || (defined(PTHREAD_MUTEX_ROBUST_NP) && !defined(PTHREAD_MUTEX_ROBUST))
#define PTHREAD_MUTEX_ROBUST PTHREAD_MUTEX_ROBUST_NP
#define pthread_mutexattr_setrobust(attr, flag) pthread_mutexattr_setrobust_np(attr, flag)
#define pthread_mutex_consistent(mutex) pthread_mutex_consistent_np(mutex)
#endif
#endif  // FDS_LINUX

#ifdef FDS_WINDOWS
#define THREAD_RET DWORD
#define pthread_t HANDLE
#define pthread_mutex_t HANDLE
#define pthread_cond_t HANDLE
using fds_mutex_t = HANDLE;
using fds_mutexref_t = HANDLE;
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
#define FDS_PROCESS_QUERY_LIMITED_INFORMATION PROCESS_QUERY_LIMITED_INFORMATION
#else
#define FDS_PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif
#else
#define THREAD_RET void*
#define THREAD_CREATE(thr, start, arg) pthread_create(&thr, NULL, start, arg)
#define THREAD_FINISH(thr) pthread_join(thr, NULL)

#if defined FDS_MACOS

struct fds_mutex
{
    int semid;
    int semnum;
    int* locked;
};

typedef fds_mutex fds_mutex_t[1];
typedef fds_mutex* fds_mutexref_t;

#else  // FDS_LINUX:
// Shared mutex/semaphore as the original is stored.
//
// Not for copies. Instead it can be assigned to an fds_mutexref_t.
// When fds_mutexref_t is a pointer and fds_mutex_t is not, then it
// is array[size 1] so it can be assigned to the pointer.
typedef pthread_mutex_t fds_mutex_t[1];
// Reference to an fds_mutex_t
typedef pthread_mutex_t* fds_mutexref_t;

#endif  // FDS_MACOS

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

#ifdef FDS_MACOS
#define MNAME_LEN (sizeof(int))
#else
#define MNAME_LEN (sizeof(pthread_mutex_t))
#endif

// The max size of a key we can write, or 0 for computed max.
//
// This macro should normally be left alone or set to 0.
// Note that a database with big keys cannot be
// reliably modified by a FiksDataStore which uses a smaller max.
// The default is 511 for backwards compat.
//
// Other values are allowed, for backwards compat. However:
// A value bigger than the computed max can break if you do not
// know what you are doing, and FiksDataStore <= 0.9.10 can break when
// modifying a DB with keys bigger than its max.
//
// Keys must fit on a node in a regular page.
//
#ifndef FDS_MAXKEYSIZE
#define FDS_MAXKEYSIZE 511
#endif

// The maximum size of a key we can write to the environment.
#if FDS_MAXKEYSIZE
#define ENV_MAXKEY(env) (FDS_MAXKEYSIZE)
#else
#define ENV_MAXKEY(env) ((env)->me_maxkey)
#endif

// An invalid page number.
// Mainly used to denote an empty tree.
//
#define P_INVALID (~(pgno_t)0)

// The size of a CPU cache line in bytes. We want our lock structures
// aligned to this size to avoid false cache line sharing in the
// lock table.
// This value works for most CPUs. For Itanium this should be 128.
//
#ifndef CACHELINE
#define CACHELINE 64
#endif

// Lock type and layout. Values 0-119.
// Some low values are reserved for future tweaks.
//
#ifdef FDS_WINDOWS
#define FDS_LOCK_TYPE (0 + ALIGNOF2(fds_hash_t) / 8 % 2)
#elif defined FDS_MACOS
#define FDS_LOCK_TYPE (8)
#elif defined FDS_LINUX
// We do not know the inside of a POSIX mutex and how to check if mutexes
// used by two executables are compatible. Just check alignment and size.
//
#define FDS_LOCK_TYPE (10 + LOG2_MOD(ALIGNOF2(pthread_mutex_t), 5) + sizeof(pthread_mutex_t) / 4U % 22 * 5)
#endif

constexpr int FDS_VALID = 0x8000;  // DB handle is valid, for me_dbflags
#define PERSISTENT_FLAGS (0xffff & ~(FDS_VALID))

// Handle for the DB used to track free pages.
constexpr int FREE_DBI = 0;
// Handle for the default DB.
constexpr int MAIN_DBI = 1;
// Number of DBs in metapage (free and main) - also hardcoded elsewhere
constexpr int CORE_DBS = 2;

// Number of meta pages - also hardcoded elsewhere
constexpr int NUM_METAS = 2;

// A transaction ID.
// See struct FDS_txn.mt_txnid for details.
//
using txnid_t = FDS_ID;

// Used for offsets within a single page.
// Since memory pages are typically 4 or 8KB in size, 12-13 bits,
// this is plenty.
//
using indx_t = uint16_t;

// A page number in the database.
// Note that 64 bit page numbers are overkill, since pages themselves
// already represent 12-13 bits of addressable memory, and the OS will
// always limit applications to a maximum of 63 bits of address space.
//
// In the FDS_node structure, we only store 48 bits of this value,
// which thus limits us to only 60 bits of addressable data.
//
using pgno_t = FDS_ID;

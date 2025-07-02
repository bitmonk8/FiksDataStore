// @file fds.h
// @brief FiksDataStore memory-mapped database library

// @mainpage FiksDataStore Memory-Mapped Database Manager

// @section intro_sec Introduction
// FiksDataStore is a Btree-based database management library modeled loosely on the
// BerkeleyDB API, but much simplified. The entire database is exposed
// in a memory map, and all data fetches return data directly
// from the mapped memory, so no malloc's or memcpy's occur during
// data fetches. As such, the library is extremely simple because it
// requires no page caching layer of its own, and it is extremely high
// performance and memory-efficient. It is also fully transactional with
// full ACID semantics, and when the memory map is read-only, the
// database integrity cannot be corrupted by stray pointer writes from
// application code.

// FiksDataStore is built upon the foundation of LMDB (Lightning Memory-Mapped Database),
// originally created by Howard Chu at Symas Corporation. We gratefully acknowledge
// their contributions. For the original LMDB project, please visit
// https://github.com/LMDB/lmdb.

// The library is fully thread-aware and supports concurrent read/write
// access from multiple processes and threads. Data pages use a copy-on-
// write strategy so no active data pages are ever overwritten, which
// also provides resistance to corruption and eliminates the need of any
// special recovery procedures after a system crash. Writes are fully
// serialized; only one write transaction may be active at a time, which
// guarantees that writers can never deadlock. The database structure is
// multi-versioned so readers run with no locks; writers cannot block
// readers, and readers don't block writers.

// Unlike other well-known database mechanisms which use either write-ahead
// transaction logs or append-only data writes, FiksDataStore requires no maintenance
// during operation. Both write-ahead loggers and append-only databases
// require periodic checkpointing and/or compaction of their log or database
// files otherwise they grow without bound. FiksDataStore tracks free pages within
// the database and re-uses them for new write operations, so the database
// size does not grow without bound in normal use.

// The memory map can be used as a read-only or read-write map. It is
// read-only by default as this provides total immunity to corruption.
// Using read-write mode offers much higher write performance, but adds
// the possibility for stray application writes thru pointers to silently
// corrupt the database. Of course if your application code is known to
// be bug-free (...) then this is not an issue.

// If this is your first time using a transactional embedded key/value
// store, you may find the \ref starting page to be helpful.

// @section caveats_sec Caveats
// Troubleshooting the lock file, plus semaphores on BSD systems:

// - A broken lockfile can cause sync issues.
// Stale reader transactions left behind by an aborted program
// cause further writes to grow the database quickly, and
// stale locks can block further operation.

// Fix: Check for stale readers periodically, using the
// #fds_reader_check function or the \ref fds_stat_1 "fds_stat" tool.
// Stale writers will be cleared automatically on most systems:
// - Windows - automatic
// - BSD, systems using SysV semaphores - automatic
// - Linux, systems using POSIX mutexes with Robust option - automatic
// Otherwise just make all programs using the database close it;
// the lockfile is always reset on first open of the environment.

// - On BSD systems or others configured with FDS_MACOS,
// startup can fail due to semaphores owned by another userid.

// Fix: Open and close the database as the user which owns the
// semaphores (likely last user) or as root, while no other
// process is using the database.

// Restrictions/caveats (in addition to those listed for some functions):

// - Only the database owner should normally use the database on
// BSD systems.
// Multiple users can cause startup to fail later, as noted above.

// - There is normally no pure read-only mode, since readers need write
// access to locks and lock file. Exceptions: On read-only filesystems
// or with the #FDS_NOLOCK flag described under #fds_env_open().

// - A FiksDataStore configuration will often reserve considerable \b unused
// memory address space and maybe file size for future growth.
// This does not use actual memory or disk space, but users may need
// to understand the difference so they won't be scared off.

// - By default, in versions before 0.9.10, unused portions of the data
// file might receive garbage data from memory freed by other code.
// (This does not happen when using the #FDS_WRITEMAP flag.) As of
// 0.9.10 the default behavior is to initialize such memory before
// writing to the data file. Since there may be a slight performance
// cost due to this initialization, applications may disable it using
// the #FDS_NOMEMINIT flag. Applications handling sensitive data
// which must not be written should not use this flag. This flag is
// irrelevant when using #FDS_WRITEMAP.

// - A thread can only use one transaction at a time, plus any child
// transactions. Each transaction belongs to one thread. See below.
// The #FDS_NOTLS flag changes this for read-only transactions.

// - Use an FDS_env* in the process which opened it, not after fork().

// - Do not have open a FiksDataStore database twice in the same process at
// the same time. Not even from a plain open() call - close()ing it
// breaks fcntl() advisory locking. (It is OK to reopen it after
// fork() - exec*(), since the lockfile has FD_CLOEXEC set.)

// - Avoid long-lived transactions. Read transactions prevent
// reuse of pages freed by newer write transactions, thus the
// database can grow quickly. Write transactions prevent
// other write transactions, since writes are serialized.

// - Avoid suspending a process with active transactions. These
// would then be "long-lived" as above. Also read transactions
// suspended when writers commit could sometimes see wrong data.

// ...when several processes can use a database concurrently:

// - Avoid aborting a process with an active transaction.
// The transaction becomes "long-lived" as above until a check
// for stale readers is performed or the lockfile is reset,
// since the process may not remove it from the lockfile.

// This does not apply to write transactions if the system clears
// stale writers, see above.

// - If you do that anyway, do a periodic check for stale readers. Or
// close the environment once in a while, so the lockfile can get reset.

// - Do not use FiksDataStore databases on remote filesystems, even between
// processes on the same host. This breaks flock() on some OSes,
// possibly memory map sync, and certainly sync between programs
// on different hosts.

// - Opening a database can fail if another process is opening or
// closing it at exactly the same time.

// @author Howard Chu, Symas Corporation.

// @copyright Copyright 2011-2021 Howard Chu, Symas Corp. All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted only as authorized by the OpenLDAP
// Public License.

// A copy of this license is available in the file LICENSE in the
// top-level directory of the distribution or, alternatively, at
// .

// @par Derived From:
// This code is derived from btree.c written by Martin Hedenfalk.

// Copyright (c) 2009, 2010 Martin Hedenfalk

// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Unix permissions for creating files, or dummy definition for Windows
#ifdef _MSC_VER
using fds_mode_t = int;
#else
typedef mode_t fds_mode_t;
#endif

// Unsigned type used for mapsize, entry counts and page/transaction IDs.
// It size_t, hence the name.
using fds_size_t = size_t;

#define FDS_SIZE_MAX SIZE_MAX  // max #fds_size_t

// An abstraction for a file handle.
#ifdef _WIN32
using fds_filehandle_t = void*;
#else
using fds_filehandle_t = int;
#endif

// Library major version
enum
{
    FDS_VERSION_MAJOR = 0,
    // Library minor version
    FDS_VERSION_MINOR = 9,
    // Library patch version
    FDS_VERSION_PATCH = 70
};

// Combine args a,b,c into a single integer for easy version comparisons
#define FDS_VERINT(a, b, c) (((a) << 24) | ((b) << 16) | (c))

// The full library version as a single integer
#define FDS_VERSION_FULL FDS_VERINT(FDS_VERSION_MAJOR, FDS_VERSION_MINOR, FDS_VERSION_PATCH)

// The release date of this library version
#define FDS_VERSION_DATE "December 19, 2015"

// A stringifier for the version info
#define FDS_VERSTR(a, b, c, d) "FiksDataStore " #a "." #b "." #c ": (" d ")"
// A helper for the stringifier macro
#define FDS_VERFOO(a, b, c, d) FDS_VERSTR(a, b, c, d)

// The full library version as a C string
#define FDS_VERSION_STRING FDS_VERFOO(FDS_VERSION_MAJOR, FDS_VERSION_MINOR, FDS_VERSION_PATCH, FDS_VERSION_DATE)

// @brief Opaque structure for a database environment.
// A DB environment supports multiple databases, all residing in the same
// shared-memory map.
struct FDS_env;

// @brief Opaque structure for a transaction handle.
// All database operations require a transaction handle. Transactions may be
// read-only or read-write.
struct FDS_txn;

// @brief A handle for an individual database in the DB environment.
using FDS_dbi = unsigned int;

// @brief Opaque structure for navigating through a database
struct FDS_cursor;

// @brief Generic structure used for passing keys and data in and out
// of the database.
// Values returned from the database are valid only until a subsequent
// update operation, or the end of the transaction. Do not modify or
// free them, they commonly point into the database itself.
// Key sizes must be between 1 and #fds_env_get_maxkeysize() inclusive.
// Data items can in theory be from 0 to 0xffffffff bytes long.
struct FDS_val
{
    size_t mv_size;  // size of the data item
    void* mv_data;   // address of the data item
};

// @brief A callback function used to compare two keys in a database
using FDS_cmp_func = int (*)(const FDS_val* a, const FDS_val* b);

// @defgroup fds_env Environment Flags
// @{

// no environment directory
enum
{
    FDS_NOSUBDIR = 0x4000,
    // don't fsync after commit
    FDS_NOSYNC = 0x10000,
    // read only
    FDS_RDONLY = 0x20000,
    // don't fsync metapage after commit
    FDS_NOMETASYNC = 0x40000,
    // use writable mmap
    FDS_WRITEMAP = 0x80000,
    // use asynchronous msync when #FDS_WRITEMAP is used
    FDS_MAPASYNC = 0x100000,
    // tie reader locktable slots to #FDS_txn objects instead of to threads
    FDS_NOTLS = 0x200000,
    // don't do any locking, caller must manage their own locks
    FDS_NOLOCK = 0x400000,
    // don't do readahead (no effect on Windows)
    FDS_NORDAHEAD = 0x800000,
    // don't initialize malloc'd memory before writing to datafile
    FDS_NOMEMINIT = 0x1000000,
    // use the previous snapshot rather than the latest one
    FDS_PREVSNAPSHOT = 0x2000000
};
// @}

// @defgroup fds_dbi_open Database Flags
// @{

// use reverse string keys
enum
{
    FDS_REVERSEKEY = 0x02,
    // create DB if not already existing
    FDS_CREATE = 0x40000
};
// @}

// @defgroup fds_put Write Flags
// @{

// For put: Don't write if the key already exists.
enum
{
    FDS_NOOVERWRITE = 0x10,
    // For fds_cursor_put: overwrite the current key/data pair
    FDS_CURRENT = 0x40,
    // For put: Just reserve space for data, don't copy it. Return a
    // pointer to the reserved space.
    FDS_RESERVE = 0x10000,
    // Data is being appended, don't split full pages.
    FDS_APPEND = 0x20000
};
// @}

// @brief Cursor Get operations.
// This is the set of all operations for retrieving data
// using a cursor.
using FDS_cursor_op = enum FDS_cursor_op {
    FDS_FIRST,        // Position at first key/data item
    FDS_GET_CURRENT,  // Return key/data at current cursor position
    FDS_LAST,         // Position at last key/data item
    FDS_NEXT,         // Position at next data item
    FDS_PREV,         // Position at previous data item
    FDS_SET,          // Position at specified key
    FDS_SET_KEY,      // Position at specified key, return key + data
    FDS_SET_RANGE     // Position at first key greater than or equal to specified key.
};

// @defgroup errors Return Codes
// BerkeleyDB uses -30800 to -30999, we'll go under them
// @{

// Successful result
enum
{
    FDS_SUCCESS = 0,
    // key/data pair already exists
    FDS_KEYEXIST = (-30799),
    // key/data pair not found (EOF)
    FDS_NOTFOUND = (-30798),
    // Requested page not found - this usually indicates corruption
    FDS_PAGE_NOTFOUND = (-30797),
    // Located page was wrong type
    FDS_CORRUPTED = (-30796),
    // Update of meta page failed or environment had fatal error
    FDS_PANIC = (-30795),
    // Environment version mismatch
    FDS_VERSION_MISMATCH = (-30794),
    // File is not a valid FiksDataStore file
    FDS_INVALID = (-30793),
    // Environment mapsize reached
    FDS_MAP_FULL = (-30792),
    // Environment maxdbs reached
    FDS_DBS_FULL = (-30791),
    // Environment maxreaders reached
    FDS_READERS_FULL = (-30790),
    // Too many TLS keys in use - Windows only
    FDS_TLS_FULL = (-30789),
    // Txn has too many dirty pages
    FDS_TXN_FULL = (-30788),
    // Cursor stack too deep - internal error
    FDS_CURSOR_FULL = (-30787),
    // Page has not enough space - internal error
    FDS_PAGE_FULL = (-30786),
    // Database contents grew beyond environment mapsize
    FDS_MAP_RESIZED = (-30785),
    // Operation and DB incompatible, or DB type changed. This can mean:
    // Accessing a data record as a database, or vice versa.
    // The database was dropped and recreated with different flags.
    FDS_INCOMPATIBLE = (-30784),
    // Invalid reuse of reader locktable slot
    FDS_BAD_RSLOT = (-30783),
    // Transaction must abort, has a child, or is invalid
    FDS_BAD_TXN = (-30782),
    // Unsupported size of key/DB name/data
    FDS_BAD_VALSIZE = (-30781),
    // The specified DBI was changed unexpectedly
    FDS_BAD_DBI = (-30780),
    // Unexpected problem - txn should abort
    FDS_PROBLEM = (-30779)
};
// The last defined error code
#define FDS_LAST_ERRCODE FDS_PROBLEM
// @}

// @brief Statistics for a database in the environment
struct FDS_stat
{
    unsigned int ms_psize;  // Size of a database page.
    // This is currently the same for all databases.
    unsigned int ms_depth;         // Depth (height) of the B-tree
    fds_size_t ms_branch_pages;    // Number of internal (non-leaf) pages
    fds_size_t ms_leaf_pages;      // Number of leaf pages
    fds_size_t ms_overflow_pages;  // Number of overflow pages
    fds_size_t ms_entries;         // Number of data items
};

// @brief Information about the environment
struct FDS_envinfo
{
    fds_size_t me_mapsize;       // Size of the data memory map
    fds_size_t me_last_pgno;     // ID of the last used page
    fds_size_t me_last_txnid;    // ID of the last committed transaction
    unsigned int me_maxreaders;  // max reader slots in the environment
    unsigned int me_numreaders;  // max reader slots used in the environment
};

// @brief Return the FiksDataStore library version information.
// @param[out] major if non-NULL, the library major version number is copied here
// @param[out] minor if non-NULL, the library minor version number is copied here
// @param[out] patch if non-NULL, the library patch version number is copied here
// @retval "version string" The library version as a string
auto fds_version(int* major, int* minor, int* patch) -> const char*;

// @brief Return a string describing a given error code.
// This function is a superset of the ANSI C X3.159-1989 (ANSI C) strerror(3)
// function. If the error code is greater than or equal to 0, then the string
// returned by the system function strerror(3) is returned. If the error code
// is less than 0, an error string corresponding to the FiksDataStore library error is
// returned. See @ref errors for a list of FiksDataStore-specific error codes.
// @param[in] err The error code
// @retval "error message" The description of the error
auto fds_strerror(int err) -> const char*;

// @brief Create a FiksDataStore environment handle.
// This function allocates memory for a #FDS_env structure. To release
// the allocated memory and discard the handle, call #fds_env_close().
// Before the handle may be used, it must be opened using #fds_env_open().
// Various other options may also need to be set before opening the handle,
// e.g. #fds_env_set_mapsize(), #fds_env_set_maxreaders(), #fds_env_set_maxdbs(),
// depending on usage requirements.
// @param[out] env The address where the new handle will be stored
// @return A non-zero error value on failure and 0 on success.
auto fds_env_create(FDS_env** env) -> int;

// @brief Open an environment handle.
// If this function fails, #fds_env_close() must be called to discard the #FDS_env handle.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] path The directory in which the database files reside. This
// directory must already exist and be writable.
// @param[in] flags Special options for this environment. This parameter
// must be set to 0 or by bitwise OR'ing together one or more of the
// values described here.
// Flags set by fds_env_set_flags() are also used.
//
// #FDS_NOSUBDIR
// By default, FiksDataStore creates its environment in a directory whose
// pathname is given in \b path, and creates its data and lock files
// under that directory. With this option, \b path is used as-is for
// the database main data file. The database lock file is the \b path
// with "-lock" appended.
// #FDS_RDONLY
// Open the environment in read-only mode. No write operations will be
// allowed. FiksDataStore will still modify the lock file - except on read-only
// filesystems, where FiksDataStore does not use locks.
// #FDS_WRITEMAP
// Use a writeable memory map unless FDS_RDONLY is set. This uses
// fewer mallocs but loses protection from application bugs
// like wild pointer writes and other bad updates into the database.
// This may be slightly faster for DBs that fit entirely in RAM, but
// is slower for DBs larger than RAM.
// Incompatible with nested transactions.
// Do not mix processes with and without FDS_WRITEMAP on the same
// environment. This can defeat durability (#fds_env_sync etc).
// #FDS_NOMETASYNC
// Flush system buffers to disk only once per transaction, omit the
// metadata flush. Defer that until the system flushes files to disk,
// or next non-FDS_RDONLY commit or #fds_env_sync(). This optimization
// maintains database integrity, but a system crash may undo the last
// committed transaction. I.e. it preserves the ACI (atomicity,
// consistency, isolation) but not D (durability) database property.
// This flag may be changed at any time using #fds_env_set_flags().
// #FDS_NOSYNC
// Don't flush system buffers to disk when committing a transaction.
// This optimization means a system crash can corrupt the database or
// lose the last transactions if buffers are not yet flushed to disk.
// The risk is governed by how often the system flushes dirty buffers
// to disk and how often #fds_env_sync() is called. However, if the
// filesystem preserves write order and the #FDS_WRITEMAP flag is not
// used, transactions exhibit ACI (atomicity, consistency, isolation)
// properties and only lose D (durability). I.e. database integrity
// is maintained, but a system crash may undo the final transactions.
// Note that (#FDS_NOSYNC | #FDS_WRITEMAP) leaves the system with no
// hint for when to write transactions to disk, unless #fds_env_sync()
// is called. (#FDS_MAPASYNC | #FDS_WRITEMAP) may be preferable.
// This flag may be changed at any time using #fds_env_set_flags().
// #FDS_MAPASYNC
// When using #FDS_WRITEMAP, use asynchronous flushes to disk.
// As with #FDS_NOSYNC, a system crash can then corrupt the
// database or lose the last transactions. Calling #fds_env_sync()
// ensures on-disk database integrity until next commit.
// This flag may be changed at any time using #fds_env_set_flags().
// #FDS_NOTLS
// Don't use Thread-Local Storage. Tie reader locktable slots to
// #FDS_txn objects instead of to threads. I.e. #fds_txn_reset() keeps
// the slot reserved for the #FDS_txn object. A thread may use parallel
// read-only transactions. A read-only transaction may span threads if
// the user synchronizes its use. Applications that multiplex many
// user threads over individual OS threads need this option. Such an
// application must also serialize the write transactions in an OS
// thread, since FiksDataStore's write locking is unaware of the user threads.
// #FDS_NOLOCK
// Don't do any locking. If concurrent access is anticipated, the
// caller must manage all concurrency itself. For proper operation
// the caller must enforce single-writer semantics, and must ensure
// that no readers are using old transactions while a writer is
// active. The simplest approach is to use an exclusive lock so that
// no readers may be active at all when a writer begins.
// #FDS_NORDAHEAD
// Turn off readahead. Most operating systems perform readahead on
// read requests by default. This option turns it off if the OS
// supports it. Turning it off may help random read performance
// when the DB is larger than RAM and system RAM is full.
// The option is not implemented on Windows.
// #FDS_NOMEMINIT
// Don't initialize malloc'd memory before writing to unused spaces
// in the data file. By default, memory for pages written to the data
// file is obtained using malloc. While these pages may be reused in
// subsequent transactions, freshly malloc'd pages will be initialized
// to zeroes before use. This avoids persisting leftover data from other
// code (that used the heap and subsequently freed the memory) into the
// data file. Note that many other system libraries may allocate
// and free memory from the heap for arbitrary uses. E.g., stdio may
// use the heap for file I/O buffers. This initialization step has a
// modest performance cost so some applications may want to disable
// it using this flag. This option can be a problem for applications
// which handle sensitive data like passwords, and it makes memory
// checkers like Valgrind noisy. This flag is not needed with #FDS_WRITEMAP,
// which writes directly to the mmap instead of using malloc for pages. The
// initialization is also skipped if #FDS_RESERVE is used; the
// caller is expected to overwrite all of the memory that was
// reserved in that case.
// This flag may be changed at any time using #fds_env_set_flags().
// #FDS_PREVSNAPSHOT
// Open the environment with the previous snapshot rather than the latest
// one. This loses the latest transaction, but may help work around some
// types of corruption. If opened with write access, this must be the
// only process using the environment. This flag is automatically reset
// after a write transaction is successfully committed.
//
// @param[in] mode The UNIX permissions to set on created files and semaphores.
// This parameter is ignored on Windows.
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// #FDS_VERSION_MISMATCH - the version of the FiksDataStore library doesn't match the
// version that created the database environment.
// #FDS_INVALID - the environment file headers are corrupted.
// ENOENT - the directory specified by the path parameter doesn't exist.
// EACCES - the user didn't have permission to access the environment files.
// EAGAIN - the environment was locked by another process.
auto fds_env_open(FDS_env* env, const char* path, unsigned int flags, fds_mode_t mode) -> int;

// @brief Return statistics about the FiksDataStore environment.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[out] stat The address of an #FDS_stat structure
// where the statistics will be copied
auto fds_env_stat(FDS_env* env, FDS_stat* stat) -> int;

// @brief Return information about the FiksDataStore environment.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[out] stat The address of an #FDS_envinfo structure
// where the information will be copied
auto fds_env_info(FDS_env* env, FDS_envinfo* stat) -> int;

// @brief Flush the data buffers to disk.
// Data is always written to disk when #fds_txn_commit() is called,
// but the operating system may keep it buffered. FiksDataStore always flushes
// the OS buffers upon commit as well, unless the environment was
// opened with #FDS_NOSYNC or in part #FDS_NOMETASYNC. This call is
// not valid if the environment was opened with #FDS_RDONLY.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] force If non-zero, force a synchronous flush. Otherwise
// if the environment has the #FDS_NOSYNC flag set the flushes
// will be omitted, and with #FDS_MAPASYNC they will be asynchronous.
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EACCES - the environment is read-only.
// EINVAL - an invalid parameter was specified.
// EIO - an error occurred during synchronization.
auto fds_env_sync(FDS_env* env, int force) -> int;

// @brief Close the environment and release the memory map.
// Only a single thread may call this function. All transactions, databases,
// and cursors must already be closed before calling this function. Attempts to
// use any such handles after calling this function will cause a SIGSEGV.
// The environment handle will be freed and must not be used again after this call.
// @param[in] env An environment handle returned by #fds_env_create()
void fds_env_close(FDS_env* env);

// @brief Set environment flags.
// This may be used to set some flags in addition to those from
// #fds_env_open(), or to unset these flags. If several threads
// change the flags at the same time, the result is undefined.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] flags The flags to change, bitwise OR'ed together
// @param[in] onoff A non-zero value sets the flags, zero clears them.
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
auto fds_env_set_flags(FDS_env* env, unsigned int flags, int onoff) -> int;

// @brief Get environment flags.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[out] flags The address of an integer to store the flags
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
auto fds_env_get_flags(FDS_env* env, unsigned int* flags) -> int;

// @brief Return the path that was used in #fds_env_open().
// @param[in] env An environment handle returned by #fds_env_create()
// @param[out] path Address of a string pointer to contain the path. This
// is the actual string in the environment, not a copy. It should not be
// altered in any way.
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
auto fds_env_get_path(FDS_env* env, const char** path) -> int;

// @brief Return the filedescriptor for the given environment.
// This function may be called after fork(), so the descriptor can be
// closed before exec*(). Other FiksDataStore file descriptors have FD_CLOEXEC.
//
// @param[in] env An environment handle returned by #fds_env_create()
// @param[out] fd Address of a fds_filehandle_t to contain the descriptor.
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
auto fds_env_get_fd(FDS_env* env, fds_filehandle_t* fd) -> int;

// @brief Set the size of the memory map to use for this environment.
// The size should be a multiple of the OS page size. The default is
// 10485760 bytes. The size of the memory map is also the maximum size
// of the database. The value should be chosen as large as possible,
// to accommodate future growth of the database.
// This function should be called after #fds_env_create() and before #fds_env_open().
// It may be called at later times if no transactions are active in
// this process. Note that the library does not check for this condition,
// the caller must ensure it explicitly.
//
// The new size takes effect immediately for the current process but
// will not be persisted to any others until a write transaction has been
// committed by the current process. Also, only mapsize increases are
// persisted into the environment.
//
// If the mapsize is increased by another process, and data has grown
// beyond the range of the current mapsize, #fds_txn_begin() will
// return #FDS_MAP_RESIZED. This function may be called with a size
// of zero to adopt the new size.
//
// Any attempt to set a size smaller than the space already consumed
// by the environment will be silently changed to the current size of the used space.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] size The size in bytes
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified, or the environment has
// an active write transaction.
auto fds_env_set_mapsize(FDS_env* env, fds_size_t size) -> int;

// @brief Set the maximum number of threads/reader slots for the environment.
// This defines the number of slots in the lock table that is used to track readers in the
// the environment. The default is 126.
// Starting a read-only transaction normally ties a lock table slot to the
// current thread until the environment closes or the thread exits. If
// FDS_NOTLS is in use, #fds_txn_begin() instead ties the slot to the
// FDS_txn object until it or the #FDS_env object is destroyed.
// This function may only be called after #fds_env_create() and before #fds_env_open().
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] readers The maximum number of reader lock table slots
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified, or the environment is already open.
auto fds_env_set_maxreaders(FDS_env* env, unsigned int readers) -> int;

// @brief Get the maximum number of threads/reader slots for the environment.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[out] readers Address of an integer to store the number of readers
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
auto fds_env_get_maxreaders(FDS_env* env, unsigned int* readers) -> int;

// @brief Set the maximum number of named databases for the environment.
// This function is only needed if multiple databases will be used in the
// environment. Simpler applications that use the environment as a single
// unnamed database can ignore this option.
// This function may only be called after #fds_env_create() and before #fds_env_open().
//
// Currently a moderate number of slots are cheap but a huge number gets
// expensive: 7-120 words per transaction, and every #fds_dbi_open()
// does a linear search of the opened slots.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] dbs The maximum number of databases
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified, or the environment is already open.
auto fds_env_set_maxdbs(FDS_env* env, FDS_dbi dbs) -> int;

// @brief Get the maximum size of keys we can write.
// Depends on the compile-time constant #FDS_MAXKEYSIZE. Default 511.
// See @ref FDS_val.
// @param[in] env An environment handle returned by #fds_env_create()
// @return The maximum size of a key we can write
auto fds_env_get_maxkeysize(FDS_env* env) -> int;

// @brief Set application information associated with the #FDS_env.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] ctx An arbitrary pointer for whatever the application needs.
// @return A non-zero error value on failure and 0 on success.
auto fds_env_set_userctx(FDS_env* env, void* ctx) -> int;

// @brief Get the application information associated with the #FDS_env.
// @param[in] env An environment handle returned by #fds_env_create()
// @return The pointer set by #fds_env_set_userctx().
auto fds_env_get_userctx(FDS_env* env) -> void*;

// @brief A callback function for most FiksDataStore assert() failures,
// called before printing the message and aborting.
//
// @param[in] env An environment handle returned by #fds_env_create().
// @param[in] msg The assertion message, not including newline.
using FDS_assert_func = void(FDS_env* env, const char* msg);

// Set or reset the assert() callback of the environment.
// Disabled if FiksDataStore is built with NDEBUG.
// @note This hack should become obsolete as FiksDataStore's error handling matures.
// @param[in] env An environment handle returned by #fds_env_create().
// @param[in] func An #FDS_assert_func function, or 0.
// @return A non-zero error value on failure and 0 on success.
auto fds_env_set_assert(FDS_env* env, FDS_assert_func* func) -> int;

// @brief Create a transaction for use with the environment.
// The transaction handle may be discarded using #fds_txn_abort() or #fds_txn_commit().
// @note A transaction and its cursors must only be used by a single
// thread, and a thread may only have a single transaction at a time.
// If #FDS_NOTLS is in use, this does not apply to read-only transactions.
// @note Cursors may not span transactions.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] parent If this parameter is non-NULL, the new transaction
// will be a nested transaction, with the transaction indicated by \b parent
// as its parent. Transactions may be nested to any level. A parent
// transaction and its cursors may not issue any other operations than
// fds_txn_commit and fds_txn_abort while it has active child transactions.
// @param[in] flags Special options for this transaction. This parameter
// must be set to 0 or by bitwise OR'ing together one or more of the
// values described here.
//
// #FDS_RDONLY
// This transaction will not perform any write operations.
// #FDS_NOSYNC
// Don't flush system buffers to disk when committing this transaction.
// #FDS_NOMETASYNC
// Flush system buffers but omit metadata flush when committing this transaction.
//
// @param[out] txn Address where the new #FDS_txn handle will be stored
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// #FDS_PANIC - a fatal error occurred earlier and the environment
// must be shut down.
// #FDS_MAP_RESIZED - another process wrote data beyond this FDS_env's
// mapsize and this environment's map must be resized as well.
// See #fds_env_set_mapsize().
// #FDS_READERS_FULL - a read-only transaction was requested and
// the reader lock table is full. See #fds_env_set_maxreaders().
// ENOMEM - out of memory.
auto fds_txn_begin(FDS_env* env, FDS_txn* parent, unsigned int flags, FDS_txn** txn) -> int;

// @brief Returns the transaction's #FDS_env
// @param[in] txn A transaction handle returned by #fds_txn_begin()
auto fds_txn_env(FDS_txn* txn) -> FDS_env*;

// @brief Return the transaction's ID.
// This returns the identifier associated with this transaction. For a
// read-only transaction, this corresponds to the snapshot being read;
// concurrent readers will frequently have the same transaction ID.
//
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @return A transaction ID, valid if input is an active transaction.
auto fds_txn_id(FDS_txn* txn) -> fds_size_t;

// @brief Commit all the operations of a transaction into the database.
// The transaction handle is freed. It and its cursors must not be used
// again after this call, except with #fds_cursor_renew().
// @note Earlier documentation incorrectly said all cursors would be freed.
// Only write-transactions free cursors.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
// ENOSPC - no more disk space.
// EIO - a low-level I/O error occurred while writing.
// ENOMEM - out of memory.
auto fds_txn_commit(FDS_txn* txn) -> int;

// @brief Abandon all the operations of the transaction instead of saving them.
// The transaction handle is freed. It and its cursors must not be used
// again after this call, except with #fds_cursor_renew().
// @note Earlier documentation incorrectly said all cursors would be freed.
// Only write-transactions free cursors.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
void fds_txn_abort(FDS_txn* txn);

// @brief Reset a read-only transaction.
// Abort the transaction like #fds_txn_abort(), but keep the transaction
// handle. #fds_txn_renew() may reuse the handle. This saves allocation
// overhead if the process will start a new read-only transaction soon,
// and also locking overhead if #FDS_NOTLS is in use. The reader table
// lock is released, but the table slot stays tied to its thread or
// #FDS_txn. Use fds_txn_abort() to discard a reset handle, and to free
// its lock table slot if FDS_NOTLS is in use.
// Cursors opened within the transaction must not be used
// again after this call, except with #fds_cursor_renew().
// Reader locks generally don't interfere with writers, but they keep old
// versions of database pages allocated. Thus they prevent the old pages
// from being reused when writers commit new data, and so under heavy load
// the database size may grow much more rapidly than otherwise.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
void fds_txn_reset(FDS_txn* txn);

// @brief Renew a read-only transaction.
// This acquires a new reader lock for a transaction handle that had been
// released by #fds_txn_reset(). It must be called before a reset transaction
// may be used again.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// #FDS_PANIC - a fatal error occurred earlier and the environment
// must be shut down.
// EINVAL - an invalid parameter was specified.
auto fds_txn_renew(FDS_txn* txn) -> int;

// Compat with version <= 0.9.4, avoid clash with libmdb from MDB Tools project
#define fds_open(txn, name, flags, dbi) fds_dbi_open(txn, name, flags, dbi)
// Compat with version <= 0.9.4, avoid clash with libmdb from MDB Tools project
#define fds_close(env, dbi) fds_dbi_close(env, dbi)

// @brief Open a database in the environment.
// A database handle denotes the name and parameters of a database,
// independently of whether such a database exists.
// The database handle may be discarded by calling #fds_dbi_close().
// The old database handle is returned if the database was already open.
// The handle may only be closed once.
//
// The database handle will be private to the current transaction until
// the transaction is successfully committed. If the transaction is
// aborted the handle will be closed automatically.
// After a successful commit the handle will reside in the shared
// environment, and may be used by other transactions.
//
// This function must not be called from multiple concurrent
// transactions in the same process. A transaction that uses
// this function must finish (either commit or abort) before
// any other transaction in the process may use this function.
//
// To use named databases (with name != NULL), #fds_env_set_maxdbs()
// must be called before opening the environment. Database names are
// keys in the unnamed database, and may be read but not written.
//
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] name The name of the database to open. If only a single
// database is needed in the environment, this value may be NULL.
// @param[in] flags Special options for this database. This parameter
// must be set to 0 or by bitwise OR'ing together one or more of the
// values described here.
//
// #FDS_REVERSEKEY
// Keys are strings to be compared in reverse order, from the end
// of the strings to the beginning. By default, Keys are treated as strings and
// compared from beginning to end.
// #FDS_CREATE
// Create the named database if it doesn't exist. This option is not
// allowed in a read-only transaction or a read-only environment.
//
// @param[out] dbi Address where the new #FDS_dbi handle will be stored
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// #FDS_NOTFOUND - the specified database doesn't exist in the environment
// and #FDS_CREATE was not specified.
// #FDS_DBS_FULL - too many databases have been opened. See #fds_env_set_maxdbs().
auto fds_dbi_open(FDS_txn* txn, const char* name, unsigned int flags, FDS_dbi* dbi) -> int;

// @brief Retrieve statistics for a database.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] dbi A database handle returned by #fds_dbi_open()
// @param[out] stat The address of an #FDS_stat structure
// where the statistics will be copied
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
auto fds_stat(FDS_txn* txn, FDS_dbi dbi, FDS_stat* stat) -> int;

// @brief Retrieve the DB flags for a database handle.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] dbi A database handle returned by #fds_dbi_open()
// @param[out] flags Address where the flags will be returned.
// @return A non-zero error value on failure and 0 on success.
auto fds_dbi_flags(FDS_txn* txn, FDS_dbi dbi, unsigned int* flags) -> int;

// @brief Close a database handle. Normally unnecessary. Use with care:
// This call is not mutex protected. Handles should only be closed by
// a single thread, and only if no other threads are going to reference
// the database handle or one of its cursors any further. Do not close
// a handle if an existing transaction has modified its database.
// Doing so can cause misbehavior from database corruption to errors
// like FDS_BAD_VALSIZE (since the DB name is gone).
//
// Closing a database handle is not necessary, but lets #fds_dbi_open()
// reuse the handle value. Usually it's better to set a bigger
// #fds_env_set_maxdbs(), unless that value would be large.
//
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] dbi A database handle returned by #fds_dbi_open()
void fds_dbi_close(FDS_env* env, FDS_dbi dbi);

// @brief Empty or delete+close a database.
// See #fds_dbi_close() for restrictions about closing the DB handle.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] dbi A database handle returned by #fds_dbi_open()
// @param[in] del 0 to empty the DB, 1 to delete it from the
// environment and close the DB handle.
// @return A non-zero error value on failure and 0 on success.
auto fds_drop(FDS_txn* txn, FDS_dbi dbi, int del) -> int;

// @brief Set a custom key comparison function for a database.
// The comparison function is called whenever it is necessary to compare a
// key specified by the application with a key currently stored in the database.
// If no comparison function is specified, and no special key flags were specified
// with #fds_dbi_open(), the keys are compared lexically, with shorter keys collating
// before longer keys.
// @warning This function must be called before any data access functions are used,
// otherwise data corruption may occur. The same comparison function must be used by every
// program accessing the database, every time the database is used.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] dbi A database handle returned by #fds_dbi_open()
// @param[in] cmp A #FDS_cmp_func function
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
auto fds_set_compare(FDS_txn* txn, FDS_dbi dbi, FDS_cmp_func cmp) -> int;

// @brief Get items from a database.
// This function retrieves key/data pairs from the database. The address
// and length of the data associated with the specified \b key are returned
// in the structure to which \b data refers.
//
// @note The memory pointed to by the returned values is owned by the
// database. The caller need not dispose of the memory, and may not
// modify it in any way. For values returned in a read-only transaction
// any modification attempts will cause a SIGSEGV.
// @note Values returned from the database are valid only until a
// subsequent update operation, or the end of the transaction.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] dbi A database handle returned by #fds_dbi_open()
// @param[in] key The key to search for in the database
// @param[out] data The data corresponding to the key
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// #FDS_NOTFOUND - the key was not in the database.
// EINVAL - an invalid parameter was specified.
auto fds_get(FDS_txn* txn, FDS_dbi dbi, FDS_val* key, FDS_val* data) -> int;

// @brief Store items into a database.
// This function stores key/data pairs in the database. The default behavior
// is to enter the new key/data pair, replacing any previously existing key.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] dbi A database handle returned by #fds_dbi_open()
// @param[in] key The key to store in the database
// @param[in,out] data The data to store
// @param[in] flags Special options for this operation. This parameter
// must be set to 0 or by bitwise OR'ing together one or more of the
// values described here.
//
// #FDS_NOOVERWRITE - enter the new key/data pair only if the key
// does not already appear in the database. The function will return
// #FDS_KEYEXIST if the key already appears in the database. The \b data
// parameter will be set to point to the existing item.
// #FDS_RESERVE - reserve space for data of the given size, but
// don't copy the given data. Instead, return a pointer to the
// reserved space, which the caller can fill in later - before
// the next update operation or the transaction ends. This saves
// an extra memcpy if the data is being generated later.
// FiksDataStore does nothing else with this memory, the caller is expected
// to modify all of the space requested.
// #FDS_APPEND - append the given key/data pair to the end of the
// database. This option allows fast bulk loading when keys are
// already known to be in the correct order. Loading unsorted keys
// with this flag will cause a #FDS_KEYEXIST error.
//
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// #FDS_MAP_FULL - the database is full, see #fds_env_set_mapsize().
// #FDS_TXN_FULL - the transaction has too many dirty pages.
// EACCES - an attempt was made to write in a read-only transaction.
// EINVAL - an invalid parameter was specified.
auto fds_put(FDS_txn* txn, FDS_dbi dbi, FDS_val* key, FDS_val* data, unsigned int flags) -> int;

// @brief Delete items from a database.
// This function removes key/data pairs from the database.
// The data parameter is ignored.
// This function will return #FDS_NOTFOUND if the specified key
// is not in the database.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] dbi A database handle returned by #fds_dbi_open()
// @param[in] key The key to delete from the database
// @param[in] data The data to delete
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EACCES - an attempt was made to write in a read-only transaction.
// EINVAL - an invalid parameter was specified.
auto fds_del(FDS_txn* txn, FDS_dbi dbi, FDS_val* key, FDS_val* data) -> int;

// @brief Create a cursor handle.
// A cursor is associated with a specific transaction and database.
// A cursor cannot be used when its database handle is closed. Nor
// when its transaction has ended, except with #fds_cursor_renew().
// It can be discarded with #fds_cursor_close().
// A cursor in a write-transaction can be closed before its transaction
// ends, and will otherwise be closed when its transaction ends.
// A cursor in a read-only transaction must be closed explicitly, before
// or after its transaction ends. It can be reused with
// #fds_cursor_renew() before finally closing it.
// @note Earlier documentation said that cursors in every transaction
// were closed when the transaction committed or aborted.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] dbi A database handle returned by #fds_dbi_open()
// @param[out] cursor Address where the new #FDS_cursor handle will be stored
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
auto fds_cursor_open(FDS_txn* txn, FDS_dbi dbi, FDS_cursor** cursor) -> int;

// @brief Close a cursor handle.
// The cursor handle will be freed and must not be used again after this call.
// Its transaction must still be live if it is a write-transaction.
// @param[in] cursor A cursor handle returned by #fds_cursor_open()
void fds_cursor_close(FDS_cursor* cursor);

// @brief Renew a cursor handle.
// A cursor is associated with a specific transaction and database.
// Cursors that are only used in read-only
// transactions may be re-used, to avoid unnecessary malloc/free overhead.
// The cursor may be associated with a new read-only transaction, and
// referencing the same database handle as it was created with.
// This may be done whether the previous transaction is live or dead.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] cursor A cursor handle returned by #fds_cursor_open()
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EINVAL - an invalid parameter was specified.
auto fds_cursor_renew(FDS_txn* txn, FDS_cursor* cursor) -> int;

// @brief Return the cursor's transaction handle.
// @param[in] cursor A cursor handle returned by #fds_cursor_open()
auto fds_cursor_txn(FDS_cursor* cursor) -> FDS_txn*;

// @brief Return the cursor's database handle.
// @param[in] cursor A cursor handle returned by #fds_cursor_open()
auto fds_cursor_dbi(FDS_cursor* cursor) -> FDS_dbi;

// @brief Retrieve by cursor.
// This function retrieves key/data pairs from the database. The address and length
// of the key are returned in the object to which \b key refers (except for the
// case of the #FDS_SET option, in which the \b key object is unchanged), and
// the address and length of the data are returned in the object to which \b data
// refers.
// See #fds_get() for restrictions on using the output values.
// @param[in] cursor A cursor handle returned by #fds_cursor_open()
// @param[in,out] key The key for a retrieved item
// @param[in,out] data The data of a retrieved item
// @param[in] op A cursor operation #FDS_cursor_op
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// #FDS_NOTFOUND - no matching key found.
// EINVAL - an invalid parameter was specified.
auto fds_cursor_get(FDS_cursor* cursor, FDS_val* key, FDS_val* data, FDS_cursor_op op) -> int;

// @brief Store by cursor.
// This function stores key/data pairs into the database.
// The cursor is positioned at the new item, or on failure usually near it.
// @note Earlier documentation incorrectly said errors would leave the
// state of the cursor unchanged.
// @param[in] cursor A cursor handle returned by #fds_cursor_open()
// @param[in] key The key operated on.
// @param[in] data The data operated on.
// @param[in] flags Options for this operation. This parameter
// must be set to 0 or one of the values described here.
//
// #FDS_CURRENT - replace the item at the current cursor position.
// The \b key parameter must still be provided, and must match it.
// This is intended to be used when the new data is the same size as the old.
// Otherwise it will simply perform a delete of the old record followed by an insert.
// #FDS_NOOVERWRITE - enter the new key/data pair only if the key
// does not already appear in the database. The function will return
// #FDS_KEYEXIST if the key already appears in the database.
// #FDS_RESERVE - reserve space for data of the given size, but
// don't copy the given data. Instead, return a pointer to the
// reserved space, which the caller can fill in later - before
// the next update operation or the transaction ends. This saves
// an extra memcpy if the data is being generated later.
// #FDS_APPEND - append the given key/data pair to the end of the
// database. No key comparisons are performed. This option allows
// fast bulk loading when keys are already known to be in the
// correct order. Loading unsorted keys with this flag will cause
// a #FDS_KEYEXIST error.
//
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// #FDS_MAP_FULL - the database is full, see #fds_env_set_mapsize().
// #FDS_TXN_FULL - the transaction has too many dirty pages.
// EACCES - an attempt was made to write in a read-only transaction.
// EINVAL - an invalid parameter was specified.
auto fds_cursor_put(FDS_cursor* cursor, FDS_val* key, FDS_val* data, unsigned int flags) -> int;

// @brief Delete current key/data pair
// This function deletes the key/data pair to which the cursor refers.
// This does not invalidate the cursor, so operations such as FDS_NEXT
// can still be used on it.
// Both FDS_NEXT and FDS_GET_CURRENT will return the same record after
// this operation.
// @param[in] cursor A cursor handle returned by #fds_cursor_open()
// @param[in] flags Options for this operation. This parameter
// must be set to 0.
//
// @return A non-zero error value on failure and 0 on success. Some possible
// errors are:
//
// EACCES - an attempt was made to write in a read-only transaction.
// EINVAL - an invalid parameter was specified.
auto fds_cursor_del(FDS_cursor* cursor, unsigned int flags) -> int;

// @brief Compare two data items according to a particular database.
// This returns a comparison as if the two data items were keys in the
// specified database.
// @param[in] txn A transaction handle returned by #fds_txn_begin()
// @param[in] dbi A database handle returned by #fds_dbi_open()
// @param[in] a The first item to compare
// @param[in] b The second item to compare
// @return < 0 if a < b, 0 if a == b, > 0 if a > b
auto fds_cmp(FDS_txn* txn, FDS_dbi dbi, const FDS_val* a, const FDS_val* b) -> int;

// @brief A callback function used to print a message from the library.
// @param[in] msg The string to be printed.
// @param[in] ctx An arbitrary context pointer for the callback.
// @return < 0 on failure, >= 0 on success.
using FDS_msg_func = int (*)(const char* msg, void* ctx);

// @brief Dump the entries in the reader lock table.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[in] func A #FDS_msg_func function
// @param[in] ctx Anything the message function needs
// @return < 0 on failure, >= 0 on success.
auto fds_reader_list(FDS_env* env, FDS_msg_func func, void* ctx) -> int;

// @brief Check for stale entries in the reader lock table.
// @param[in] env An environment handle returned by #fds_env_create()
// @param[out] dead Number of stale slots that were cleared
// @return 0 on success, non-zero on failure.
auto fds_reader_check(FDS_env* env, int* dead) -> int;

#if FDS_DEBUG
// Display a key in hexadecimal and return the address of the result.
// @param[in] key the key to display
// @param[in] buf the buffer to write into. Should always be #DKBUF.
// @return The key in hexadecimal form.
auto fds_dkey(FDS_val* key, char* buf) -> char*;
#endif

// @}

// @page tools FiksDataStore Command Line Tools
// The following describes the command line tools that are available for FiksDataStore.
// \li \ref fds_copy_1
// \li \ref fds_dump_1
// \li \ref fds_load_1
// \li \ref fds_stat_1

#include "env.h"

#include "btree.h"
#include "compare.h"
#include "cursor.h"
#include "db.h"
#include "debug.h"
#include "hash.h"
#include "lock.h"
#include "txn.h"

// The maximum size of a database page.
//
// It is 32k or 64k, since value-PAGEBASE must fit in
// #FDS_page.%mp_upper.
//
// FiksDataStore will use database pages < OS pages if needed.
// That causes more I/O in write transactions: The OS must
// know (read) the whole page before writing a partial page.
//
// Note that we don't currently support Huge pages. On Linux,
// regular data files cannot use Huge pages, and in general
// Huge pages aren't actually pageable. We rely on the OS
// demand-pager to read our data and page it out when memory
// pressure from other processes is high. So until OSs have
// actual paging support for Huge pages, they're not viable.
#define MAX_PAGESIZE (PAGEBASE ? 0x10000 : 0x8000)

// The minimum number of keys required in a database page.
// Setting this to a larger value will place a smaller bound on the
// maximum size of a data item. Data items larger than this size will
// be pushed into overflow pages instead of being stored directly in
// the B-tree node. This value used to default to 4. With a page size
// of 4096 bytes that meant that any item larger than 1024 bytes would
// go into an overflow page. That also meant that on average 2-3KB of
// each overflow page was wasted space. The value cannot be lower than
// 2 because then there would no longer be a tree structure. With this
// value, items larger than 2KB will go into overflow pages, and on
// average only 1KB will be wasted.
enum
{
    FDS_MINKEYS = 2
};

// A stamp that identifies a file as an FiksDataStore file.
// There's nothing special about this value other than that it is easily
// recognizable, and it will reflect any byte order mismatches.
enum
{
    FDS_MAGIC = 0xFDFDC0DE
};

// The version number for a database's datafile format.
enum
{
    FDS_DATA_VERSION = 1
};

static void fds_env_reader_dest(void* ptr);

#ifdef FDS_WINDOWS
using fds_nchar_t = wchar_t;
#define FDS_NAME(str) L##str
// Suppress deprecation warning for wcscpy - we know the buffer sizes
#pragma warning(push)
#pragma warning(disable : 4996)
#define fds_name_cpy wcscpy
#pragma warning(pop)
#else
// Character type for file names: char on Unix, wchar_t on Windows
typedef char fds_nchar_t;
#define FDS_NAME(str) str    // #fds_nchar_t[] string literal
#define fds_name_cpy strcpy  // Copy name (#fds_nchar_t string)
#endif

#ifdef O_CLOEXEC  // POSIX.1-2008: Set FD_CLOEXEC atomically at open()
#define FDS_CLOEXEC O_CLOEXEC
#else
#define FDS_CLOEXEC 0
#endif

#ifdef FDS_WINDOWS

// Junk for arranging thread-specific callbacks on Windows. This is
// necessarily platform and compiler-specific. Windows supports up
// to 1088 keys. Let's assume nobody opens more than 64 environments
// in a single process, for now. They can override this if needed.
#ifndef MAX_TLS_KEYS
#define MAX_TLS_KEYS 64
#endif

// Junk for arranging thread-specific callbacks on Windows. This is
// necessarily platform and compiler-specific. Windows supports up
// to 1088 keys. Let's assume nobody opens more than 64 environments
// in a single process, for now. They can override this if needed.
pthread_key_t fds_tls_keys[MAX_TLS_KEYS];
int fds_tls_nkeys;

void NTAPI fds_tls_callback(PVOID module, DWORD reason, PVOID ptr)
{
    int i;
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_PROCESS_DETACH:
        // No action needed for these cases
        break;
    case DLL_THREAD_DETACH:
        for (i = 0; i < fds_tls_nkeys; i++)
        {
            auto* r = (FDS_reader*)(pthread_getspecific(fds_tls_keys[i]));
            if (r != nullptr)
            {
                fds_env_reader_dest(r);
            }
        }
        break;
    default:
        // Handle any unexpected values
        break;
    }
}

// We use native NT APIs to setup the memory map, so that we can
// let the DB file grow incrementally instead of always preallocating
// the full size. These APIs are defined in <wdm.h> and <ntifs.h>
// but those headers are meant for driver-level development and
// conflict with the regular user-level headers, so we explicitly
// declare them here. We get pointers to these functions from
// NTDLL.DLL at runtime, to avoid buildtime dependencies on any
// NTDLL import libraries.
using NtCreateSectionFunc = NTSTATUS(WINAPI*)(OUT PHANDLE sh,
                                              IN ACCESS_MASK acc,
                                              IN void* oa OPTIONAL,
                                              IN PLARGE_INTEGER ms OPTIONAL,
                                              IN ULONG pp,
                                              IN ULONG aa,
                                              IN HANDLE fh OPTIONAL);

using SECTION_INHERIT = enum SECTION_INHERIT_ENUM { ViewShare = 1, ViewUnmap = 2 };

using NtMapViewOfSectionFunc = NTSTATUS(WINAPI*)(IN HANDLE sh,
                                                 IN HANDLE ph,
                                                 IN OUT PVOID* addr,
                                                 IN ULONG_PTR zbits,
                                                 IN SIZE_T cs,
                                                 IN OUT PLARGE_INTEGER off OPTIONAL,
                                                 IN OUT PSIZE_T vs,
                                                 IN SECTION_INHERIT ih,
                                                 IN ULONG at,
                                                 IN ULONG pp);

using NtCloseFunc = NTSTATUS(WINAPI*)(HANDLE h);

static int fds_sec_inited;
static SECURITY_DESCRIPTOR fds_null_sd;
static SECURITY_ATTRIBUTES fds_all_sa;
static NtCloseFunc NtClose;
static NtCreateSectionFunc NtCreateSection;
static NtMapViewOfSectionFunc NtMapViewOfSection;
#endif

#if defined(__FreeBSD__) && defined(__FreeBSD_version) && __FreeBSD_version >= 1100110
#elif defined(__APPLE__)
#define FDS_FDATASYNC(fd) fcntl(fd, F_FULLFSYNC)
#elif defined(BSD) || defined(__FreeBSD_kernel__)
#define FDS_FDATASYNC fsync
#endif

#ifdef FDS_WINDOWS
#define FDS_FDATASYNC(fd) (!FlushFileBuffers(fd))
#define FDS_MSYNC(addr, len, flags) (!FlushViewOfFile(addr, len))
#endif

// Function for flushing the data of a file. Define this to fsync
// if fdatasync() is not supported.
#ifndef FDS_FDATASYNC
#define FDS_FDATASYNC fdatasync
#endif

#ifndef FDS_WINDOWS
// A flag for opening a file and requesting synchronous data writes.
// This is only used when writing a meta page. It's not strictly needed;
// we could just do a normal write and then immediately perform a flush.
// But if this flag is available it saves us an extra system call.
//
// @note If O_DSYNC is undefined but exists in /usr/include,
// preferably set some compiler flag to get the definition.
#ifndef FDS_DSYNC
#ifdef O_DSYNC
#define FDS_DSYNC O_DSYNC
#else
#define FDS_DSYNC O_SYNC
#endif
#endif
#endif

#ifndef FDS_MSYNC
#define FDS_MSYNC(addr, len, flags) msync(addr, len, flags)
#endif

#ifndef MS_SYNC
#define MS_SYNC 1
#endif

#ifndef MS_ASYNC
#define MS_ASYNC 0
#endif

// Filename - string of #fds_nchar_t[]
struct FDS_name
{
    int mn_len;           // Length
    int mn_alloced;       // True if #mn_val was malloced
    fds_nchar_t* mn_val;  // Contents
};

// Filename suffixes [datafile,lockfile][without,with FDS_NOSUBDIR]
static const fds_nchar_t* const fds_suffixes[2][2] = {
    {FDS_NAME("/data.mdb"),      FDS_NAME("")},
    {FDS_NAME("/lock.mdb"), FDS_NAME("-lock")}
};

enum
{
    FDS_SUFFLEN = 9  // Max string length in #fds_suffixes[]
};

// Destroy fname from #fds_fname_init()
#define fds_fname_destroy(fname)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((fname).mn_alloced)                                                                                        \
            free((fname).mn_val);                                                                                      \
    } while (0)

#if defined(FDS_WINDOWS)

// Convert src to new wchar_t[] string with room for xtra extra chars
static auto ESECT utf8_to_utf16(const char* src, FDS_name* dst, int xtra) -> int
{
    int rc;
    int need = 0;
    wchar_t* result = nullptr;
    for (;;)
    {  // malloc result, then fill it in
        need = MultiByteToWideChar(CP_UTF8, 0, src, -1, result, need);
        if (need == 0)
        {
            rc = ErrCode();
            free(result);
            return rc;
        }
        if (result == nullptr)
        {
            result = (wchar_t*)malloc(sizeof(wchar_t) * (need + xtra));
            if (result == nullptr)
                return ENOMEM;
            continue;
        }
        dst->mn_alloced = 1;
        dst->mn_len = need - 1;
        dst->mn_val = result;
        return FDS_SUCCESS;
    }
}
#endif  // defined(FDS_WINDOWS)

// Set up filename + scratch area for filename suffix, for opening files.
// It should be freed with #fds_fname_destroy().
// On Windows, paths are converted from char *UTF-8 to wchar_t *UTF-16.
//
// path Pathname for #fds_env_open().
// envflags Whether a subdir and/or lockfile will be used.
// fname Resulting filename, with room for a suffix if necessary.
static auto ESECT fds_fname_init(const char* path, unsigned envflags, FDS_name* fname) -> int
{
    int no_suffix = F_ISSET(envflags, FDS_NOSUBDIR | FDS_NOLOCK);
    fname->mn_alloced = 0;
#ifdef FDS_WINDOWS
    return utf8_to_utf16(path, fname, (no_suffix != 0) ? 0 : FDS_SUFFLEN);
#else
    fname->mn_len = strlen(path);
    if (no_suffix)
        fname->mn_val = (char*)path;
    else if ((fname->mn_val = (char*)malloc(fname->mn_len + FDS_SUFFLEN + 1)) != NULL)
    {
        fname->mn_alloced = 1;
        strcpy(fname->mn_val, path);
    }
    else
        return ENOMEM;
    return FDS_SUCCESS;
#endif
}

// File type, access mode etc. for #fds_fopen()
enum fds_fopen_type
{
#ifdef FDS_WINDOWS
    FDS_O_RDONLY,
    FDS_O_RDWR,
    FDS_O_OVERLAPPED,
    FDS_O_META,
    FDS_O_COPY,
    FDS_O_LOCKS
#else
    // A comment in fds_fopen() explains some O_* flag choices.
    FDS_O_RDONLY = O_RDONLY,                          // for RDONLY me_fd
    FDS_O_RDWR = O_RDWR | O_CREAT,                    // for me_fd
    FDS_O_META = O_WRONLY | FDS_DSYNC | FDS_CLOEXEC,  // for me_mfd
    FDS_O_COPY =
        O_WRONLY | O_CREAT | O_EXCL | FDS_CLOEXEC,  // for #fds_env_copy()
                                                    // Bitmask for open() flags in enum #fds_fopen_type.  The other bits
                                                    // distinguish otherwise-equal FDS_O_* constants from each other.
    FDS_O_MASK = FDS_O_RDWR | FDS_CLOEXEC | FDS_O_RDONLY | FDS_O_META | FDS_O_COPY,
    FDS_O_LOCKS = FDS_O_RDWR | FDS_CLOEXEC | ((FDS_O_MASK + 1) & ~FDS_O_MASK)  // for me_lfd
#endif
};

// Open an FiksDataStore file.
// env	The FiksDataStore environment.
// fname	Path from from #fds_fname_init().  A suffix is
// appended if necessary to create the filename, without changing mn_len.
// which	Determines file type, access mode, etc.
// mode	The Unix permissions for the file, if we create it.
// res	Resulting file handle.
// Return 0 on success, non-zero on failure.
static auto ESECT
fds_fopen(const FDS_env* env, FDS_name* fname, enum fds_fopen_type which, fds_mode_t mode, HANDLE* res) -> int
{
    int rc = FDS_SUCCESS;
    HANDLE fd;
#ifdef FDS_WINDOWS
    DWORD acc;
    DWORD share;
    DWORD disp;
    DWORD attrs;
#else
    int flags;
#endif

    if (fname->mn_alloced != 0)  // modifiable copy
    {
#ifdef FDS_WINDOWS
#pragma warning(push)
#pragma warning(disable : 4996)  // Suppress deprecation warning for wcscpy
#endif
        fds_name_cpy(fname->mn_val + fname->mn_len,
                     fds_suffixes[which == FDS_O_LOCKS][F_ISSET(env->me_flags, FDS_NOSUBDIR)]);
#ifdef FDS_WINDOWS
#pragma warning(pop)
#endif
    }

    // The directory must already exist.  Usually the file need not.
    // FDS_O_META requires the file because we already created it using
    // FDS_O_RDWR.  FDS_O_COPY must not overwrite an existing file.
    //
    // With FDS_O_COPY we do not want the OS to cache the writes, since
    // the source data is already in the OS cache.
    //
    // The lockfile needs FD_CLOEXEC (close file descriptor on exec*())
    // to avoid the flock() issues noted under Caveats in fds.h.
    // Also set it for other filehandles which the user cannot get at
    // and close himself, which he may need after fork().  I.e. all but
    // me_fd, which programs do use via fds_env_get_fd().

#ifdef FDS_WINDOWS
    acc = GENERIC_READ | GENERIC_WRITE;
    share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    disp = OPEN_ALWAYS;
    attrs = FILE_ATTRIBUTE_NORMAL;
    switch (which)
    {
    case FDS_O_OVERLAPPED:  // for unbuffered asynchronous writes (write-through mode)
        acc = GENERIC_WRITE;
        disp = OPEN_EXISTING;
        attrs = FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH;
        break;
    case FDS_O_RDONLY:  // read-only datafile
        acc = GENERIC_READ;
        disp = OPEN_EXISTING;
        break;
    case FDS_O_META:  // for writing metapages
        acc = GENERIC_WRITE;
        disp = OPEN_EXISTING;
        attrs = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH;
        break;
    case FDS_O_COPY:  // fds_env_copy() & co
        acc = GENERIC_WRITE;
        share = 0;
        disp = CREATE_NEW;
        attrs = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
        break;
    default:
        break;  // silence gcc -Wswitch (not all enum values handled)
    }
    fd = CreateFileW(fname->mn_val, acc, share, nullptr, disp, attrs, nullptr);
#else
    fd = open(fname->mn_val, which & FDS_O_MASK, mode);
#endif

    if (fd == INVALID_HANDLE_VALUE)
        rc = ErrCode();
#ifndef FDS_WINDOWS
    else
    {
        if (which != FDS_O_RDONLY && which != FDS_O_RDWR)
        {
            // Set CLOEXEC if we could not pass it to open()
            if (!FDS_CLOEXEC && (flags = fcntl(fd, F_GETFD)) != -1)
                (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
        if (which == FDS_O_COPY && env->me_psize >= env->me_os_psize)
        {
            // This may require buffer alignment.  There is no portable
            // way to ask how much, so we require OS pagesize alignment.
#ifdef F_NOCACHE  // __APPLE__
            (void)fcntl(fd, F_NOCACHE, 1);
#elif defined O_DIRECT
            // open(...O_DIRECT...) would break on filesystems without
            // O_DIRECT support (ITS#7682). Try to set it here instead.
            if ((flags = fcntl(fd, F_GETFL)) != -1)
                (void)fcntl(fd, F_SETFL, flags | O_DIRECT);
#endif
        }
    }
#endif  // !FDS_WINDOWS

    *res = fd;
    return rc;
}

auto fds_env_sync0(FDS_env* env, int force, pgno_t numpgs) -> int
{
    int rc = 0;
    if ((env->me_flags & FDS_RDONLY) != 0U)
        return EACCES;
    if (force != 0
#ifndef FDS_WINDOWS  // Sync is normally achieved in Windows by doing WRITE_THROUGH writes
        || !(env->me_flags & FDS_NOSYNC)
#endif
    )
    {
        if ((env->me_flags & FDS_WRITEMAP) != 0U)
        {
            int flags = (((env->me_flags & FDS_MAPASYNC) != 0U) && (force == 0)) ? MS_ASYNC : MS_SYNC;
            if (FDS_MSYNC(env->me_map, env->me_psize * numpgs, flags)
#if defined(FDS_WINDOWS) || defined(__APPLE__)
                || (flags == MS_SYNC && FDS_FDATASYNC(env->me_fd))
#endif
            )
            {
                rc = ErrCode();
            }
        }
        else if (FDS_FDATASYNC(env->me_fd))
        {
            rc = ErrCode();
        }
    }
    return rc;
}

auto fds_env_sync(FDS_env* env, int force) -> int
{
    FDS_meta* m = fds_env_pick_meta(env);
    return fds_env_sync0(env, force, m->mm_last_pg + 1);
}

// Read the environment parameters of a DB environment before
// mapping it into memory.
// env the environment handle
// prev whether to read the backup meta page
// meta address of where to store the meta information
// Return 0 on success, non-zero on failure.
auto ESECT fds_env_read_header(FDS_env* env, int prev, FDS_meta* meta) -> int
{
    // Buffer for a stack-allocated meta page.
    // The members define size and alignment, and silence type
    // aliasing warnings.  They are not used directly; that could
    // mean incorrectly using several union members in parallel.
    union FDS_metabuf
    {
        FDS_page mb_page;
        struct
        {
            char mm_pad[PAGEHDRSZ];
            FDS_meta mm_meta;
        } mb_metabuf;
    };

    FDS_metabuf pbuf;
    FDS_page* p;
    FDS_meta* m;
    int i;
    int rc;
    int off;
    enum
    {
        Size = sizeof(pbuf)
    };

    // We don't know the page size yet, so use a minimum value.
    // Read both meta pages so we can use the latest one.

    for (i = off = 0; i < NUM_METAS; i++, off += meta->mm_psize)
    {
#ifdef FDS_WINDOWS
        DWORD len;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = off;
        rc = (ReadFile(env->me_fd, &pbuf, Size, &len, &ov) != 0) ? (int)len : -1;
        if (rc == -1 && ErrCode() == ERROR_HANDLE_EOF)
            rc = 0;
#else
        rc = pread(env->me_fd, &pbuf, Size, off);
#endif
        if (rc != Size)
        {
            if (rc == 0 && off == 0)
                return ENOENT;
            rc = rc < 0 ? (int)ErrCode() : FDS_INVALID;
            DPRINTF(("read: %s", fds_strerror(rc)));
            return rc;
        }

        p = (FDS_page*)&pbuf;

        if (!F_ISSET(p->mp_flags, P_META))
        {
            DPRINTF(("page %" Yu " not a meta page", p->mp_pgno));
            return FDS_INVALID;
        }

        m = reinterpret_cast<FDS_meta*>(reinterpret_cast<char*>(p) + PAGEHDRSZ);
        if (m->mm_magic != FDS_MAGIC)
        {
            DPUTS("meta has invalid magic");
            return FDS_INVALID;
        }

        if (m->mm_version != FDS_DATA_VERSION)
        {
            DPRINTF(("database is version %u, expected version %u", m->mm_version, FDS_DATA_VERSION));
            return FDS_VERSION_MISMATCH;
        }

        if (off == 0 || ((prev != 0) ? m->mm_txnid < meta->mm_txnid : m->mm_txnid > meta->mm_txnid))
            *meta = *m;
    }
    return 0;
}

// Fill in most of the zeroed #FDS_meta for an empty database environment
void ESECT fds_env_init_meta0(FDS_env* env, FDS_meta* meta)
{
    meta->mm_magic = FDS_MAGIC;
    meta->mm_version = FDS_DATA_VERSION;
    meta->mm_mapsize = env->me_mapsize;
    meta->mm_psize = env->me_psize;
    meta->mm_last_pg = NUM_METAS - 1;
    meta->mm_flags = env->me_flags & 0xffff;
    meta->mm_dbs[FREE_DBI].md_root = P_INVALID;
    meta->mm_dbs[MAIN_DBI].md_root = P_INVALID;
}

// Write the environment parameters of a freshly created DB environment.
// env the environment handle
// meta the #FDS_meta to write
// Return 0 on success, non-zero on failure.
auto ESECT fds_env_init_meta(FDS_env* env, FDS_meta* meta) -> int
{
    FDS_page* p;
    FDS_page* q;
    int rc;
    unsigned int psize;
#ifdef FDS_WINDOWS
    DWORD len;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
#define DO_PWRITE(rc, fd, ptr, size, len, pos)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        ov.Offset = (pos);                                                                                             \
        (rc) = WriteFile((fd), (ptr), (size), &(len), &ov);                                                            \
    } while (0)
#else
    int len;
#define DO_PWRITE(rc, fd, ptr, size, len, pos)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        len = pwrite(fd, ptr, size, pos);                                                                              \
        if (len == -1 && ErrCode() == EINTR)                                                                           \
            continue;                                                                                                  \
        rc = (len >= 0);                                                                                               \
        break;                                                                                                         \
    } while (1)
#endif
    DPUTS("writing new meta page");

    psize = env->me_psize;

    p = (FDS_page*)calloc(NUM_METAS, psize);
    if (p == nullptr)
        return ENOMEM;
    p->mp_pgno = 0;
    p->mp_flags = P_META;
    *reinterpret_cast<FDS_meta*>(reinterpret_cast<char*>(p) + PAGEHDRSZ) = *meta;

    q = (FDS_page*)((char*)p + psize);
    q->mp_pgno = 1;
    q->mp_flags = P_META;
    *reinterpret_cast<FDS_meta*>(reinterpret_cast<char*>(q) + PAGEHDRSZ) = *meta;

    DO_PWRITE(rc, env->me_fd, p, psize * NUM_METAS, len, 0);
    if (rc == 0)
        rc = ErrCode();
    else if ((unsigned)len == psize * NUM_METAS)
        rc = FDS_SUCCESS;
    else
        rc = ENOSPC;
    free(p);
    return rc;
}

// Update the environment info to commit a transaction.
// txn the transaction that's being committed
// Return 0 on success, non-zero on failure.
#ifdef FDS_WINDOWS
auto fds_env_write_meta(FDS_txn* txn) -> int
{
    int toggle{static_cast<int>(txn->mt_txnid & 1)};
    DPRINTF(("writing meta page %d for root page %" Yu, toggle, txn->mt_dbs[MAIN_DBI].md_root));

    FDS_env* env{txn->mt_env};
    unsigned flags{txn->mt_flags | env->me_flags};
    FDS_meta* mp{env->me_metas[toggle]};
    fds_size_t mapsize{env->me_metas[toggle ^ 1]->mm_mapsize};
    // Persist any increases of mapsize config
    if (mapsize < env->me_mapsize)
        mapsize = env->me_mapsize;

    FDS_meta metab{};
    metab.mm_txnid = mp->mm_txnid;
    metab.mm_last_pg = mp->mm_last_pg;

    FDS_meta meta{};
    meta.mm_mapsize = mapsize;
    meta.mm_dbs[FREE_DBI] = txn->mt_dbs[FREE_DBI];
    meta.mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
    meta.mm_last_pg = txn->mt_next_pgno - 1;
    meta.mm_txnid = txn->mt_txnid;

    FDS_OFF_T off{offsetof(FDS_meta, mm_mapsize)};
    char* ptr{(char*)&meta + off};
    int len{static_cast<int>(sizeof(FDS_meta) - off)};
    off += (char*)mp - env->me_map;

    // Write to the SYNC fd unless FDS_NOSYNC/FDS_NOMETASYNC.
    // (me_mfd goes to the same file as me_fd, but writing to it
    // also syncs to disk.  Avoids a separate fdatasync() call.)
    HANDLE mfd{((flags & (FDS_NOSYNC | FDS_NOMETASYNC)) != 0U) ? env->me_fd : env->me_mfd};
    int rc{};
    {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = off;
        if (WriteFile(mfd, ptr, len, (DWORD*)&rc, &ov) == 0)
            rc = -1;
    }
    if (rc != len)
    {
        rc = rc < 0 ? ErrCode() : EIO;
        DPUTS("write failed, disk error?");
        // On a failure, the pagecache still contains the new data.
        // Write some old data back, to prevent it from being used.
        // Use the non-SYNC fd; we know it will fail anyway.
        meta.mm_last_pg = metab.mm_last_pg;
        meta.mm_txnid = metab.mm_txnid;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = off;
        WriteFile(env->me_fd, ptr, len, nullptr, &ov);
        env->me_flags |= FDS_FATAL_ERROR;
        return rc;
    }

    // Memory ordering issues are irrelevant; since the entire writer
    // is wrapped by wmutex, all of these changes will become visible
    // after the wmutex is unlocked. Since the DB is multi-version,
    // readers will get consistent data regardless of how fresh or
    // how stale their view of these values is.
    if (env->me_txns != nullptr)
        env->me_txns->mti_txnid = txn->mt_txnid;

    return FDS_SUCCESS;
}
#else
int fds_env_write_meta(FDS_txn* txn)
{
    int toggle{static_cast<int>(txn->mt_txnid & 1)};
    DPRINTF(("writing meta page %d for root page %" Yu, toggle, txn->mt_dbs[MAIN_DBI].md_root));

    FDS_env* env{txn->mt_env};
    unsigned flags{txn->mt_flags | env->me_flags};
    FDS_meta* mp{env->me_metas[toggle]};
    fds_size_t mapsize{env->me_metas[toggle ^ 1]->mm_mapsize};
    // Persist any increases of mapsize config
    if (mapsize < env->me_mapsize)
        mapsize = env->me_mapsize;

    if (flags & FDS_WRITEMAP)
    {
        mp->mm_mapsize = mapsize;
        mp->mm_dbs[FREE_DBI] = txn->mt_dbs[FREE_DBI];
        mp->mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
        mp->mm_last_pg = txn->mt_next_pgno - 1;
#if (__GNUC__ * 100 + __GNUC_MINOR__ >= 404) && /* TODO: portability */                                                \
    !(defined(__i386__) || defined(__x86_64__))
        // LY: issue a memory barrier, if not x86. ITS#7969
        __sync_synchronize();
#endif
        mp->mm_txnid = txn->mt_txnid;
        if (!(flags & (FDS_NOMETASYNC | FDS_NOSYNC)))
        {
            unsigned meta_size = env->me_psize;
            int rc = (env->me_flags & FDS_MAPASYNC) ? MS_ASYNC : MS_SYNC;
            char* ptr = (char*)mp - PAGEHDRSZ;
            // POSIX msync() requires ptr = start of OS page
            int r2 = (ptr - env->me_map) & (env->me_os_psize - 1);
            ptr -= r2;
            meta_size += r2;
            if (FDS_MSYNC(ptr, meta_size, rc))
            {
                rc = ErrCode();
                env->me_flags |= FDS_FATAL_ERROR;
                return rc;
            }
        }

        // Memory ordering issues are irrelevant; since the entire writer
        // is wrapped by wmutex, all of these changes will become visible
        // after the wmutex is unlocked. Since the DB is multi-version,
        // readers will get consistent data regardless of how fresh or
        // how stale their view of these values is.
        if (env->me_txns)
            env->me_txns->mti_txnid = txn->mt_txnid;

        return FDS_SUCCESS;
    }

    FDS_meta metab{};
    metab.mm_txnid = mp->mm_txnid;
    metab.mm_last_pg = mp->mm_last_pg;

    FDS_meta meta{};
    meta.mm_mapsize = mapsize;
    meta.mm_dbs[FREE_DBI] = txn->mt_dbs[FREE_DBI];
    meta.mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
    meta.mm_last_pg = txn->mt_next_pgno - 1;
    meta.mm_txnid = txn->mt_txnid;

    FDS_OFF_T off{offsetof(FDS_meta, mm_mapsize)};
    char* ptr{(char*)&meta + off};
    int len{static_cast<int>(sizeof(FDS_meta) - off)};
    off += (char*)mp - env->me_map;

    while (true)
    {
        // Write to the SYNC fd unless FDS_NOSYNC/FDS_NOMETASYNC.
        // (me_mfd goes to the same file as me_fd, but writing to it
        // also syncs to disk.  Avoids a separate fdatasync() call.)
        HANDLE mfd{(flags & (FDS_NOSYNC | FDS_NOMETASYNC)) ? env->me_fd : env->me_mfd};
        int rc{};
        rc = pwrite(mfd, ptr, len, off);
        if (rc == len)
            break;

        rc = rc < 0 ? ErrCode() : EIO;
        if (rc == EINTR)
            continue;

        DPUTS("write failed, disk error?");
        // On a failure, the pagecache still contains the new data.
        // Write some old data back, to prevent it from being used.
        // Use the non-SYNC fd; we know it will fail anyway.
        meta.mm_last_pg = metab.mm_last_pg;
        meta.mm_txnid = metab.mm_txnid;
        pwrite(env->me_fd, ptr, len, off);
        env->me_flags |= FDS_FATAL_ERROR;
        return rc;
    }

    // Memory ordering issues are irrelevant; since the entire writer
    // is wrapped by wmutex, all of these changes will become visible
    // after the wmutex is unlocked. Since the DB is multi-version,
    // readers will get consistent data regardless of how fresh or
    // how stale their view of these values is.
    if (env->me_txns)
        env->me_txns->mti_txnid = txn->mt_txnid;

    return FDS_SUCCESS;
}
#endif

// Check both meta pages to see which one is newer.
// env the environment handle
// Return newest #FDS_meta.
auto fds_env_pick_meta(const FDS_env* env) -> FDS_meta*
{
    FDS_meta* const* metas = env->me_metas;
    return metas[(metas[0]->mm_txnid < metas[1]->mm_txnid) ^ ((env->me_flags & FDS_PREVSNAPSHOT) != 0)];
}

auto ESECT fds_env_create(FDS_env** env) -> int
{
    FDS_env* e;

    e = (FDS_env*)calloc(1, sizeof(FDS_env));
    if (e == nullptr)
        return ENOMEM;

    e->me_maxreaders = DEFAULT_READERS;
    e->me_maxdbs = e->me_numdbs = CORE_DBS;
    e->me_fd = INVALID_HANDLE_VALUE;
    e->me_lfd = INVALID_HANDLE_VALUE;
    e->me_mfd = INVALID_HANDLE_VALUE;
#ifdef FDS_USE_POSIX_SEM
    e->me_rmutex = SEM_FAILED;
    e->me_wmutex = SEM_FAILED;
#elif defined FDS_USE_SYSV_SEM
    e->me_rmutex->semid = -1;
    e->me_wmutex->semid = -1;
#endif
    e->me_pid = getpid();
    GET_PAGESIZE(e->me_os_psize);
    VGMEMP_CREATE(e, 0, 0);
    *env = e;
    DPRINTF(("%p", e));
    return FDS_SUCCESS;
}

#ifdef FDS_WINDOWS
// Map a result from an NTAPI call to WIN32.
static auto fds_nt2win32(NTSTATUS st) -> DWORD
{
    OVERLAPPED o = {0};
    DWORD br;
    o.Internal = st;
    GetOverlappedResult(nullptr, &o, &br, FALSE);
    return GetLastError();
}
#endif

auto ESECT fds_env_map(FDS_env* env, void* addr) -> int
{
    FDS_page* p;
    unsigned int flags = env->me_flags;
#ifdef FDS_WINDOWS
    int rc;
    int access = SECTION_MAP_READ;
    HANDLE mh;
    void* map;
    SIZE_T msize;
    ULONG pageprot = PAGE_READONLY;
    ULONG secprot;
    ULONG alloctype;

    if ((flags & FDS_WRITEMAP) != 0U)
    {
        access |= SECTION_MAP_WRITE;
        pageprot = PAGE_READWRITE;
    }
    if ((flags & FDS_RDONLY) != 0U)
    {
        secprot = PAGE_READONLY;
        msize = 0;
        alloctype = 0;
    }
    else
    {
        secprot = PAGE_READWRITE;
        msize = env->me_mapsize;
        alloctype = MEM_RESERVE;
    }

    // Some users are afraid of seeing their disk space getting used
    // all at once, so the default is now to do incremental file growth.
    // But that has a large performance impact, so give the option of
    // allocating the file up front.
#ifdef FDS_FIXEDSIZE
    LARGE_INTEGER fsize;
    fsize.LowPart = msize & 0xffffffff;
    fsize.HighPart = msize >> 16 >> 16;
    rc = NtCreateSection(&mh, access, NULL, &fsize, secprot, SEC_RESERVE, env->me_fd);
#else
    rc = NtCreateSection(&mh, access, nullptr, nullptr, secprot, SEC_RESERVE, env->me_fd);
#endif
    if (rc != 0)
        return fds_nt2win32(rc);
    map = addr;
    rc = NtMapViewOfSection(mh, GetCurrentProcess(), &map, 0, 0, nullptr, &msize, ViewUnmap, alloctype, pageprot);
    NtClose(mh);
    if (rc != 0)
        return fds_nt2win32(rc);
    env->me_map = (char*)map;
#else
    int mmap_flags = MAP_SHARED;
    int prot = PROT_READ;
#ifdef MAP_NOSYNC  // Used on FreeBSD
    if (flags & FDS_NOSYNC)
        mmap_flags |= MAP_NOSYNC;
#endif
    if (flags & FDS_WRITEMAP)
    {
        prot |= PROT_WRITE;
        if (ftruncate(env->me_fd, env->me_mapsize) < 0)
            return ErrCode();
    }
    env->me_map = (char*)mmap(addr, env->me_mapsize, prot, mmap_flags, env->me_fd, 0);
    if (env->me_map == MAP_FAILED)
    {
        env->me_map = NULL;
        return ErrCode();
    }

    if (flags & FDS_NORDAHEAD)
    {
        // Turn off readahead. It's harmful when the DB is larger than RAM.
#ifdef MADV_RANDOM
        madvise(env->me_map, env->me_mapsize, MADV_RANDOM);
#else
#ifdef POSIX_MADV_RANDOM
        posix_madvise(env->me_map, env->me_mapsize, POSIX_MADV_RANDOM);
#endif  // POSIX_MADV_RANDOM
#endif  // MADV_RANDOM
    }
#endif  // FDS_WINDOWS

    // Can happen because the address argument to mmap() is just a
    // hint.  mmap() can pick another, e.g. if the range is in use.
    // The MAP_FIXED flag would prevent that, but then mmap could
    // instead unmap existing pages to make room for the new map.
    if ((addr != nullptr) && env->me_map != addr)
        return EBUSY;  // TODO: Make a new FDS_* error code?

    p = (FDS_page*)env->me_map;
    env->me_metas[0] = reinterpret_cast<FDS_meta*>(reinterpret_cast<char*>(p) + PAGEHDRSZ);
    env->me_metas[1] = (FDS_meta*)((char*)env->me_metas[0] + env->me_psize);

    return FDS_SUCCESS;
}

auto ESECT fds_env_set_mapsize(FDS_env* env, fds_size_t size) -> int
{
    // If env is already open, caller is responsible for making
    // sure there are no active txns.
    if (env->me_map != nullptr)
    {
        FDS_meta* meta;
        void* old;
        int rc;

        if (env->me_txn != nullptr)
            return EINVAL;
        meta = fds_env_pick_meta(env);
        if (size == 0U)
            size = meta->mm_mapsize;
        {
            // Silently round up to minimum if the size is too small
            fds_size_t minsize = (meta->mm_last_pg + 1) * env->me_psize;
            if (size < minsize)
                size = minsize;
        }

        munmap(env->me_map, env->me_mapsize);
        env->me_mapsize = size;
        old = nullptr;
        rc = fds_env_map(env, old);
        if (rc != 0)
            return rc;
    }
    env->me_mapsize = size;
    if (env->me_psize != 0U)
        env->me_maxpg = env->me_mapsize / env->me_psize;
    DPRINTF(("%p, %" Yu "", env, size));
    return FDS_SUCCESS;
}

auto ESECT fds_env_set_maxdbs(FDS_env* env, FDS_dbi dbs) -> int
{
    if (env->me_map != nullptr)
        return EINVAL;
    env->me_maxdbs = dbs + CORE_DBS;
    DPRINTF(("%p, %u", env, dbs));
    return FDS_SUCCESS;
}

auto ESECT fds_env_set_maxreaders(FDS_env* env, unsigned int readers) -> int
{
    if ((env->me_map != nullptr) || readers < 1)
        return EINVAL;
    env->me_maxreaders = readers;
    DPRINTF(("%p, %u", env, readers));
    return FDS_SUCCESS;
}

auto ESECT fds_env_get_maxreaders(FDS_env* env, unsigned int* readers) -> int
{
    if ((env == nullptr) || (readers == nullptr))
        return EINVAL;
    *readers = env->me_maxreaders;
    return FDS_SUCCESS;
}

// Further setup required for opening an FiksDataStore environment
auto ESECT fds_env_open2(FDS_env* env, int prev) -> int
{
    unsigned int flags = env->me_flags;
    int i;
    int newenv = 0;
    int rc;
    FDS_meta meta;

#ifdef FDS_WINDOWS
    // See if we should use QueryLimited
    // Use GetVersionEx instead of deprecated GetVersion
    OSVERSIONINFO osvi;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

#pragma warning(push)
#pragma warning(disable : 4996)  // Suppress deprecation warning
    if (GetVersionEx(&osvi) && osvi.dwMajorVersion > 5)
#pragma warning(pop)
        env->me_pidquery = FDS_PROCESS_QUERY_LIMITED_INFORMATION;
    else
        env->me_pidquery = PROCESS_QUERY_INFORMATION;
    // Grab functions we need from NTDLL
    if (NtCreateSection == nullptr)
    {
        HMODULE h = GetModuleHandleW(L"NTDLL.DLL");
        if (h == nullptr)
            return FDS_PROBLEM;
        NtClose = (NtCloseFunc)GetProcAddress(h, "NtClose");
        if (NtClose == nullptr)
            return FDS_PROBLEM;
        NtMapViewOfSection = (NtMapViewOfSectionFunc)GetProcAddress(h, "NtMapViewOfSection");
        if (NtMapViewOfSection == nullptr)
            return FDS_PROBLEM;
        NtCreateSection = (NtCreateSectionFunc)GetProcAddress(h, "NtCreateSection");
        if (NtCreateSection == nullptr)
            return FDS_PROBLEM;
    }
    env->ovs = 0;
#endif  // FDS_WINDOWS

    i = fds_env_read_header(env, prev, &meta);
    if (i != 0)
    {
        if (i != ENOENT)
            return i;
        DPUTS("new mdbenv");
        newenv = 1;
        env->me_psize = env->me_os_psize;
        if (env->me_psize > MAX_PAGESIZE)
            env->me_psize = MAX_PAGESIZE;
        memset(&meta, 0, sizeof(meta));
        fds_env_init_meta0(env, &meta);
        meta.mm_mapsize = DEFAULT_MAPSIZE;
    }
    else
    {
        env->me_psize = meta.mm_psize;
    }

    // Was a mapsize configured?
    if (env->me_mapsize == 0U)
    {
        env->me_mapsize = meta.mm_mapsize;
    }
    {
        // Make sure mapsize >= committed data size.  Even when using
        // mm_mapsize, which could be broken in old files (ITS#7789).
        fds_size_t minsize = (meta.mm_last_pg + 1) * meta.mm_psize;
        if (env->me_mapsize < minsize)
            env->me_mapsize = minsize;
    }
    meta.mm_mapsize = env->me_mapsize;

    if (newenv != 0)
    {
        // fds_env_map() may grow the datafile.  Write the metapages
        // first, so the file will be valid if initialization fails.
        rc = fds_env_init_meta(env, &meta);
        if (rc != 0)
            return rc;
        newenv = 0;
    }
#ifdef FDS_WINDOWS
    // For FIXEDMAP, make sure the file is non-empty before we attempt to map it
    if (newenv != 0)
    {
        char dummy = 0;
        DWORD len;
        rc = WriteFile(env->me_fd, &dummy, 1, &len, nullptr);
        if (rc == 0)
        {
            rc = ErrCode();
            return rc;
        }
    }
#endif

    rc = fds_env_map(env, nullptr);
    if (rc != 0)
        return rc;

    if (newenv != 0)
    {
        i = fds_env_init_meta(env, &meta);
        if (i != FDS_SUCCESS)
        {
            return i;
        }
    }

    env->me_maxfree_1pg = (env->me_psize - PAGEHDRSZ) / sizeof(pgno_t) - 1;
    env->me_nodemax = (((env->me_psize - PAGEHDRSZ) / FDS_MINKEYS) & -2) - sizeof(indx_t);
#if !(FDS_MAXKEYSIZE)
    env->me_maxkey = env->me_nodemax - (NODESIZE + sizeof(FDS_db));
#endif
    env->me_maxpg = env->me_mapsize / env->me_psize;

    if ((prev != 0) && (env->me_txns != nullptr))
        env->me_txns->mti_txnid = meta.mm_txnid;

#if FDS_DEBUG
    {
        FDS_meta* meta = fds_env_pick_meta(env);
        FDS_db* db = &meta->mm_dbs[MAIN_DBI];

        DPRINTF(("opened database version %u, pagesize %u", meta->mm_version, env->me_psize));
        DPRINTF(("using meta page %d", (int)(meta->mm_txnid & 1)));
        DPRINTF(("depth: %u", db->md_depth));
        DPRINTF(("entries: %" Yu, db->md_entries));
        DPRINTF(("branch pages: %" Yu, db->md_branch_pages));
        DPRINTF(("leaf pages: %" Yu, db->md_leaf_pages));
        DPRINTF(("overflow pages: %" Yu, db->md_overflow_pages));
        DPRINTF(("root: %" Yu, db->md_root));
    }
#endif

    return FDS_SUCCESS;
}

// Release a reader thread's slot in the reader lock table.
// This function is called automatically when a thread exits.
// ptr This points to the slot in the reader lock table.
static void fds_env_reader_dest(void* ptr)
{
    auto* reader = (FDS_reader*)ptr;

#ifndef FDS_WINDOWS
    if (reader->mr_pid == getpid())  // catch pthread_exit() in child process
#endif
        // We omit the mutex, so do this atomically (i.e. skip mr_txnid)
        reader->mr_pid = 0;
}

// Downgrade the exclusive lock on the region back to shared
auto ESECT fds_env_share_locks(FDS_env* env, int* excl) -> int
{
    int rc = 0;
    FDS_meta* meta = fds_env_pick_meta(env);

    env->me_txns->mti_txnid = meta->mm_txnid;

#ifdef FDS_WINDOWS
    {
        OVERLAPPED ov;
        // First acquire a shared lock. The Unlock will
        // then release the existing exclusive lock.
        memset(&ov, 0, sizeof(ov));
        if (LockFileEx(env->me_lfd, 0, 0, 1, 0, &ov) == 0)
        {
            rc = ErrCode();
        }
        else
        {
            UnlockFile(env->me_lfd, 0, 0, 1, 0);
            *excl = 0;
        }
    }
#else
    {
        struct flock lock_info;
        // The shared lock replaces the existing lock
        memset((void*)&lock_info, 0, sizeof(lock_info));
        lock_info.l_type = F_RDLCK;
        lock_info.l_whence = SEEK_SET;
        lock_info.l_start = 0;
        lock_info.l_len = 1;
        while ((rc = fcntl(env->me_lfd, F_SETLK, &lock_info)) && (rc = ErrCode()) == EINTR)
            ;
        *excl = rc ? -1 : 0;  // error may mean we lost the lock
    }
#endif

    return rc;
}

// Try to get exclusive lock, otherwise shared.
// Maintain *excl = -1: no/unknown lock, 0: shared, 1: exclusive.
auto ESECT fds_env_excl_lock(FDS_env* env, int* excl) -> int
{
    int rc = 0;
#ifdef FDS_WINDOWS
    if (LockFile(env->me_lfd, 0, 0, 1, 0) != 0)
    {
        *excl = 1;
    }
    else
    {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        if (LockFileEx(env->me_lfd, 0, 0, 1, 0, &ov) != 0)
        {
            *excl = 0;
        }
        else
        {
            rc = ErrCode();
        }
    }
#else
    struct flock lock_info;
    memset((void*)&lock_info, 0, sizeof(lock_info));
    lock_info.l_type = F_WRLCK;
    lock_info.l_whence = SEEK_SET;
    lock_info.l_start = 0;
    lock_info.l_len = 1;
    while ((rc = fcntl(env->me_lfd, F_SETLK, &lock_info)) && (rc = ErrCode()) == EINTR)
        ;
    if (!rc)
    {
        *excl = 1;
    }
    else
#ifndef FDS_USE_POSIX_MUTEX
        if (*excl < 0)  // always true when FDS_USE_POSIX_MUTEX
#endif
    {
        lock_info.l_type = F_RDLCK;
        while ((rc = fcntl(env->me_lfd, F_SETLKW, &lock_info)) && (rc = ErrCode()) == EINTR)
            ;
        if (rc == 0)
            *excl = 0;
    }
#endif
    return rc;
}

#if defined(FDS_WINDOWS) || defined(FDS_USE_POSIX_SEM)

// Init #FDS_env.me_mutexname[] except the char which #MUTEXNAME() will set.
// Changes to this code must be reflected in #FDS_LOCK_FORMAT.
void ESECT fds_env_mname_init(FDS_env* env)
{
    char* nm = env->me_mutexname;
#pragma warning(push)
#pragma warning(disable : 4996)  // Suppress deprecation warning for strcpy
    strcpy(nm, MUTEXNAME_PREFIX);
#pragma warning(pop)
    fds_pack85(env->me_txns->mti_mutexid, nm + sizeof(MUTEXNAME_PREFIX));
}

// Return env->me_mutexname after filling in ch ('r'/'w') for convenience
#define MUTEXNAME(env, ch) ((void)((env)->me_mutexname[sizeof(MUTEXNAME_PREFIX) - 1] = (ch)), (env)->me_mutexname)

#endif

// Open and/or initialize the lock region for the environment.
// env The FiksDataStore environment.
// fname Filename + scratch area, from #fds_fname_init().
// mode The Unix permissions for the file, if we create it.
// excl In -1, out lock type: -1 none, 0 shared, 1 exclusive
// Return 0 on success, non-zero on failure.
auto ESECT fds_env_setup_locks(FDS_env* env, FDS_name* fname, int mode, int* excl) -> int
{
#ifdef FDS_WINDOWS
#define FDS_ERRCODE_ROFS ERROR_WRITE_PROTECT
#else
#define FDS_ERRCODE_ROFS EROFS
#endif
#ifdef FDS_USE_SYSV_SEM
    int semid{};
    union semun semu{};
#endif
    int rc{};
    FDS_OFF_T size{};
    FDS_OFF_T rsize{};

    rc = fds_fopen(env, fname, FDS_O_LOCKS, mode, &env->me_lfd);
    if (rc != 0)
    {
        // Omit lockfile if read-only env on read-only filesystem
        if (rc == FDS_ERRCODE_ROFS && ((env->me_flags & FDS_RDONLY) != 0U))
        {
            return FDS_SUCCESS;
        }
        goto fail;
    }

    if ((env->me_flags & FDS_NOTLS) == 0U)
    {
        rc = pthread_key_create(&env->me_txkey, fds_env_reader_dest);
        if (rc != 0)
            goto fail;
        env->me_flags |= FDS_ENV_TXKEY;
#ifdef FDS_WINDOWS
        // Windows TLS callbacks need help finding their TLS info.
        if (fds_tls_nkeys >= MAX_TLS_KEYS)
        {
            rc = FDS_TLS_FULL;
            goto fail;
        }
        fds_tls_keys[fds_tls_nkeys++] = env->me_txkey;
#endif
    }

    // Try to get exclusive lock. If we succeed, then
    // nobody is using the lock region and we should initialize it.
    rc = fds_env_excl_lock(env, excl);
    if (rc != 0)
        goto fail;

#ifdef FDS_WINDOWS
    size = GetFileSize(env->me_lfd, nullptr);
#else
    size = lseek(env->me_lfd, 0, SEEK_END);
    if (size == -1)
        goto fail_errno;
#endif
    rsize = (env->me_maxreaders - 1) * sizeof(FDS_reader) + sizeof(FDS_txninfo);
    if (size < rsize && *excl > 0)
    {
#ifdef FDS_WINDOWS
        if (SetFilePointer(env->me_lfd, rsize, nullptr, FILE_BEGIN) != (DWORD)rsize || (SetEndOfFile(env->me_lfd) == 0))
            goto fail_errno;
#else
        if (ftruncate(env->me_lfd, rsize) != 0)
            goto fail_errno;
#endif
    }
    else
    {
        rsize = size;
        size = rsize - sizeof(FDS_txninfo);
        env->me_maxreaders = size / sizeof(FDS_reader) + 1;
    }
    {
#ifdef FDS_WINDOWS
        HANDLE mh;
        mh = CreateFileMapping(env->me_lfd, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (mh == nullptr)
            goto fail_errno;
        env->me_txns = (FDS_txninfo*)MapViewOfFileEx(mh, FILE_MAP_WRITE, 0, 0, rsize, nullptr);
        CloseHandle(mh);
        if (env->me_txns == nullptr)
            goto fail_errno;
#else
        void* m = mmap(NULL, rsize, PROT_READ | PROT_WRITE, MAP_SHARED, env->me_lfd, 0);
        if (m == MAP_FAILED)
            goto fail_errno;
        env->me_txns = (FDS_txninfo*)m;
#endif
    }
    if (*excl > 0)
    {
#ifdef FDS_WINDOWS
        BY_HANDLE_FILE_INFORMATION stbuf;
        struct
        {
            DWORD volume;
            DWORD nhigh;
            DWORD nlow;
        } idbuf;

        if (fds_sec_inited == 0)
        {
            InitializeSecurityDescriptor(&fds_null_sd, SECURITY_DESCRIPTOR_REVISION);
            SetSecurityDescriptorDacl(&fds_null_sd, TRUE, nullptr, FALSE);
            fds_all_sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            fds_all_sa.bInheritHandle = FALSE;
            fds_all_sa.lpSecurityDescriptor = &fds_null_sd;
            fds_sec_inited = 1;
        }
        if (GetFileInformationByHandle(env->me_lfd, &stbuf) == 0)
            goto fail_errno;
        idbuf.volume = stbuf.dwVolumeSerialNumber;
        idbuf.nhigh = stbuf.nFileIndexHigh;
        idbuf.nlow = stbuf.nFileIndexLow;
        env->me_txns->mti_mutexid = fds_hash(&idbuf, sizeof(idbuf));
        fds_env_mname_init(env);
        env->me_rmutex = CreateMutexA(&fds_all_sa, FALSE, MUTEXNAME(env, 'r'));
        if (env->me_rmutex == nullptr)
            goto fail_errno;
        env->me_wmutex = CreateMutexA(&fds_all_sa, FALSE, MUTEXNAME(env, 'w'));
        if (env->me_wmutex == nullptr)
            goto fail_errno;
#elif defined(FDS_USE_POSIX_SEM)
        struct stat stbuf;
        struct
        {
            dev_t dev;
            ino_t ino;
        } idbuf;

#if defined(__NetBSD__)
#define FDS_SHORT_SEMNAMES 1  // limited to 14 chars
#endif
        if (fstat(env->me_lfd, &stbuf))
            goto fail_errno;
        memset(&idbuf, 0, sizeof(idbuf));
        idbuf.dev = stbuf.st_dev;
        idbuf.ino = stbuf.st_ino;
        env->me_txns->mti_mutexid = fds_hash(&idbuf, sizeof(idbuf))
#ifdef FDS_SHORT_SEMNAMES
                                    /* Max 9 base85-digits.  We truncate here instead of in
                                     * fds_env_mname_init() to keep the latter portable.
                                     */
                                    % ((fds_hash_t)85 * 85 * 85 * 85 * 85 * 85 * 85 * 85 * 85)
#endif
            ;
        fds_env_mname_init(env);
        // Clean up after a previous run, if needed:  Try to
        // remove both semaphores before doing anything else.
        sem_unlink(MUTEXNAME(env, 'r'));
        sem_unlink(MUTEXNAME(env, 'w'));
        env->me_rmutex = sem_open(MUTEXNAME(env, 'r'), O_CREAT | O_EXCL, mode, 1);
        if (env->me_rmutex == SEM_FAILED)
            goto fail_errno;
        env->me_wmutex = sem_open(MUTEXNAME(env, 'w'), O_CREAT | O_EXCL, mode, 1);
        if (env->me_wmutex == SEM_FAILED)
            goto fail_errno;
#elif defined(FDS_USE_SYSV_SEM)
        unsigned short vals[2] = {1, 1};
        key_t key = ftok(fname->mn_val, 'M');  // fname is lockfile path now
        if (key == -1)
            goto fail_errno;
        semid = semget(key, 2, (mode & 0777) | IPC_CREAT);
        if (semid < 0)
            goto fail_errno;
        semu.array = vals;
        if (semctl(semid, 0, SETALL, semu) < 0)
            goto fail_errno;
        env->me_txns->mti_semid = semid;
        env->me_txns->mti_rlocked = 0;
        env->me_txns->mti_wlocked = 0;
#else   // FDS_USE_POSIX_MUTEX:
        pthread_mutexattr_t mattr;

        // Solaris needs this before initing a robust mutex.  Otherwise
        // it may skip the init and return EBUSY "seems someone already
        // inited" or EINVAL "it was inited differently".
        memset(env->me_txns->mti_rmutex, 0, sizeof(*env->me_txns->mti_rmutex));
        memset(env->me_txns->mti_wmutex, 0, sizeof(*env->me_txns->mti_wmutex));

        if ((rc = pthread_mutexattr_init(&mattr)) != 0)
            goto fail;
        rc = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
        if (!rc)
            rc = pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
        if (!rc)
            rc = pthread_mutex_init(env->me_txns->mti_rmutex, &mattr);
        if (!rc)
            rc = pthread_mutex_init(env->me_txns->mti_wmutex, &mattr);
        pthread_mutexattr_destroy(&mattr);
        if (rc)
            goto fail;
#endif  // FDS_WINDOWS || ...

        env->me_txns->mti_magic = FDS_MAGIC;
        env->me_txns->mti_format = FDS_LOCK_FORMAT;
        env->me_txns->mti_txnid = 0;
        env->me_txns->mti_numreaders = 0;
    }
    else
    {
#ifdef FDS_USE_SYSV_SEM
        struct semid_ds buf;
#endif
        if (env->me_txns->mti_magic != FDS_MAGIC)
        {
            DPUTS("lock region has invalid magic");
            rc = FDS_INVALID;
            goto fail;
        }
        if (env->me_txns->mti_format != FDS_LOCK_FORMAT)
        {
            DPRINTF(("lock region has format+version 0x%x, expected 0x%x", env->me_txns->mti_format, FDS_LOCK_FORMAT));
            rc = FDS_VERSION_MISMATCH;
            goto fail;
        }
        rc = ErrCode();
        if ((rc != 0) && rc != EACCES && rc != EAGAIN)
        {
            goto fail;
        }
#ifdef FDS_WINDOWS
        fds_env_mname_init(env);
        env->me_rmutex = OpenMutexA(SYNCHRONIZE, FALSE, MUTEXNAME(env, 'r'));
        if (env->me_rmutex == nullptr)
            goto fail_errno;
        env->me_wmutex = OpenMutexA(SYNCHRONIZE, FALSE, MUTEXNAME(env, 'w'));
        if (env->me_wmutex == nullptr)
            goto fail_errno;
#elif defined(FDS_USE_POSIX_SEM)
        fds_env_mname_init(env);
        env->me_rmutex = sem_open(MUTEXNAME(env, 'r'), 0);
        if (env->me_rmutex == SEM_FAILED)
            goto fail_errno;
        env->me_wmutex = sem_open(MUTEXNAME(env, 'w'), 0);
        if (env->me_wmutex == SEM_FAILED)
            goto fail_errno;
#elif defined(FDS_USE_SYSV_SEM)
        semid = env->me_txns->mti_semid;
        semu.buf = &buf;
        // check for read access
        if (semctl(semid, 0, IPC_STAT, semu) < 0)
            goto fail_errno;
        // check for write access
        if (semctl(semid, 0, IPC_SET, semu) < 0)
            goto fail_errno;
#endif
    }
#ifdef FDS_USE_SYSV_SEM
    env->me_rmutex->semid = semid;
    env->me_wmutex->semid = semid;
    env->me_rmutex->semnum = 0;
    env->me_wmutex->semnum = 1;
    env->me_rmutex->locked = &env->me_txns->mti_rlocked;
    env->me_wmutex->locked = &env->me_txns->mti_wlocked;
#endif

    return FDS_SUCCESS;

fail_errno:
    rc = ErrCode();
fail:
    return rc;
}

// Only a subset of the fds_env flags can be changed
// at runtime. Changing other flags requires closing the
// environment and re-opening it with the new flags.
#define CHANGEABLE (FDS_NOSYNC | FDS_NOMETASYNC | FDS_MAPASYNC | FDS_NOMEMINIT)
#define CHANGELESS                                                                                                     \
    (FDS_NOSUBDIR | FDS_RDONLY | FDS_WRITEMAP | FDS_NOTLS | FDS_NOLOCK | FDS_NORDAHEAD | FDS_PREVSNAPSHOT)

#if VALID_FLAGS & PERSISTENT_FLAGS & (CHANGEABLE | CHANGELESS)
#error "Persistent DB flags & env flags overlap, but both go in mm_flags"
#endif

auto ESECT fds_env_open(FDS_env* env, const char* path, unsigned int flags, fds_mode_t mode) -> int
{
    int rc;
    int excl = -1;
    FDS_name fname;

    if (env->me_fd != INVALID_HANDLE_VALUE || ((flags & ~(CHANGEABLE | CHANGELESS)) != 0U))
        return EINVAL;

    flags |= env->me_flags;

    rc = fds_fname_init(path, flags, &fname);
    if (rc != 0)
        return rc;

    flags |= FDS_ENV_ACTIVE;  // tell fds_env_close0() to clean up

    if ((flags & FDS_RDONLY) != 0U)
    {
        // silently ignore WRITEMAP when we're only getting read access
        flags &= ~FDS_WRITEMAP;
    }
    else
    {
        env->me_free_pgs = fds_midl_alloc(FDS_IDL_UM_MAX);
        env->me_dirty_list = (FDS_ID2L)calloc(FDS_IDL_UM_SIZE, sizeof(FDS_ID2));
        if ((env->me_free_pgs == nullptr) || (env->me_dirty_list == nullptr))
            rc = ENOMEM;
    }

    env->me_flags = flags;
    if (rc != 0)
        goto leave;

    env->me_path = fds_strdup(path);
    env->me_dbxs = (FDS_dbx*)calloc(env->me_maxdbs, sizeof(FDS_dbx));
    env->me_dbflags = (uint16_t*)calloc(env->me_maxdbs, sizeof(uint16_t));
    env->me_dbiseqs = (unsigned int*)calloc(env->me_maxdbs, sizeof(unsigned int));
    if ((env->me_dbxs == nullptr) || (env->me_path == nullptr) || (env->me_dbflags == nullptr) ||
        (env->me_dbiseqs == nullptr))
    {
        rc = ENOMEM;
        goto leave;
    }
    env->me_dbxs[FREE_DBI].md_cmp = fds_cmp_long;  // aligned FDS_INTEGERKEY

    // For RDONLY, get lockfile after we know datafile exists
    if ((flags & (FDS_RDONLY | FDS_NOLOCK)) == 0U)
    {
        rc = fds_env_setup_locks(env, &fname, mode, &excl);
        if (rc != 0)
            goto leave;
        if (((flags & FDS_PREVSNAPSHOT) != 0U) && (excl == 0))
        {
            rc = EAGAIN;
            goto leave;
        }
    }

    rc = fds_fopen(env, &fname, ((flags & FDS_RDONLY) != 0U) ? FDS_O_RDONLY : FDS_O_RDWR, mode, &env->me_fd);
    if (rc != 0)
        goto leave;
#ifdef FDS_WINDOWS
    rc = fds_fopen(env, &fname, FDS_O_OVERLAPPED, mode, &env->me_ovfd);
    if (rc != 0)
        goto leave;
#endif

    if ((flags & (FDS_RDONLY | FDS_NOLOCK)) == FDS_RDONLY)
    {
        rc = fds_env_setup_locks(env, &fname, mode, &excl);
        if (rc != 0)
            goto leave;
    }

    rc = fds_env_open2(env, flags & FDS_PREVSNAPSHOT);
    if (rc == FDS_SUCCESS)
    {
        // Synchronous fd for meta writes. Needed even with
        // FDS_NOSYNC/FDS_NOMETASYNC, in case these get reset.
        if ((flags & (FDS_RDONLY | FDS_WRITEMAP)) == 0U)
        {
            rc = fds_fopen(env, &fname, FDS_O_META, mode, &env->me_mfd);
            if (rc != 0)
                goto leave;
        }
        DPRINTF(("opened dbenv %p", (void*)env));
        if (excl > 0 && ((flags & FDS_PREVSNAPSHOT) == 0U))
        {
            rc = fds_env_share_locks(env, &excl);
            if (rc != 0)
                goto leave;
        }
        if ((flags & FDS_RDONLY) == 0U)
        {
            FDS_txn* txn;
            int tsize = sizeof(FDS_txn);
            int size = tsize + (env->me_maxdbs * (sizeof(FDS_db) + sizeof(FDS_cursor*) + sizeof(unsigned int) + 1));
            env->me_pbuf = calloc(1, env->me_psize);
            txn = (FDS_txn*)calloc(1, size);
            if ((env->me_pbuf != nullptr) && (txn != nullptr))
            {
                txn->mt_dbs = (FDS_db*)((char*)txn + tsize);
                txn->mt_cursors = (FDS_cursor**)(txn->mt_dbs + env->me_maxdbs);
                txn->mt_dbiseqs = (unsigned int*)(txn->mt_cursors + env->me_maxdbs);
                txn->mt_dbflags = (unsigned char*)(txn->mt_dbiseqs + env->me_maxdbs);
                txn->mt_env = env;
                txn->mt_dbxs = env->me_dbxs;
                txn->mt_flags = FDS_TXN_FINISHED;
                env->me_txn0 = txn;
            }
            else
            {
                rc = ENOMEM;
            }
        }
    }

leave:
    DPRINTF(("%p, %s, %u, %04o", env, path, flags & (CHANGEABLE | CHANGELESS), mode));
    if (rc != 0)
    {
        fds_env_close0(env, excl);
    }
    fds_fname_destroy(fname);
    return rc;
}

// Destroy resources from fds_env_open(), clear our readers & DBIs
void ESECT fds_env_close0(FDS_env* env, int excl)
{
    int i;

    if ((env->me_flags & FDS_ENV_ACTIVE) == 0U)
        return;

    // Doing this here since me_dbxs may not exist during fds_env_close
    if (env->me_dbxs != nullptr)
    {
        for (i = env->me_maxdbs; --i >= CORE_DBS;)
            free(env->me_dbxs[i].md_name.mv_data);
        free(env->me_dbxs);
    }

    free(env->me_pbuf);
    free(env->me_dbiseqs);
    free(env->me_dbflags);
    free(env->me_path);
    free(env->me_dirty_list);
    free(env->me_txn0);
    fds_midl_free(env->me_free_pgs);

    if ((env->me_flags & FDS_ENV_TXKEY) != 0U)
    {
        pthread_key_delete(env->me_txkey);
#ifdef FDS_WINDOWS
        // Delete our key from the global list
        for (i = 0; i < fds_tls_nkeys; i++)
            if (fds_tls_keys[i] == env->me_txkey)
            {
                fds_tls_keys[i] = fds_tls_keys[fds_tls_nkeys - 1];
                fds_tls_nkeys--;
                break;
            }
#endif
    }

    if (env->me_map != nullptr)
    {
        munmap(env->me_map, env->me_mapsize);
    }
    if (env->me_mfd != INVALID_HANDLE_VALUE)
        (void)close(env->me_mfd);
#ifdef FDS_WINDOWS
    if (env->ovs > 0)
    {
        for (i = 0; i < env->ovs; i++)
        {
            CloseHandle(env->ov[i].hEvent);
        }
        free(env->ov);
    }
    if (env->me_ovfd != INVALID_HANDLE_VALUE)
        (void)close(env->me_ovfd);
#endif
    if (env->me_fd != INVALID_HANDLE_VALUE)
        (void)close(env->me_fd);
    if (env->me_txns != nullptr)
    {
        FDS_PID_T pid = getpid();
        // Clearing readers is done in this function because
        // me_txkey with its destructor must be disabled first.
        //
        // We skip the the reader mutex, so we touch only
        // data owned by this process (me_close_readers and
        // our readers), and clear each reader atomically.
        for (i = env->me_close_readers; --i >= 0;)
            if (env->me_txns->mti_readers[i].mr_pid == pid)
                env->me_txns->mti_readers[i].mr_pid = 0;
#ifdef FDS_WINDOWS
        if (env->me_rmutex != nullptr)
        {
            CloseHandle(env->me_rmutex);
            if (env->me_wmutex != nullptr)
                CloseHandle(env->me_wmutex);
        }
        // Windows automatically destroys the mutexes when
        // the last handle closes.
#elif defined(FDS_USE_POSIX_SEM)
        if (env->me_rmutex != SEM_FAILED)
        {
            sem_close(env->me_rmutex);
            if (env->me_wmutex != SEM_FAILED)
                sem_close(env->me_wmutex);
            // If we have the filelock:  If we are the
            // only remaining user, clean up semaphores.
            if (excl == 0)
                fds_env_excl_lock(env, &excl);
            if (excl > 0)
            {
                sem_unlink(MUTEXNAME(env, 'r'));
                sem_unlink(MUTEXNAME(env, 'w'));
            }
        }
#elif defined(FDS_USE_SYSV_SEM)
        if (env->me_rmutex->semid != -1)
        {
            // If we have the filelock:  If we are the
            // only remaining user, clean up semaphores.
            if (excl == 0)
                fds_env_excl_lock(env, &excl);
            if (excl > 0)
                semctl(env->me_rmutex->semid, 0, IPC_RMID);
        }
#endif
        munmap((void*)env->me_txns, (env->me_maxreaders - 1) * sizeof(FDS_reader) + sizeof(FDS_txninfo));
    }
    if (env->me_lfd != INVALID_HANDLE_VALUE)
    {
#ifdef FDS_WINDOWS
        if (excl >= 0)
        {
            // Unlock the lockfile.  Windows would have unlocked it
            // after closing anyway, but not necessarily at once.
            UnlockFile(env->me_lfd, 0, 0, 1, 0);
        }
#endif
        (void)close(env->me_lfd);
    }

    env->me_flags &= ~(FDS_ENV_ACTIVE | FDS_ENV_TXKEY);
}

void ESECT fds_env_close(FDS_env* env)
{
    FDS_page* dp;

    if (env == nullptr)
        return;

    DPRINTF(("%p", env));
    VGMEMP_DESTROY(env);
    while ((dp = env->me_dpages) != nullptr)
    {
        VGMEMP_DEFINED(&dp->mp_next, sizeof(dp->mp_next));
        env->me_dpages = dp->mp_next;
        free(dp);
    }

    fds_env_close0(env, 0);
    free(env);
}

#ifndef FDS_WBUF
#define FDS_WBUF (1024 * 1024)
#endif
enum
{
    FDS_EOF = 0x10  // fds_env_copyfd1() is done reading
};

// State needed for a double-buffering compacting copy.
struct fds_copy
{
    FDS_env* mc_env;
    FDS_txn* mc_txn;
    pthread_mutex_t mc_mutex;
    pthread_cond_t mc_cond;  // Condition variable for #mc_new
    char* mc_wbuf[2];
    char* mc_over[2];
    int mc_wlen[2];
    int mc_olen[2];
    pgno_t mc_next_pgno;
    HANDLE mc_fd;
    int mc_toggle;  // Buffer number in provider
    int mc_new;     // (0-2 buffers to write) | (FDS_EOF at end)
    // Error code.  Never cleared if set.  Both threads can set nonzero
    // to fail the copy.  Not mutex-protected, FiksDataStore expects atomic int.
    volatile int mc_error;
};

// Dedicated writer thread for compacting copy.
auto ESECT CALL_CONV fds_env_copythr(void* arg) -> THREAD_RET
{
    auto* my = (fds_copy*)arg;
    char* ptr;
    int toggle = 0;
    int wsize;
    int rc;
#ifdef FDS_WINDOWS
    DWORD len;
#define DO_WRITE(rc, fd, ptr, w2, len) (rc) = WriteFile((fd), (ptr), (w2), &(len), NULL)
#else
    int len;
#define DO_WRITE(rc, fd, ptr, w2, len)                                                                                 \
    len = write(fd, ptr, w2);                                                                                          \
    rc = (len >= 0)
#ifdef SIGPIPE
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    if ((rc = pthread_sigmask(SIG_BLOCK, &set, NULL)) != 0)
        my->mc_error = rc;
#endif
#endif

    pthread_mutex_lock(&my->mc_mutex);
    for (;;)
    {
        while (my->mc_new == 0)
            pthread_cond_wait(&my->mc_cond, &my->mc_mutex);
        if (my->mc_new == 0 + FDS_EOF)  // 0 buffers, just EOF
            break;
        wsize = my->mc_wlen[toggle];
        ptr = my->mc_wbuf[toggle];
    again:
        rc = FDS_SUCCESS;
        while (wsize > 0 && (my->mc_error == 0))
        {
            DO_WRITE(rc, my->mc_fd, ptr, wsize, len);
            if (rc == 0)
            {
                rc = ErrCode();
#if defined(SIGPIPE) && !defined(FDS_WINDOWS)
                if (rc == EPIPE)
                {
                    // Collect the pending SIGPIPE, otherwise at least OS X
                    // gives it to the process on thread-exit (ITS#8504).
                    int tmp;
                    sigwait(&set, &tmp);
                }
#endif
                break;
            }
            if (len > 0)
            {
                rc = FDS_SUCCESS;
                ptr += len;
                wsize -= len;
                continue;
            }

            rc = EIO;
            break;
        }
        if (rc != 0)
        {
            my->mc_error = rc;
        }
        // If there's an overflow page tail, write it too
        if (my->mc_olen[toggle] != 0)
        {
            wsize = my->mc_olen[toggle];
            ptr = my->mc_over[toggle];
            my->mc_olen[toggle] = 0;
            goto again;
        }
        my->mc_wlen[toggle] = 0;
        toggle ^= 1;
        // Return the empty buffer to provider
        my->mc_new--;
        pthread_cond_signal(&my->mc_cond);
    }
    pthread_mutex_unlock(&my->mc_mutex);
    return (THREAD_RET)0;
#undef DO_WRITE
}

// Give buffer and/or FDS_EOF to writer thread, await unused buffer.
//
// my control structure.
// adjust (1 to hand off 1 buffer) | (FDS_EOF when ending).
auto ESECT fds_env_cthr_toggle(fds_copy* my, int adjust) -> int
{
    pthread_mutex_lock(&my->mc_mutex);
    my->mc_new += adjust;
    pthread_cond_signal(&my->mc_cond);
    while ((my->mc_new & 2) != 0)  // both buffers in use
        pthread_cond_wait(&my->mc_cond, &my->mc_mutex);
    pthread_mutex_unlock(&my->mc_mutex);

    my->mc_toggle ^= (adjust & 1);
    // Both threads reset mc_wlen, to be safe from threading errors
    my->mc_wlen[my->mc_toggle] = 0;
    return my->mc_error;
}

// Depth-first tree traversal for compacting copy.
// my control structure.
// pg database root.
auto ESECT fds_env_cwalk(fds_copy* my, pgno_t* pg) -> int
{
    FDS_cursor mc;
    mc.mc_next = nullptr;
    FDS_node* ni;
    FDS_page* mo;
    FDS_page* mp;
    FDS_page* leaf;
    char* buf;
    char* ptr;
    int rc;
    int toggle;
    unsigned int i;

    // Empty DB, nothing to do
    if (*pg == P_INVALID)
        return FDS_SUCCESS;

    mc.mc_snum = 1;
    mc.mc_txn = my->mc_txn;
    mc.mc_flags = my->mc_txn->mt_flags & (C_ORIG_RDONLY | C_WRITEMAP);

    rc = fds_page_get(&mc, *pg, &mc.mc_pg[0], nullptr);
    if (rc != 0)
        return rc;
    rc = fds_page_search_root(&mc, nullptr, FDS_PS_FIRST);
    if (rc != 0)
        return rc;

    // Make cursor pages writable
    buf = ptr = (char*)malloc(static_cast<size_t>(my->mc_env->me_psize) * mc.mc_snum);
    if (buf == nullptr)
        return ENOMEM;

    for (i = 0; i < mc.mc_top; i++)
    {
        fds_page_copy((FDS_page*)ptr, mc.mc_pg[i], my->mc_env->me_psize);
        mc.mc_pg[i] = (FDS_page*)ptr;
        ptr += my->mc_env->me_psize;
    }

    // This is writable space for a leaf page. Usually not needed.
    leaf = (FDS_page*)ptr;

    toggle = my->mc_toggle;
    while (mc.mc_snum > 0)
    {
        unsigned n;
        mp = mc.mc_pg[mc.mc_top];
        n = NUMKEYS(mp);

        if (IS_LEAF(mp))
        {
            // No LEAF2 or duplicate support - simplified logic
            {
                for (i = 0; i < n; i++)
                {
                    ni = NODEPTR(mp, i);
                    if ((ni->mn_flags & F_BIGDATA) != 0)
                    {
                        FDS_page* omp;
                        pgno_t pg;

                        // Need writable leaf
                        if (mp != leaf)
                        {
                            mc.mc_pg[mc.mc_top] = leaf;
                            fds_page_copy(leaf, mp, my->mc_env->me_psize);
                            mp = leaf;
                            ni = NODEPTR(mp, i);
                        }

                        memcpy(&pg, NODEDATA(ni), sizeof(pg));
                        memcpy(NODEDATA(ni), &my->mc_next_pgno, sizeof(pgno_t));
                        rc = fds_page_get(&mc, pg, &omp, nullptr);
                        if (rc != 0)
                            goto done;
                        if (my->mc_wlen[toggle] >= FDS_WBUF)
                        {
                            rc = fds_env_cthr_toggle(my, 1);
                            if (rc != 0)
                                goto done;
                            toggle = my->mc_toggle;
                        }
                        mo = (FDS_page*)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
                        memcpy(mo, omp, my->mc_env->me_psize);
                        mo->mp_pgno = my->mc_next_pgno;
                        my->mc_next_pgno += omp->mp_pages;
                        my->mc_wlen[toggle] += my->mc_env->me_psize;
                        if (omp->mp_pages > 1)
                        {
                            my->mc_olen[toggle] = my->mc_env->me_psize * (omp->mp_pages - 1);
                            my->mc_over[toggle] = (char*)omp + my->mc_env->me_psize;
                            rc = fds_env_cthr_toggle(my, 1);
                            if (rc != 0)
                                goto done;
                            toggle = my->mc_toggle;
                        }
                    }
                    else if ((ni->mn_flags & F_SUBDATA) != 0)
                    {
                        FDS_db db;

                        // Need writable leaf
                        if (mp != leaf)
                        {
                            mc.mc_pg[mc.mc_top] = leaf;
                            fds_page_copy(leaf, mp, my->mc_env->me_psize);
                            mp = leaf;
                            ni = NODEPTR(mp, i);
                        }

                        memcpy(&db, NODEDATA(ni), sizeof(db));
                        my->mc_toggle = toggle;
                        rc = fds_env_cwalk(my, &db.md_root);
                        if (rc != 0)
                            goto done;
                        toggle = my->mc_toggle;
                        memcpy(NODEDATA(ni), &db, sizeof(db));
                    }
                }
            }
        }
        else
        {
            mc.mc_ki[mc.mc_top]++;
            if (mc.mc_ki[mc.mc_top] < n)
            {
                pgno_t pg;
            again:
                ni = NODEPTR(mp, mc.mc_ki[mc.mc_top]);
                pg = NODEPGNO(ni);
                rc = fds_page_get(&mc, pg, &mp, nullptr);
                if (rc != 0)
                    goto done;
                mc.mc_top++;
                mc.mc_snum++;
                mc.mc_ki[mc.mc_top] = 0;
                if (IS_BRANCH(mp))
                {
                    // Whenever we advance to a sibling branch page,
                    // we must proceed all the way down to its first leaf.
                    fds_page_copy(mc.mc_pg[mc.mc_top], mp, my->mc_env->me_psize);
                    goto again;
                }
                else
                    mc.mc_pg[mc.mc_top] = mp;
                continue;
            }
        }
        if (my->mc_wlen[toggle] >= FDS_WBUF)
        {
            rc = fds_env_cthr_toggle(my, 1);
            if (rc != 0)
                goto done;
            toggle = my->mc_toggle;
        }
        mo = (FDS_page*)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
        fds_page_copy(mo, mp, my->mc_env->me_psize);
        mo->mp_pgno = my->mc_next_pgno++;
        my->mc_wlen[toggle] += my->mc_env->me_psize;
        if (mc.mc_top != 0U)
        {
            // Update parent if there is one
            ni = NODEPTR(mc.mc_pg[mc.mc_top - 1], mc.mc_ki[mc.mc_top - 1]);
            SETPGNO(ni, mo->mp_pgno);
            fds_cursor_pop(&mc);
        }
        else
        {
            // Otherwise we're done
            *pg = mo->mp_pgno;
            break;
        }
    }
done:
    free(buf);
    return rc;
}

// Copy environment with compaction.
auto ESECT fds_env_copyfd1(FDS_env* env, HANDLE fd) -> int
{
    FDS_meta* mm;
    FDS_page* mp;
    fds_copy my;
    my.mc_env = nullptr;
    FDS_txn* txn = nullptr;
    pthread_t thr;
    pgno_t root;
    pgno_t new_root;
    int rc = FDS_SUCCESS;

#ifdef FDS_WINDOWS
    my.mc_mutex = CreateMutex(nullptr, FALSE, nullptr);
    my.mc_cond = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if ((my.mc_mutex == nullptr) || (my.mc_cond == nullptr))
    {
        rc = ErrCode();
        goto done;
    }
    my.mc_wbuf[0] = (char*)_aligned_malloc(static_cast<size_t>(FDS_WBUF) * 2, env->me_os_psize);
    if (my.mc_wbuf[0] == nullptr)
    {
        // _aligned_malloc() sets errno, but we use Windows error codes
        rc = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
#else
    if ((rc = pthread_mutex_init(&my.mc_mutex, NULL)) != 0)
        return rc;
    if ((rc = pthread_cond_init(&my.mc_cond, NULL)) != 0)
        goto done2;
#ifdef HAVE_MEMALIGN
    my.mc_wbuf[0] = memalign(env->me_os_psize, FDS_WBUF * 2);
    if (my.mc_wbuf[0] == NULL)
    {
        rc = errno;
        goto done;
    }
#else
    {
        void* p;
        if ((rc = posix_memalign(&p, env->me_os_psize, FDS_WBUF * 2)) != 0)
            goto done;
        my.mc_wbuf[0] = (char*)p;
    }
#endif
#endif
    memset(my.mc_wbuf[0], 0, static_cast<size_t>(FDS_WBUF) * 2);
    my.mc_wbuf[1] = my.mc_wbuf[0] + static_cast<ptrdiff_t>(FDS_WBUF);
    my.mc_next_pgno = NUM_METAS;
    my.mc_env = env;
    my.mc_fd = fd;
    rc = THREAD_CREATE(thr, fds_env_copythr, &my);
    if (rc != 0)
        goto done;

    rc = fds_txn_begin(env, nullptr, FDS_RDONLY, &txn);
    if (rc != 0)
        goto finish;

    mp = (FDS_page*)my.mc_wbuf[0];
    memset(mp, 0, static_cast<size_t>(NUM_METAS) * env->me_psize);
    mp->mp_pgno = 0;
    mp->mp_flags = P_META;
    mm = reinterpret_cast<FDS_meta*>(reinterpret_cast<char*>(mp) + PAGEHDRSZ);
    fds_env_init_meta0(env, mm);

    mp = (FDS_page*)(my.mc_wbuf[0] + env->me_psize);
    mp->mp_pgno = 1;
    mp->mp_flags = P_META;
    *reinterpret_cast<FDS_meta*>(reinterpret_cast<char*>(mp) + PAGEHDRSZ) = *mm;
    mm = reinterpret_cast<FDS_meta*>(reinterpret_cast<char*>(mp) + PAGEHDRSZ);

    // Set metapage 1 with current main DB
    root = new_root = txn->mt_dbs[MAIN_DBI].md_root;
    if (root != P_INVALID)
    {
        // Count free pages + freeDB pages.  Subtract from last_pg
        // to find the new last_pg, which also becomes the new root.
        FDS_ID freecount = 0;
        FDS_cursor mc;
        FDS_val key;
        FDS_val data;
        fds_cursor_init(&mc, txn, FREE_DBI);
        while ((rc = fds_cursor_get(&mc, &key, &data, FDS_NEXT)) == 0)
            freecount += *(FDS_ID*)data.mv_data;
        if (rc != FDS_NOTFOUND)
            goto finish;
        freecount += txn->mt_dbs[FREE_DBI].md_branch_pages + txn->mt_dbs[FREE_DBI].md_leaf_pages +
                     txn->mt_dbs[FREE_DBI].md_overflow_pages;

        new_root = txn->mt_next_pgno - 1 - freecount;
        mm->mm_last_pg = new_root;
        mm->mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
        mm->mm_dbs[MAIN_DBI].md_root = new_root;
    }
    else
    {
        // When the DB is empty, handle it specially to
        // fix any breakage like page leaks from ITS#8174.
        mm->mm_dbs[MAIN_DBI].md_flags = txn->mt_dbs[MAIN_DBI].md_flags;
    }
    if (root != P_INVALID || (mm->mm_dbs[MAIN_DBI].md_flags != 0U))
    {
        mm->mm_txnid = 1;  // use metapage 1
    }

    my.mc_wlen[0] = env->me_psize * NUM_METAS;
    my.mc_txn = txn;
    rc = fds_env_cwalk(&my, &root);
    if (rc == FDS_SUCCESS && root != new_root)
    {
        rc = FDS_INCOMPATIBLE;  // page leak or corrupt DB
    }

finish:
    if (rc != 0)
        my.mc_error = rc;
    fds_env_cthr_toggle(&my, 1 | FDS_EOF);
    rc = THREAD_FINISH(thr);
    fds_txn_abort_impl(txn);

done:
#ifdef FDS_WINDOWS
    if (my.mc_wbuf[0] != nullptr)
        _aligned_free(my.mc_wbuf[0]);
    if (my.mc_cond != nullptr)
        CloseHandle(my.mc_cond);
    if (my.mc_mutex != nullptr)
        CloseHandle(my.mc_mutex);
#else
    free(my.mc_wbuf[0]);
    pthread_cond_destroy(&my.mc_cond);
done2:
    pthread_mutex_destroy(&my.mc_mutex);
#endif
    return (rc != 0) ? rc : my.mc_error;
}

static auto ESECT fds_fsize(HANDLE fd, fds_size_t* size) -> int
{
#ifdef FDS_WINDOWS
    LARGE_INTEGER fsize;

    if (GetFileSizeEx(fd, &fsize) == 0)
        return ErrCode();

    *size = fsize.QuadPart;
#else
    struct stat st;

    if (fstat(fd, &st))
        return ErrCode();

    *size = st.st_size;
#endif
    return FDS_SUCCESS;
}

// Copy environment as-is.
auto ESECT fds_env_copyfd0(FDS_env* env, HANDLE fd) -> int
{
    FDS_txn* txn = nullptr;
    fds_mutexref_t wmutex = nullptr;
    int rc;
    fds_size_t wsize;
    fds_size_t w3;
    char* ptr;
#ifdef FDS_WINDOWS
    DWORD len;
    DWORD w2;
#define DO_WRITE(rc, fd, ptr, w2, len) (rc) = WriteFile((fd), (ptr), (w2), &(len), NULL)
#else
    ssize_t len;
    size_t w2;
#define DO_WRITE(rc, fd, ptr, w2, len)                                                                                 \
    len = write(fd, ptr, w2);                                                                                          \
    rc = (len >= 0)
#endif

    // Do the lock/unlock of the reader mutex before starting the
    // write txn.  Otherwise other read txns could block writers.
    rc = fds_txn_begin(env, nullptr, FDS_RDONLY, &txn);
    if (rc != 0)
        return rc;

    if (env->me_txns != nullptr)
    {
        // We must start the actual read txn after blocking writers
        fds_txn_end(txn, FDS_END_RESET_TMP);

        // Temporarily block writers until we snapshot the meta pages
        wmutex = env->me_wmutex;
        rc = LOCK_MUTEX0(wmutex);
        if ((rc != 0) && ((env->me_flags & FDS_FATAL_ERROR) != 0U))
            goto leave;

        rc = fds_txn_renew0(txn);
        if (rc != 0)
        {
            UNLOCK_MUTEX(wmutex);
            goto leave;
        }
    }

    wsize = static_cast<fds_size_t>(env->me_psize) * NUM_METAS;
    ptr = env->me_map;
    w2 = wsize;
    while (w2 > 0)
    {
        DO_WRITE(rc, fd, ptr, w2, len);
        if (rc == 0)
        {
            rc = ErrCode();
            break;
        }
        if (len > 0)
        {
            rc = FDS_SUCCESS;
            ptr += len;
            w2 -= len;
            continue;
        }

        // Non-blocking or async handles are not supported
        rc = EIO;
        break;
    }
    if (wmutex != nullptr)
        UNLOCK_MUTEX(wmutex);

    if (rc != 0)
        goto leave;

    w3 = txn->mt_next_pgno * env->me_psize;
    {
        fds_size_t fsize = 0;
        rc = fds_fsize(env->me_fd, &fsize);
        if (rc != 0)
            goto leave;
        if (w3 > fsize)
            w3 = fsize;
    }
    wsize = w3 - wsize;
    while (wsize > 0)
    {
        if (wsize > MAX_WRITE)
            w2 = MAX_WRITE;
        else
            w2 = wsize;
        DO_WRITE(rc, fd, ptr, w2, len);
        if (rc == 0)
        {
            rc = ErrCode();
            break;
        }
        if (len > 0)
        {
            rc = FDS_SUCCESS;
            ptr += len;
            wsize -= len;
            continue;
        }

        rc = EIO;
        break;
    }

leave:
    fds_txn_abort_impl(txn);
    return rc;
}

auto ESECT fds_env_copyfd2(FDS_env* env, HANDLE fd, unsigned int flags) -> int
{
    if ((flags & FDS_CP_COMPACT) != 0U)
        return fds_env_copyfd1(env, fd);

    return fds_env_copyfd0(env, fd);
}

auto ESECT fds_env_copyfd(FDS_env* env, HANDLE fd) -> int
{
    return fds_env_copyfd2(env, fd, 0);
}

auto ESECT fds_env_copy2(FDS_env* env, const char* path, unsigned int flags) -> int
{
    int rc;
    FDS_name fname;
    HANDLE newfd = INVALID_HANDLE_VALUE;

    rc = fds_fname_init(path, env->me_flags | FDS_NOLOCK, &fname);
    if (rc == FDS_SUCCESS)
    {
        rc = fds_fopen(env, &fname, FDS_O_COPY, 0666, &newfd);
        fds_fname_destroy(fname);
    }
    if (rc == FDS_SUCCESS)
    {
        rc = fds_env_copyfd2(env, newfd, flags);
        if (close(newfd) < 0 && rc == FDS_SUCCESS)
            rc = ErrCode();
    }
    return rc;
}

auto ESECT fds_env_copy(FDS_env* env, const char* path) -> int
{
    return fds_env_copy2(env, path, 0);
}

auto ESECT fds_env_set_flags(FDS_env* env, unsigned int flag, int onoff) -> int
{
    if ((flag & ~CHANGEABLE) != 0U)
        return EINVAL;
    if (onoff != 0)
        env->me_flags |= flag;
    else
        env->me_flags &= ~flag;
    return FDS_SUCCESS;
}

auto ESECT fds_env_get_flags(FDS_env* env, unsigned int* flags) -> int
{
    if ((env == nullptr) || (flags == nullptr))
        return EINVAL;

    *flags = env->me_flags & (CHANGEABLE | CHANGELESS);
    return FDS_SUCCESS;
}

auto ESECT fds_env_set_userctx(FDS_env* env, void* ctx) -> int
{
    if (env == nullptr)
        return EINVAL;
    env->me_userctx = ctx;
    return FDS_SUCCESS;
}

auto ESECT fds_env_get_userctx(FDS_env* env) -> void*
{
    return (env != nullptr) ? env->me_userctx : nullptr;
}

auto ESECT fds_env_set_assert(FDS_env* env, FDS_assert_func* func) -> int
{
    if (env == nullptr)
        return EINVAL;
#ifndef NDEBUG
    env->me_assert_func = func;
#endif
    return FDS_SUCCESS;
}

auto ESECT fds_env_get_path(FDS_env* env, const char** path) -> int
{
    if ((env == nullptr) || (path == nullptr))
        return EINVAL;

    *path = env->me_path;
    return FDS_SUCCESS;
}

auto ESECT fds_env_get_fd(FDS_env* env, fds_filehandle_t* fd) -> int
{
    if ((env == nullptr) || (fd == nullptr))
        return EINVAL;

    *fd = env->me_fd;
    return FDS_SUCCESS;
}

// Common code for #fds_stat() and #fds_env_stat().
// env the environment to operate in.
// db the #FDS_db record containing the stats to return.
// arg the address of an #FDS_stat structure to receive the stats.
// Return 0, this function always succeeds.
static auto ESECT fds_stat0(FDS_env* env, FDS_db* db, FDS_stat* arg) -> int
{
    arg->ms_psize = env->me_psize;
    arg->ms_depth = db->md_depth;
    arg->ms_branch_pages = db->md_branch_pages;
    arg->ms_leaf_pages = db->md_leaf_pages;
    arg->ms_overflow_pages = db->md_overflow_pages;
    arg->ms_entries = db->md_entries;

    return FDS_SUCCESS;
}

auto ESECT fds_stat(FDS_txn* txn, FDS_dbi dbi, FDS_stat* stat) -> int
{
    if (stat == nullptr || (TXN_DBI_EXIST(txn, dbi, DB_VALID) == 0))
        return EINVAL;

    if ((txn->mt_flags & FDS_TXN_BLOCKED) != 0U)
        return FDS_BAD_TXN;

    if ((txn->mt_dbflags[dbi] & DB_STALE) != 0)
    {
        FDS_cursor mc;
        // Stale, must read the DB's root. cursor_init does it for us.
        fds_cursor_init(&mc, txn, dbi);
    }
    return fds_stat0(txn->mt_env, &txn->mt_dbs[dbi], stat);
}

auto ESECT fds_env_stat(FDS_env* env, FDS_stat* stat) -> int
{
    FDS_meta* meta;

    if (env == nullptr || stat == nullptr)
        return EINVAL;

    meta = fds_env_pick_meta(env);

    return fds_stat0(env, &meta->mm_dbs[MAIN_DBI], stat);
}

auto ESECT fds_env_info(FDS_env* env, FDS_envinfo* stat) -> int
{
    FDS_meta* meta;

    if (env == nullptr || stat == nullptr)
        return EINVAL;

    meta = fds_env_pick_meta(env);
    stat->me_last_pgno = meta->mm_last_pg;
    stat->me_last_txnid = meta->mm_txnid;

    stat->me_mapsize = env->me_mapsize;
    stat->me_maxreaders = env->me_maxreaders;
    stat->me_numreaders = (env->me_txns != nullptr) ? env->me_txns->mti_numreaders : 0;
    return FDS_SUCCESS;
}

auto ESECT fds_env_get_maxkeysize(FDS_env* env) -> int
{
    return ENV_MAXKEY(env);
}

#include "mdb_env.h"

#include "mdb_compare.h"
#include "mdb_page.h"
#include "mdb_txn.h"
#include "mdb_lock.h"
#include "mdb_cursor.h"
#include "mdb_debug.h"
#include "mdb_hash.h"
#include "mdb_db.h"

	/**	@brief The maximum size of a database page.
	 *
	 *	It is 32k or 64k, since value-PAGEBASE must fit in
	 *	#MDB_page.%mp_upper.
	 *
	 *	LMDB will use database pages < OS pages if needed.
	 *	That causes more I/O in write transactions: The OS must
	 *	know (read) the whole page before writing a partial page.
	 *
	 *	Note that we don't currently support Huge pages. On Linux,
	 *	regular data files cannot use Huge pages, and in general
	 *	Huge pages aren't actually pageable. We rely on the OS
	 *	demand-pager to read our data and page it out when memory
	 *	pressure from other processes is high. So until OSs have
	 *	actual paging support for Huge pages, they're not viable.
	 */
#define MAX_PAGESIZE	 (PAGEBASE ? 0x10000 : 0x8000)

	/** The minimum number of keys required in a database page.
	 *	Setting this to a larger value will place a smaller bound on the
	 *	maximum size of a data item. Data items larger than this size will
	 *	be pushed into overflow pages instead of being stored directly in
	 *	the B-tree node. This value used to default to 4. With a page size
	 *	of 4096 bytes that meant that any item larger than 1024 bytes would
	 *	go into an overflow page. That also meant that on average 2-3KB of
	 *	each overflow page was wasted space. The value cannot be lower than
	 *	2 because then there would no longer be a tree structure. With this
	 *	value, items larger than 2KB will go into overflow pages, and on
	 *	average only 1KB will be wasted.
	 */
#define MDB_MINKEYS	 2

	/**	A stamp that identifies a file as an LMDB file.
	 *	There's nothing special about this value other than that it is easily
	 *	recognizable, and it will reflect any byte order mismatches.
	 */
#define MDB_MAGIC	 0xBEEFC0DE

	/**	The version number for a database's datafile format. */
#define MDB_DATA_VERSION	 1

static void mdb_env_reader_dest(void *ptr);

#ifdef _WIN32
typedef wchar_t	mdb_nchar_t;
# define MDB_NAME(str)	L##str
# define mdb_name_cpy	wcscpy
#else
/** Character type for file names: char on Unix, wchar_t on Windows */
typedef char	mdb_nchar_t;
# define MDB_NAME(str)	str		/**< #mdb_nchar_t[] string literal */
# define mdb_name_cpy	strcpy	/**< Copy name (#mdb_nchar_t string) */
#endif

#ifdef O_CLOEXEC /* POSIX.1-2008: Set FD_CLOEXEC atomically at open() */
# define MDB_CLOEXEC		O_CLOEXEC
#else
# define MDB_CLOEXEC		0
#endif

#ifdef _WIN32

/** Junk for arranging thread-specific callbacks on Windows. This is
 *	necessarily platform and compiler-specific. Windows supports up
 *	to 1088 keys. Let's assume nobody opens more than 64 environments
 *	in a single process, for now. They can override this if needed.
 */
#ifndef MAX_TLS_KEYS
#define MAX_TLS_KEYS	64
#endif

/** Junk for arranging thread-specific callbacks on Windows. This is
 *	necessarily platform and compiler-specific. Windows supports up
 *	to 1088 keys. Let's assume nobody opens more than 64 environments
 *	in a single process, for now. They can override this if needed.
 */
pthread_key_t mdb_tls_keys[MAX_TLS_KEYS];
int mdb_tls_nkeys;

void NTAPI mdb_tls_callback(PVOID module, DWORD reason, PVOID ptr)
{
	int i;
	switch(reason) {
	case DLL_PROCESS_ATTACH: break;
	case DLL_THREAD_ATTACH: break;
	case DLL_THREAD_DETACH:
		for (i=0; i<mdb_tls_nkeys; i++) {
			MDB_reader *r = (MDB_reader*) (pthread_getspecific(mdb_tls_keys[i]));
			if (r) {
				mdb_env_reader_dest(r);
			}
		}
		break;
	case DLL_PROCESS_DETACH: break;
	}
}

/* We use native NT APIs to setup the memory map, so that we can
 * let the DB file grow incrementally instead of always preallocating
 * the full size. These APIs are defined in <wdm.h> and <ntifs.h>
 * but those headers are meant for driver-level development and
 * conflict with the regular user-level headers, so we explicitly
 * declare them here. We get pointers to these functions from
 * NTDLL.DLL at runtime, to avoid buildtime dependencies on any
 * NTDLL import libraries.
 */
typedef NTSTATUS (WINAPI NtCreateSectionFunc)
  (OUT PHANDLE sh, IN ACCESS_MASK acc,
  IN void * oa OPTIONAL,
  IN PLARGE_INTEGER ms OPTIONAL,
  IN ULONG pp, IN ULONG aa, IN HANDLE fh OPTIONAL);

  typedef enum _SECTION_INHERIT {
	ViewShare = 1,
	ViewUnmap = 2
} SECTION_INHERIT;

typedef NTSTATUS (WINAPI NtMapViewOfSectionFunc)
  (IN HANDLE sh, IN HANDLE ph,
  IN OUT PVOID *addr, IN ULONG_PTR zbits,
  IN SIZE_T cs, IN OUT PLARGE_INTEGER off OPTIONAL,
  IN OUT PSIZE_T vs, IN SECTION_INHERIT ih,
  IN ULONG at, IN ULONG pp);

typedef NTSTATUS (WINAPI NtCloseFunc)(HANDLE h);

static int mdb_sec_inited;
static SECURITY_DESCRIPTOR mdb_null_sd;
static SECURITY_ATTRIBUTES mdb_all_sa;
static NtCloseFunc *NtClose;
static NtCreateSectionFunc *NtCreateSection;
static NtMapViewOfSectionFunc *NtMapViewOfSection;
#endif

#if defined(__FreeBSD__) && defined(__FreeBSD_version) && __FreeBSD_version >= 1100110
#elif defined(__APPLE__)
#define MDB_FDATASYNC(fd)		fcntl(fd, F_FULLFSYNC)
#elif defined (BSD) || defined(__FreeBSD_kernel__)
#define MDB_FDATASYNC		fsync
#endif

#ifdef _WIN32
#define	MDB_FDATASYNC(fd)	(!FlushFileBuffers(fd))
#define	MDB_MSYNC(addr,len,flags)	(!FlushViewOfFile(addr,len))
#endif

/** Function for flushing the data of a file. Define this to fsync
 *	if fdatasync() is not supported.
 */
#ifndef MDB_FDATASYNC
# define MDB_FDATASYNC	fdatasync
#endif

#ifndef _WIN32
/**	A flag for opening a file and requesting synchronous data writes.
 *	This is only used when writing a meta page. It's not strictly needed;
 *	we could just do a normal write and then immediately perform a flush.
 *	But if this flag is available it saves us an extra system call.
 *
 *	@note If O_DSYNC is undefined but exists in /usr/include,
 * preferably set some compiler flag to get the definition.
 */
#ifndef MDB_DSYNC
# ifdef O_DSYNC
# define MDB_DSYNC	O_DSYNC
# else
# define MDB_DSYNC	O_SYNC
# endif
#endif
#endif

#ifndef MDB_MSYNC
# define MDB_MSYNC(addr,len,flags)	msync(addr,len,flags)
#endif

#ifndef MS_SYNC
#define	MS_SYNC	1
#endif

#ifndef MS_ASYNC
#define	MS_ASYNC	0
#endif


/** Filename - string of #mdb_nchar_t[] */
struct MDB_name {
	int mn_len;					/**< Length  */
	int mn_alloced;				/**< True if #mn_val was malloced */
	mdb_nchar_t	*mn_val;		/**< Contents */
};

/** Filename suffixes [datafile,lockfile][without,with MDB_NOSUBDIR] */
static const mdb_nchar_t *const mdb_suffixes[2][2] = {
	{ MDB_NAME("/data.mdb"), MDB_NAME("")      },
	{ MDB_NAME("/lock.mdb"), MDB_NAME("-lock") }
};

#define MDB_SUFFLEN 9	/**< Max string length in #mdb_suffixes[] */

/** Destroy \b fname from #mdb_fname_init() */
#define mdb_fname_destroy(fname) \
	do { if ((fname).mn_alloced) free((fname).mn_val); } while (0)

#if defined(_WIN32)

/** Convert \b src to new wchar_t[] string with room for \b xtra extra chars */
static int ESECT utf8_to_utf16(const char *src, MDB_name *dst, int xtra)
{
	int rc, need = 0;
	wchar_t *result = NULL;
	for (;;) {					/* malloc result, then fill it in */
		need = MultiByteToWideChar(CP_UTF8, 0, src, -1, result, need);
		if (!need) {
			rc = ErrCode();
			free(result);
			return rc;
		}
		if (!result) {
			result = (wchar_t*) malloc(sizeof(wchar_t) * (need + xtra));
			if (!result)
				return ENOMEM;
			continue;
		}
		dst->mn_alloced = 1;
		dst->mn_len = need - 1;
		dst->mn_val = result;
		return MDB_SUCCESS;
	}
}
#endif /* defined(_WIN32) */

/** Set up filename + scratch area for filename suffix, for opening files.
 * It should be freed with #mdb_fname_destroy().
 * On Windows, paths are converted from char *UTF-8 to wchar_t *UTF-16.
 *
 * @param[in] path Pathname for #mdb_env_open().
 * @param[in] envflags Whether a subdir and/or lockfile will be used.
 * @param[out] fname Resulting filename, with room for a suffix if necessary.
 */
static int ESECT mdb_fname_init(const char *path, unsigned envflags, MDB_name *fname)
{
	int no_suffix = F_ISSET(envflags, MDB_NOSUBDIR|MDB_NOLOCK);
	fname->mn_alloced = 0;
#ifdef _WIN32
	return utf8_to_utf16(path, fname, no_suffix ? 0 : MDB_SUFFLEN);
#else
	fname->mn_len = strlen(path);
	if (no_suffix)
		fname->mn_val = (char *) path;
	else if ((fname->mn_val = (char *) malloc(fname->mn_len + MDB_SUFFLEN+1)) != NULL) {
		fname->mn_alloced = 1;
		strcpy(fname->mn_val, path);
	}
	else
		return ENOMEM;
	return MDB_SUCCESS;
#endif
}

/** File type, access mode etc. for #mdb_fopen() */
enum mdb_fopen_type {
#ifdef _WIN32
	MDB_O_RDONLY, MDB_O_RDWR, MDB_O_OVERLAPPED, MDB_O_META, MDB_O_COPY, MDB_O_LOCKS
#else
	/* A comment in mdb_fopen() explains some O_* flag choices. */
	MDB_O_RDONLY= O_RDONLY,                            /**< for RDONLY me_fd */
	MDB_O_RDWR  = O_RDWR  |O_CREAT,                    /**< for me_fd */
	MDB_O_META  = O_WRONLY|MDB_DSYNC     |MDB_CLOEXEC, /**< for me_mfd */
	MDB_O_COPY  = O_WRONLY|O_CREAT|O_EXCL|MDB_CLOEXEC, /**< for #mdb_env_copy() */
	/** Bitmask for open() flags in enum #mdb_fopen_type.  The other bits
	 * distinguish otherwise-equal MDB_O_* constants from each other.
	 */
	MDB_O_MASK  = MDB_O_RDWR|MDB_CLOEXEC | MDB_O_RDONLY|MDB_O_META|MDB_O_COPY,
	MDB_O_LOCKS = MDB_O_RDWR|MDB_CLOEXEC | ((MDB_O_MASK+1) & ~MDB_O_MASK) /**< for me_lfd */
#endif
};

/** Open an LMDB file.
 * @param[in] env	The LMDB environment.
 * @param[in,out] fname	Path from from #mdb_fname_init().  A suffix is
 * appended if necessary to create the filename, without changing mn_len.
 * @param[in] which	Determines file type, access mode, etc.
 * @param[in] mode	The Unix permissions for the file, if we create it.
 * @param[out] res	Resulting file handle.
 * @return 0 on success, non-zero on failure.
 */
static int ESECT mdb_fopen(const MDB_env *env, MDB_name *fname,
	enum mdb_fopen_type which, mdb_mode_t mode,
	HANDLE *res)
{
	int rc = MDB_SUCCESS;
	HANDLE fd;
#ifdef _WIN32
	DWORD acc, share, disp, attrs;
#else
	int flags;
#endif

	if (fname->mn_alloced)		/* modifiable copy */
		mdb_name_cpy(fname->mn_val + fname->mn_len,
			mdb_suffixes[which==MDB_O_LOCKS][F_ISSET(env->me_flags, MDB_NOSUBDIR)]);

	/* The directory must already exist.  Usually the file need not.
	 * MDB_O_META requires the file because we already created it using
	 * MDB_O_RDWR.  MDB_O_COPY must not overwrite an existing file.
	 *
	 * With MDB_O_COPY we do not want the OS to cache the writes, since
	 * the source data is already in the OS cache.
	 *
	 * The lockfile needs FD_CLOEXEC (close file descriptor on exec*())
	 * to avoid the flock() issues noted under Caveats in lmdb.h.
	 * Also set it for other filehandles which the user cannot get at
	 * and close himself, which he may need after fork().  I.e. all but
	 * me_fd, which programs do use via mdb_env_get_fd().
	 */

#ifdef _WIN32
	acc = GENERIC_READ|GENERIC_WRITE;
	share = FILE_SHARE_READ|FILE_SHARE_WRITE;
	disp = OPEN_ALWAYS;
	attrs = FILE_ATTRIBUTE_NORMAL;
	switch (which) {
	case MDB_O_OVERLAPPED: 	/* for unbuffered asynchronous writes (write-through mode)*/
		acc = GENERIC_WRITE;
		disp = OPEN_EXISTING;
		attrs = FILE_FLAG_OVERLAPPED|FILE_FLAG_WRITE_THROUGH;
		break;
	case MDB_O_RDONLY:			/* read-only datafile */
		acc = GENERIC_READ;
		disp = OPEN_EXISTING;
		break;
	case MDB_O_META:			/* for writing metapages */
		acc = GENERIC_WRITE;
		disp = OPEN_EXISTING;
		attrs = FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH;
		break;
	case MDB_O_COPY:			/* mdb_env_copy() & co */
		acc = GENERIC_WRITE;
		share = 0;
		disp = CREATE_NEW;
		attrs = FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH;
		break;
	default: break;	/* silence gcc -Wswitch (not all enum values handled) */
	}
	fd = CreateFileW(fname->mn_val, acc, share, NULL, disp, attrs, NULL);
#else
	fd = open(fname->mn_val, which & MDB_O_MASK, mode);
#endif

	if (fd == INVALID_HANDLE_VALUE)
		rc = ErrCode();
#ifndef _WIN32
	else {
		if (which != MDB_O_RDONLY && which != MDB_O_RDWR) {
			/* Set CLOEXEC if we could not pass it to open() */
			if (!MDB_CLOEXEC && (flags = fcntl(fd, F_GETFD)) != -1)
				(void) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
		}
		if (which == MDB_O_COPY && env->me_psize >= env->me_os_psize) {
			/* This may require buffer alignment.  There is no portable
			 * way to ask how much, so we require OS pagesize alignment.
			 */
# ifdef F_NOCACHE	/* __APPLE__ */
			(void) fcntl(fd, F_NOCACHE, 1);
# elif defined O_DIRECT
			/* open(...O_DIRECT...) would break on filesystems without
			 * O_DIRECT support (ITS#7682). Try to set it here instead.
			 */
			if ((flags = fcntl(fd, F_GETFL)) != -1)
				(void) fcntl(fd, F_SETFL, flags | O_DIRECT);
# endif
		}
	}
#endif	/* !_WIN32 */

	*res = fd;
	return rc;
}

int
mdb_env_sync0(MDB_env *env, int force, pgno_t numpgs)
{
	int rc = 0;
	if (env->me_flags & MDB_RDONLY)
		return EACCES;
	if (force
#ifndef _WIN32	/* Sync is normally achieved in Windows by doing WRITE_THROUGH writes */
		|| !(env->me_flags & MDB_NOSYNC)
#endif
		) {
		if (env->me_flags & MDB_WRITEMAP) {
			int flags = ((env->me_flags & MDB_MAPASYNC) && !force)
				? MS_ASYNC : MS_SYNC;
			if (MDB_MSYNC(env->me_map, env->me_psize * numpgs, flags))
				rc = ErrCode();
#if defined(_WIN32) || defined(__APPLE__)
			else if (flags == MS_SYNC && MDB_FDATASYNC(env->me_fd))
				rc = ErrCode();
#endif
		} else {
			if (MDB_FDATASYNC(env->me_fd))
				rc = ErrCode();
		}
	}
	return rc;
}

int
mdb_env_sync(MDB_env *env, int force)
{
	MDB_meta *m = mdb_env_pick_meta(env);
	return mdb_env_sync0(env, force, m->mm_last_pg+1);
}

/** Read the environment parameters of a DB environment before
 * mapping it into memory.
 * @param[in] env the environment handle
 * @param[in] prev whether to read the backup meta page
 * @param[out] meta address of where to store the meta information
 * @return 0 on success, non-zero on failure.
 */
int ESECT
mdb_env_read_header(MDB_env *env, int prev, MDB_meta *meta)
{
		/** Buffer for a stack-allocated meta page.
	 *	The members define size and alignment, and silence type
	 *	aliasing warnings.  They are not used directly; that could
	 *	mean incorrectly using several union members in parallel.
	 */
	union MDB_metabuf {
		MDB_page	mb_page;
		struct {
			char		mm_pad[PAGEHDRSZ];
			MDB_meta	mm_meta;
		} mb_metabuf;
	};

	MDB_metabuf	pbuf;
	MDB_page	*p;
	MDB_meta	*m;
	int			i, rc, off;
	enum { Size = sizeof(pbuf) };

	/* We don't know the page size yet, so use a minimum value.
	 * Read both meta pages so we can use the latest one.
	 */

	for (i=off=0; i<NUM_METAS; i++, off += meta->mm_psize) {
#ifdef _WIN32
		DWORD len;
		OVERLAPPED ov;
		memset(&ov, 0, sizeof(ov));
		ov.Offset = off;
		rc = ReadFile(env->me_fd, &pbuf, Size, &len, &ov) ? (int)len : -1;
		if (rc == -1 && ErrCode() == ERROR_HANDLE_EOF)
			rc = 0;
#else
		rc = pread(env->me_fd, &pbuf, Size, off);
#endif
		if (rc != Size) {
			if (rc == 0 && off == 0)
				return ENOENT;
			rc = rc < 0 ? (int) ErrCode() : MDB_INVALID;
			DPRINTF(("read: %s", mdb_strerror(rc)));
			return rc;
		}

		p = (MDB_page *)&pbuf;

		if (!F_ISSET(p->mp_flags, P_META)) {
			DPRINTF(("page %" Yu " not a meta page", p->mp_pgno));
			return MDB_INVALID;
		}

		m = (MDB_meta*) METADATA(p);
		if (m->mm_magic != MDB_MAGIC) {
			DPUTS("meta has invalid magic");
			return MDB_INVALID;
		}

		if (m->mm_version != MDB_DATA_VERSION) {
			DPRINTF(("database is version %u, expected version %u",
				m->mm_version, MDB_DATA_VERSION));
			return MDB_VERSION_MISMATCH;
		}

		if (off == 0 || (prev ? m->mm_txnid < meta->mm_txnid : m->mm_txnid > meta->mm_txnid))
			*meta = *m;
	}
	return 0;
}

/** Fill in most of the zeroed #MDB_meta for an empty database environment */
void ESECT
mdb_env_init_meta0(MDB_env *env, MDB_meta *meta)
{
	meta->mm_magic = MDB_MAGIC;
	meta->mm_version = MDB_DATA_VERSION;
	meta->mm_mapsize = env->me_mapsize;
	meta->mm_psize = env->me_psize;
	meta->mm_last_pg = NUM_METAS-1;
	meta->mm_flags = env->me_flags & 0xffff;
	meta->mm_flags |= MDB_INTEGERKEY; /* this is mm_dbs[FREE_DBI].md_flags */
	meta->mm_dbs[FREE_DBI].md_root = P_INVALID;
	meta->mm_dbs[MAIN_DBI].md_root = P_INVALID;
}

/** Write the environment parameters of a freshly created DB environment.
 * @param[in] env the environment handle
 * @param[in] meta the #MDB_meta to write
 * @return 0 on success, non-zero on failure.
 */
int ESECT
mdb_env_init_meta(MDB_env *env, MDB_meta *meta)
{
	MDB_page *p, *q;
	int rc;
	unsigned int	 psize;
#ifdef _WIN32
	DWORD len;
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
#define DO_PWRITE(rc, fd, ptr, size, len, pos)	do { \
	ov.Offset = pos;	\
	rc = WriteFile(fd, ptr, size, &len, &ov);	} while(0)
#else
	int len;
#define DO_PWRITE(rc, fd, ptr, size, len, pos)	do { \
	len = pwrite(fd, ptr, size, pos);	\
	if (len == -1 && ErrCode() == EINTR) continue; \
	rc = (len >= 0); break; } while(1)
#endif
	DPUTS("writing new meta page");

	psize = env->me_psize;

	p = (MDB_page*) calloc(NUM_METAS, psize);
	if (!p)
		return ENOMEM;
	p->mp_pgno = 0;
	p->mp_flags = P_META;
	*(MDB_meta *)METADATA(p) = *meta;

	q = (MDB_page *)((char *)p + psize);
	q->mp_pgno = 1;
	q->mp_flags = P_META;
	*(MDB_meta *)METADATA(q) = *meta;

	DO_PWRITE(rc, env->me_fd, p, psize * NUM_METAS, len, 0);
	if (!rc)
		rc = ErrCode();
	else if ((unsigned) len == psize * NUM_METAS)
		rc = MDB_SUCCESS;
	else
		rc = ENOSPC;
	free(p);
	return rc;
}

/** Update the environment info to commit a transaction.
 * @param[in] txn the transaction that's being committed
 * @return 0 on success, non-zero on failure.
 */
int
mdb_env_write_meta(MDB_txn *txn)
{
	MDB_env *env;
	MDB_meta	meta, metab, *mp;
	unsigned flags;
	mdb_size_t mapsize;
	MDB_OFF_T off;
	int rc, len, toggle;
	char *ptr;
	HANDLE mfd;
#ifdef _WIN32
	OVERLAPPED ov;
#else
	int r2;
#endif

	toggle = txn->mt_txnid & 1;
	DPRINTF(("writing meta page %d for root page %" Yu,
		toggle, txn->mt_dbs[MAIN_DBI].md_root));

	env = txn->mt_env;
	flags = txn->mt_flags | env->me_flags;
	mp = env->me_metas[toggle];
	mapsize = env->me_metas[toggle ^ 1]->mm_mapsize;
	/* Persist any increases of mapsize config */
	if (mapsize < env->me_mapsize)
		mapsize = env->me_mapsize;

#ifndef _WIN32 /* We don't want to ever use MSYNC/FlushViewOfFile in Windows */
	if (flags & MDB_WRITEMAP) {
		mp->mm_mapsize = mapsize;
		mp->mm_dbs[FREE_DBI] = txn->mt_dbs[FREE_DBI];
		mp->mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
		mp->mm_last_pg = txn->mt_next_pgno - 1;
#if (__GNUC__ * 100 + __GNUC_MINOR__ >= 404) && /* TODO: portability */	\
	!(defined(__i386__) || defined(__x86_64__))
		/* LY: issue a memory barrier, if not x86. ITS#7969 */
		__sync_synchronize();
#endif
		mp->mm_txnid = txn->mt_txnid;
		if (!(flags & (MDB_NOMETASYNC|MDB_NOSYNC))) {
			unsigned meta_size = env->me_psize;
			rc = (env->me_flags & MDB_MAPASYNC) ? MS_ASYNC : MS_SYNC;
			ptr = (char *)mp - PAGEHDRSZ;
			/* POSIX msync() requires ptr = start of OS page */
			r2 = (ptr - env->me_map) & (env->me_os_psize - 1);
			ptr -= r2;
			meta_size += r2;
			if (MDB_MSYNC(ptr, meta_size, rc)) {
				rc = ErrCode();
				goto fail;
			}
		}
		goto done;
	}
#endif
	metab.mm_txnid = mp->mm_txnid;
	metab.mm_last_pg = mp->mm_last_pg;

	meta.mm_mapsize = mapsize;
	meta.mm_dbs[FREE_DBI] = txn->mt_dbs[FREE_DBI];
	meta.mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
	meta.mm_last_pg = txn->mt_next_pgno - 1;
	meta.mm_txnid = txn->mt_txnid;

	off = offsetof(MDB_meta, mm_mapsize);
	ptr = (char *)&meta + off;
	len = sizeof(MDB_meta) - off;
	off += (char *)mp - env->me_map;

	/* Write to the SYNC fd unless MDB_NOSYNC/MDB_NOMETASYNC.
	 * (me_mfd goes to the same file as me_fd, but writing to it
	 * also syncs to disk.  Avoids a separate fdatasync() call.)
	 */
	mfd = (flags & (MDB_NOSYNC|MDB_NOMETASYNC)) ? env->me_fd : env->me_mfd;
#ifdef _WIN32
	{
		memset(&ov, 0, sizeof(ov));
		ov.Offset = off;
		if (!WriteFile(mfd, ptr, len, (DWORD *)&rc, &ov))
			rc = -1;
	}
#else
retry_write:
	rc = pwrite(mfd, ptr, len, off);
#endif
	if (rc != len) {
		rc = rc < 0 ? ErrCode() : EIO;
#ifndef _WIN32
		if (rc == EINTR)
			goto retry_write;
#endif
		DPUTS("write failed, disk error?");
		/* On a failure, the pagecache still contains the new data.
		 * Write some old data back, to prevent it from being used.
		 * Use the non-SYNC fd; we know it will fail anyway.
		 */
		meta.mm_last_pg = metab.mm_last_pg;
		meta.mm_txnid = metab.mm_txnid;
#ifdef _WIN32
		memset(&ov, 0, sizeof(ov));
		ov.Offset = off;
		WriteFile(env->me_fd, ptr, len, NULL, &ov);
#else
		r2 = pwrite(env->me_fd, ptr, len, off);
		(void)r2;	/* Silence warnings. We don't care about pwrite's return value */
#endif
fail:
		env->me_flags |= MDB_FATAL_ERROR;
		return rc;
	}
done:
	/* Memory ordering issues are irrelevant; since the entire writer
	 * is wrapped by wmutex, all of these changes will become visible
	 * after the wmutex is unlocked. Since the DB is multi-version,
	 * readers will get consistent data regardless of how fresh or
	 * how stale their view of these values is.
	 */
	if (env->me_txns)
		env->me_txns->mti_txnid = txn->mt_txnid;

	return MDB_SUCCESS;
}

/** Check both meta pages to see which one is newer.
 * @param[in] env the environment handle
 * @return newest #MDB_meta.
 */
MDB_meta *
mdb_env_pick_meta(const MDB_env *env)
{
	MDB_meta *const *metas = env->me_metas;
	return metas[ (metas[0]->mm_txnid < metas[1]->mm_txnid) ^
		((env->me_flags & MDB_PREVSNAPSHOT) != 0) ];
}

int ESECT
mdb_env_create(MDB_env **env)
{
	MDB_env *e;

	e = (MDB_env*) calloc(1, sizeof(MDB_env));
	if (!e)
		return ENOMEM;

	e->me_maxreaders = DEFAULT_READERS;
	e->me_maxdbs = e->me_numdbs = CORE_DBS;
	e->me_fd = INVALID_HANDLE_VALUE;
	e->me_lfd = INVALID_HANDLE_VALUE;
	e->me_mfd = INVALID_HANDLE_VALUE;
#ifdef MDB_USE_POSIX_SEM
	e->me_rmutex = SEM_FAILED;
	e->me_wmutex = SEM_FAILED;
#elif defined MDB_USE_SYSV_SEM
	e->me_rmutex->semid = -1;
	e->me_wmutex->semid = -1;
#endif
	e->me_pid = getpid();
	GET_PAGESIZE(e->me_os_psize);
	VGMEMP_CREATE(e,0,0);
	*env = e;
	MDB_TRACE(("%p", e));
	return MDB_SUCCESS;
}

#ifdef _WIN32
/** @brief Map a result from an NTAPI call to WIN32. */
static DWORD mdb_nt2win32(NTSTATUS st)
{
	OVERLAPPED o = {0};
	DWORD br;
	o.Internal = st;
	GetOverlappedResult(NULL, &o, &br, FALSE);
	return GetLastError();
}
#endif

int ESECT
mdb_env_map(MDB_env *env, void *addr)
{
	MDB_page *p;
	unsigned int flags = env->me_flags;
#ifdef _WIN32
	int rc;
	int access = SECTION_MAP_READ;
	HANDLE mh;
	void *map;
	SIZE_T msize;
	ULONG pageprot = PAGE_READONLY, secprot, alloctype;

	if (flags & MDB_WRITEMAP) {
		access |= SECTION_MAP_WRITE;
		pageprot = PAGE_READWRITE;
	}
	if (flags & MDB_RDONLY) {
		secprot = PAGE_READONLY;
		msize = 0;
		alloctype = 0;
	} else {
		secprot = PAGE_READWRITE;
		msize = env->me_mapsize;
		alloctype = MEM_RESERVE;
	}

	/** Some users are afraid of seeing their disk space getting used
	 * all at once, so the default is now to do incremental file growth.
	 * But that has a large performance impact, so give the option of
	 * allocating the file up front.
	 */
#ifdef MDB_FIXEDSIZE
	LARGE_INTEGER fsize;
	fsize.LowPart = msize & 0xffffffff;
	fsize.HighPart = msize >> 16 >> 16;
	rc = NtCreateSection(&mh, access, NULL, &fsize, secprot, SEC_RESERVE, env->me_fd);
#else
	rc = NtCreateSection(&mh, access, NULL, NULL, secprot, SEC_RESERVE, env->me_fd);
#endif
	if (rc)
		return mdb_nt2win32(rc);
	map = addr;
	rc = NtMapViewOfSection(mh, GetCurrentProcess(), &map, 0, 0, NULL, &msize, ViewUnmap, alloctype, pageprot);
	NtClose(mh);
	if (rc)
		return mdb_nt2win32(rc);
	env->me_map = (char*) map;
#else
	int mmap_flags = MAP_SHARED;
	int prot = PROT_READ;
#ifdef MAP_NOSYNC	/* Used on FreeBSD */
	if (flags & MDB_NOSYNC)
		mmap_flags |= MAP_NOSYNC;
#endif
	if (flags & MDB_WRITEMAP) {
		prot |= PROT_WRITE;
		if (ftruncate(env->me_fd, env->me_mapsize) < 0)
			return ErrCode();
	}
	env->me_map = (char*) mmap(addr, env->me_mapsize, prot, mmap_flags,
		env->me_fd, 0);
	if (env->me_map == MAP_FAILED) {
		env->me_map = NULL;
		return ErrCode();
	}

	if (flags & MDB_NORDAHEAD) {
		/* Turn off readahead. It's harmful when the DB is larger than RAM. */
#ifdef MADV_RANDOM
		madvise(env->me_map, env->me_mapsize, MADV_RANDOM);
#else
#ifdef POSIX_MADV_RANDOM
		posix_madvise(env->me_map, env->me_mapsize, POSIX_MADV_RANDOM);
#endif /* POSIX_MADV_RANDOM */
#endif /* MADV_RANDOM */
	}
#endif /* _WIN32 */

	/* Can happen because the address argument to mmap() is just a
	 * hint.  mmap() can pick another, e.g. if the range is in use.
	 * The MAP_FIXED flag would prevent that, but then mmap could
	 * instead unmap existing pages to make room for the new map.
	 */
	if (addr && env->me_map != addr)
		return EBUSY;	/* TODO: Make a new MDB_* error code? */

	p = (MDB_page *)env->me_map;
	env->me_metas[0] = (MDB_meta*)(METADATA(p));
	env->me_metas[1] = (MDB_meta*)((char *)env->me_metas[0] + env->me_psize);

	return MDB_SUCCESS;
}

int ESECT
mdb_env_set_mapsize(MDB_env *env, mdb_size_t size)
{
	/* If env is already open, caller is responsible for making
	 * sure there are no active txns.
	 */
	if (env->me_map) {
		MDB_meta *meta;
		void *old;
		int rc;

		if (env->me_txn)
			return EINVAL;
		meta = mdb_env_pick_meta(env);
		if (!size)
			size = meta->mm_mapsize;
		{
			/* Silently round up to minimum if the size is too small */
			mdb_size_t minsize = (meta->mm_last_pg + 1) * env->me_psize;
			if (size < minsize)
				size = minsize;
		}

		munmap(env->me_map, env->me_mapsize);
		env->me_mapsize = size;
		old = (env->me_flags & MDB_FIXEDMAP) ? env->me_map : NULL;
		rc = mdb_env_map(env, old);
		if (rc)
			return rc;
	}
	env->me_mapsize = size;
	if (env->me_psize)
		env->me_maxpg = env->me_mapsize / env->me_psize;
	MDB_TRACE(("%p, %" Yu "", env, size));
	return MDB_SUCCESS;
}

int ESECT
mdb_env_set_maxdbs(MDB_env *env, MDB_dbi dbs)
{
	if (env->me_map)
		return EINVAL;
	env->me_maxdbs = dbs + CORE_DBS;
	MDB_TRACE(("%p, %u", env, dbs));
	return MDB_SUCCESS;
}

int ESECT
mdb_env_set_maxreaders(MDB_env *env, unsigned int readers)
{
	if (env->me_map || readers < 1)
		return EINVAL;
	env->me_maxreaders = readers;
	MDB_TRACE(("%p, %u", env, readers));
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_maxreaders(MDB_env *env, unsigned int *readers)
{
	if (!env || !readers)
		return EINVAL;
	*readers = env->me_maxreaders;
	return MDB_SUCCESS;
}

/** Further setup required for opening an LMDB environment
 */
int ESECT
mdb_env_open2(MDB_env *env, int prev)
{
	unsigned int flags = env->me_flags;
	int i, newenv = 0, rc;
	MDB_meta meta;

#ifdef _WIN32
	/* See if we should use QueryLimited */
	rc = GetVersion();
	if ((rc & 0xff) > 5)
		env->me_pidquery = MDB_PROCESS_QUERY_LIMITED_INFORMATION;
	else
		env->me_pidquery = PROCESS_QUERY_INFORMATION;
	/* Grab functions we need from NTDLL */
	if (!NtCreateSection) {
		HMODULE h = GetModuleHandleW(L"NTDLL.DLL");
		if (!h)
			return MDB_PROBLEM;
		NtClose = (NtCloseFunc *)GetProcAddress(h, "NtClose");
		if (!NtClose)
			return MDB_PROBLEM;
		NtMapViewOfSection = (NtMapViewOfSectionFunc *)GetProcAddress(h, "NtMapViewOfSection");
		if (!NtMapViewOfSection)
			return MDB_PROBLEM;
		NtCreateSection = (NtCreateSectionFunc *)GetProcAddress(h, "NtCreateSection");
		if (!NtCreateSection)
			return MDB_PROBLEM;
	}
	env->ovs = 0;
#endif /* _WIN32 */

	if ((i = mdb_env_read_header(env, prev, &meta)) != 0) {
		if (i != ENOENT)
			return i;
		DPUTS("new mdbenv");
		newenv = 1;
		env->me_psize = env->me_os_psize;
		if (env->me_psize > MAX_PAGESIZE)
			env->me_psize = MAX_PAGESIZE;
		memset(&meta, 0, sizeof(meta));
		mdb_env_init_meta0(env, &meta);
		meta.mm_mapsize = DEFAULT_MAPSIZE;
	} else {
		env->me_psize = meta.mm_psize;
	}

	/* Was a mapsize configured? */
	if (!env->me_mapsize) {
		env->me_mapsize = meta.mm_mapsize;
	}
	{
		/* Make sure mapsize >= committed data size.  Even when using
		 * mm_mapsize, which could be broken in old files (ITS#7789).
		 */
		mdb_size_t minsize = (meta.mm_last_pg + 1) * meta.mm_psize;
		if (env->me_mapsize < minsize)
			env->me_mapsize = minsize;
	}
	meta.mm_mapsize = env->me_mapsize;

	if (newenv && !(flags & MDB_FIXEDMAP)) {
		/* mdb_env_map() may grow the datafile.  Write the metapages
		 * first, so the file will be valid if initialization fails.
		 * Except with FIXEDMAP, since we do not yet know mm_address.
		 * We could fill in mm_address later, but then a different
		 * program might end up doing that - one with a memory layout
		 * and map address which does not suit the main program.
		 */
		rc = mdb_env_init_meta(env, &meta);
		if (rc)
			return rc;
		newenv = 0;
	}
#ifdef _WIN32
	/* For FIXEDMAP, make sure the file is non-empty before we attempt to map it */
	if (newenv) {
		char dummy = 0;
		DWORD len;
		rc = WriteFile(env->me_fd, &dummy, 1, &len, NULL);
		if (!rc) {
			rc = ErrCode();
			return rc;
		}
	}
#endif

	rc = mdb_env_map(env, (flags & MDB_FIXEDMAP) ? meta.mm_address : NULL);
	if (rc)
		return rc;

	if (newenv) {
		if (flags & MDB_FIXEDMAP)
			meta.mm_address = env->me_map;
		i = mdb_env_init_meta(env, &meta);
		if (i != MDB_SUCCESS) {
			return i;
		}
	}

	env->me_maxfree_1pg = (env->me_psize - PAGEHDRSZ) / sizeof(pgno_t) - 1;
	env->me_nodemax = (((env->me_psize - PAGEHDRSZ) / MDB_MINKEYS) & -2)
		- sizeof(indx_t);
#if !(MDB_MAXKEYSIZE)
	env->me_maxkey = env->me_nodemax - (NODESIZE + sizeof(MDB_db));
#endif
	env->me_maxpg = env->me_mapsize / env->me_psize;

	if (prev && env->me_txns)
		env->me_txns->mti_txnid = meta.mm_txnid;

#if MDB_DEBUG
	{
		MDB_meta *meta = mdb_env_pick_meta(env);
		MDB_db *db = &meta->mm_dbs[MAIN_DBI];

		DPRINTF(("opened database version %u, pagesize %u",
			meta->mm_version, env->me_psize));
		DPRINTF(("using meta page %d",  (int) (meta->mm_txnid & 1)));
		DPRINTF(("depth: %u",           db->md_depth));
		DPRINTF(("entries: %" Yu,        db->md_entries));
		DPRINTF(("branch pages: %" Yu,   db->md_branch_pages));
		DPRINTF(("leaf pages: %" Yu,     db->md_leaf_pages));
		DPRINTF(("overflow pages: %" Yu, db->md_overflow_pages));
		DPRINTF(("root: %" Yu,           db->md_root));
	}
#endif

	return MDB_SUCCESS;
}


/** Release a reader thread's slot in the reader lock table.
 *	This function is called automatically when a thread exits.
 * @param[in] ptr This points to the slot in the reader lock table.
 */
static void mdb_env_reader_dest(void *ptr)
{
	MDB_reader *reader = (MDB_reader*) ptr;

#ifndef _WIN32
	if (reader->mr_pid == getpid()) /* catch pthread_exit() in child process */
#endif
		/* We omit the mutex, so do this atomically (i.e. skip mr_txnid) */
		reader->mr_pid = 0;
}

/** Downgrade the exclusive lock on the region back to shared */
int ESECT
mdb_env_share_locks(MDB_env *env, int *excl)
{
	int rc = 0;
	MDB_meta *meta = mdb_env_pick_meta(env);

	env->me_txns->mti_txnid = meta->mm_txnid;

#ifdef _WIN32
	{
		OVERLAPPED ov;
		/* First acquire a shared lock. The Unlock will
		 * then release the existing exclusive lock.
		 */
		memset(&ov, 0, sizeof(ov));
		if (!LockFileEx(env->me_lfd, 0, 0, 1, 0, &ov)) {
			rc = ErrCode();
		} else {
			UnlockFile(env->me_lfd, 0, 0, 1, 0);
			*excl = 0;
		}
	}
#else
	{
		struct flock lock_info;
		/* The shared lock replaces the existing lock */
		memset((void *)&lock_info, 0, sizeof(lock_info));
		lock_info.l_type = F_RDLCK;
		lock_info.l_whence = SEEK_SET;
		lock_info.l_start = 0;
		lock_info.l_len = 1;
		while ((rc = fcntl(env->me_lfd, F_SETLK, &lock_info)) &&
				(rc = ErrCode()) == EINTR) ;
		*excl = rc ? -1 : 0;	/* error may mean we lost the lock */
	}
#endif

	return rc;
}

/** Try to get exclusive lock, otherwise shared.
 *	Maintain *excl = -1: no/unknown lock, 0: shared, 1: exclusive.
 */
int ESECT
mdb_env_excl_lock(MDB_env *env, int *excl)
{
	int rc = 0;
#ifdef _WIN32
	if (LockFile(env->me_lfd, 0, 0, 1, 0)) {
		*excl = 1;
	} else {
		OVERLAPPED ov;
		memset(&ov, 0, sizeof(ov));
		if (LockFileEx(env->me_lfd, 0, 0, 1, 0, &ov)) {
			*excl = 0;
		} else {
			rc = ErrCode();
		}
	}
#else
	struct flock lock_info;
	memset((void *)&lock_info, 0, sizeof(lock_info));
	lock_info.l_type = F_WRLCK;
	lock_info.l_whence = SEEK_SET;
	lock_info.l_start = 0;
	lock_info.l_len = 1;
	while ((rc = fcntl(env->me_lfd, F_SETLK, &lock_info)) &&
			(rc = ErrCode()) == EINTR) ;
	if (!rc) {
		*excl = 1;
	} else
# ifndef MDB_USE_POSIX_MUTEX
	if (*excl < 0) /* always true when MDB_USE_POSIX_MUTEX */
# endif
	{
		lock_info.l_type = F_RDLCK;
		while ((rc = fcntl(env->me_lfd, F_SETLKW, &lock_info)) &&
				(rc = ErrCode()) == EINTR) ;
		if (rc == 0)
			*excl = 0;
	}
#endif
	return rc;
}

#if defined(_WIN32) || defined(MDB_USE_POSIX_SEM)

/** Init #MDB_env.me_mutexname[] except the char which #MUTEXNAME() will set.
 *	Changes to this code must be reflected in #MDB_LOCK_FORMAT.
 */
void ESECT
mdb_env_mname_init(MDB_env *env)
{
	char *nm = env->me_mutexname;
	strcpy(nm, MUTEXNAME_PREFIX);
	mdb_pack85(env->me_txns->mti_mutexid, nm + sizeof(MUTEXNAME_PREFIX));
}

/** Return env->me_mutexname after filling in ch ('r'/'w') for convenience */
#define MUTEXNAME(env, ch) ( \
		(void) ((env)->me_mutexname[sizeof(MUTEXNAME_PREFIX)-1] = (ch)), \
		(env)->me_mutexname)

#endif

/** Open and/or initialize the lock region for the environment.
 * @param[in] env The LMDB environment.
 * @param[in] fname Filename + scratch area, from #mdb_fname_init().
 * @param[in] mode The Unix permissions for the file, if we create it.
 * @param[in,out] excl In -1, out lock type: -1 none, 0 shared, 1 exclusive
 * @return 0 on success, non-zero on failure.
 */
int ESECT
mdb_env_setup_locks(MDB_env *env, MDB_name *fname, int mode, int *excl)
{
#ifdef _WIN32
#	define MDB_ERRCODE_ROFS	ERROR_WRITE_PROTECT
#else
#	define MDB_ERRCODE_ROFS	EROFS
#endif
#ifdef MDB_USE_SYSV_SEM
	int semid;
	union semun semu;
#endif
	int rc;
	MDB_OFF_T size, rsize;

	rc = mdb_fopen(env, fname, MDB_O_LOCKS, mode, &env->me_lfd);
	if (rc) {
		/* Omit lockfile if read-only env on read-only filesystem */
		if (rc == MDB_ERRCODE_ROFS && (env->me_flags & MDB_RDONLY)) {
			return MDB_SUCCESS;
		}
		goto fail;
	}

	if (!(env->me_flags & MDB_NOTLS)) {
		rc = pthread_key_create(&env->me_txkey, mdb_env_reader_dest);
		if (rc)
			goto fail;
		env->me_flags |= MDB_ENV_TXKEY;
#ifdef _WIN32
		/* Windows TLS callbacks need help finding their TLS info. */
		if (mdb_tls_nkeys >= MAX_TLS_KEYS) {
			rc = MDB_TLS_FULL;
			goto fail;
		}
		mdb_tls_keys[mdb_tls_nkeys++] = env->me_txkey;
#endif
	}

	/* Try to get exclusive lock. If we succeed, then
	 * nobody is using the lock region and we should initialize it.
	 */
	if ((rc = mdb_env_excl_lock(env, excl))) goto fail;

#ifdef _WIN32
	size = GetFileSize(env->me_lfd, NULL);
#else
	size = lseek(env->me_lfd, 0, SEEK_END);
	if (size == -1) goto fail_errno;
#endif
	rsize = (env->me_maxreaders-1) * sizeof(MDB_reader) + sizeof(MDB_txninfo);
	if (size < rsize && *excl > 0) {
#ifdef _WIN32
		if (SetFilePointer(env->me_lfd, rsize, NULL, FILE_BEGIN) != (DWORD)rsize
			|| !SetEndOfFile(env->me_lfd))
			goto fail_errno;
#else
		if (ftruncate(env->me_lfd, rsize) != 0) goto fail_errno;
#endif
	} else {
		rsize = size;
		size = rsize - sizeof(MDB_txninfo);
		env->me_maxreaders = size/sizeof(MDB_reader) + 1;
	}
	{
#ifdef _WIN32
		HANDLE mh;
		mh = CreateFileMapping(env->me_lfd, NULL, PAGE_READWRITE,
			0, 0, NULL);
		if (!mh) goto fail_errno;
		env->me_txns = (MDB_txninfo*) MapViewOfFileEx(mh, FILE_MAP_WRITE, 0, 0, rsize, NULL);
		CloseHandle(mh);
		if (!env->me_txns) goto fail_errno;
#else
		void *m = mmap(NULL, rsize, PROT_READ|PROT_WRITE, MAP_SHARED,
			env->me_lfd, 0);
		if (m == MAP_FAILED) goto fail_errno;
		env->me_txns = (MDB_txninfo*)m;
#endif
	}
	if (*excl > 0) {
#ifdef _WIN32
		BY_HANDLE_FILE_INFORMATION stbuf;
		struct {
			DWORD volume;
			DWORD nhigh;
			DWORD nlow;
		} idbuf;

		if (!mdb_sec_inited) {
			InitializeSecurityDescriptor(&mdb_null_sd,
				SECURITY_DESCRIPTOR_REVISION);
			SetSecurityDescriptorDacl(&mdb_null_sd, TRUE, 0, FALSE);
			mdb_all_sa.nLength = sizeof(SECURITY_ATTRIBUTES);
			mdb_all_sa.bInheritHandle = FALSE;
			mdb_all_sa.lpSecurityDescriptor = &mdb_null_sd;
			mdb_sec_inited = 1;
		}
		if (!GetFileInformationByHandle(env->me_lfd, &stbuf)) goto fail_errno;
		idbuf.volume = stbuf.dwVolumeSerialNumber;
		idbuf.nhigh  = stbuf.nFileIndexHigh;
		idbuf.nlow   = stbuf.nFileIndexLow;
		env->me_txns->mti_mutexid = mdb_hash(&idbuf, sizeof(idbuf));
		mdb_env_mname_init(env);
		env->me_rmutex = CreateMutexA(&mdb_all_sa, FALSE, MUTEXNAME(env, 'r'));
		if (!env->me_rmutex) goto fail_errno;
		env->me_wmutex = CreateMutexA(&mdb_all_sa, FALSE, MUTEXNAME(env, 'w'));
		if (!env->me_wmutex) goto fail_errno;
#elif defined(MDB_USE_POSIX_SEM)
		struct stat stbuf;
		struct {
			dev_t dev;
			ino_t ino;
		} idbuf;

#if defined(__NetBSD__)
#define	MDB_SHORT_SEMNAMES	1	/* limited to 14 chars */
#endif
		if (fstat(env->me_lfd, &stbuf)) goto fail_errno;
		memset(&idbuf, 0, sizeof(idbuf));
		idbuf.dev = stbuf.st_dev;
		idbuf.ino = stbuf.st_ino;
		env->me_txns->mti_mutexid = mdb_hash(&idbuf, sizeof(idbuf))
#ifdef MDB_SHORT_SEMNAMES
			/* Max 9 base85-digits.  We truncate here instead of in
			 * mdb_env_mname_init() to keep the latter portable.
			 */
			% ((mdb_hash_t)85*85*85*85*85*85*85*85*85)
#endif
			;
		mdb_env_mname_init(env);
		/* Clean up after a previous run, if needed:  Try to
		 * remove both semaphores before doing anything else.
		 */
		sem_unlink(MUTEXNAME(env, 'r'));
		sem_unlink(MUTEXNAME(env, 'w'));
		env->me_rmutex = sem_open(MUTEXNAME(env, 'r'), O_CREAT|O_EXCL, mode, 1);
		if (env->me_rmutex == SEM_FAILED) goto fail_errno;
		env->me_wmutex = sem_open(MUTEXNAME(env, 'w'), O_CREAT|O_EXCL, mode, 1);
		if (env->me_wmutex == SEM_FAILED) goto fail_errno;
#elif defined(MDB_USE_SYSV_SEM)
		unsigned short vals[2] = {1, 1};
		key_t key = ftok(fname->mn_val, 'M'); /* fname is lockfile path now */
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
#else	/* MDB_USE_POSIX_MUTEX: */
		pthread_mutexattr_t mattr;

		/* Solaris needs this before initing a robust mutex.  Otherwise
		 * it may skip the init and return EBUSY "seems someone already
		 * inited" or EINVAL "it was inited differently".
		 */
		memset(env->me_txns->mti_rmutex, 0, sizeof(*env->me_txns->mti_rmutex));
		memset(env->me_txns->mti_wmutex, 0, sizeof(*env->me_txns->mti_wmutex));

		if ((rc = pthread_mutexattr_init(&mattr)) != 0)
			goto fail;
		rc = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
		if (!rc) rc = pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
		if (!rc) rc = pthread_mutex_init(env->me_txns->mti_rmutex, &mattr);
		if (!rc) rc = pthread_mutex_init(env->me_txns->mti_wmutex, &mattr);
		pthread_mutexattr_destroy(&mattr);
		if (rc)
			goto fail;
#endif	/* _WIN32 || ... */

		env->me_txns->mti_magic = MDB_MAGIC;
		env->me_txns->mti_format = MDB_LOCK_FORMAT;
		env->me_txns->mti_txnid = 0;
		env->me_txns->mti_numreaders = 0;

	} else {
#ifdef MDB_USE_SYSV_SEM
		struct semid_ds buf;
#endif
		if (env->me_txns->mti_magic != MDB_MAGIC) {
			DPUTS("lock region has invalid magic");
			rc = MDB_INVALID;
			goto fail;
		}
		if (env->me_txns->mti_format != MDB_LOCK_FORMAT) {
			DPRINTF(("lock region has format+version 0x%x, expected 0x%x",
				env->me_txns->mti_format, MDB_LOCK_FORMAT));
			rc = MDB_VERSION_MISMATCH;
			goto fail;
		}
		rc = ErrCode();
		if (rc && rc != EACCES && rc != EAGAIN) {
			goto fail;
		}
#ifdef _WIN32
		mdb_env_mname_init(env);
		env->me_rmutex = OpenMutexA(SYNCHRONIZE, FALSE, MUTEXNAME(env, 'r'));
		if (!env->me_rmutex) goto fail_errno;
		env->me_wmutex = OpenMutexA(SYNCHRONIZE, FALSE, MUTEXNAME(env, 'w'));
		if (!env->me_wmutex) goto fail_errno;
#elif defined(MDB_USE_POSIX_SEM)
		mdb_env_mname_init(env);
		env->me_rmutex = sem_open(MUTEXNAME(env, 'r'), 0);
		if (env->me_rmutex == SEM_FAILED) goto fail_errno;
		env->me_wmutex = sem_open(MUTEXNAME(env, 'w'), 0);
		if (env->me_wmutex == SEM_FAILED) goto fail_errno;
#elif defined(MDB_USE_SYSV_SEM)
		semid = env->me_txns->mti_semid;
		semu.buf = &buf;
		/* check for read access */
		if (semctl(semid, 0, IPC_STAT, semu) < 0)
			goto fail_errno;
		/* check for write access */
		if (semctl(semid, 0, IPC_SET, semu) < 0)
			goto fail_errno;
#endif
	}
#ifdef MDB_USE_SYSV_SEM
	env->me_rmutex->semid = semid;
	env->me_wmutex->semid = semid;
	env->me_rmutex->semnum = 0;
	env->me_wmutex->semnum = 1;
	env->me_rmutex->locked = &env->me_txns->mti_rlocked;
	env->me_wmutex->locked = &env->me_txns->mti_wlocked;
#endif

	return MDB_SUCCESS;

fail_errno:
	rc = ErrCode();
fail:
	return rc;
}

	/** Only a subset of the @ref mdb_env flags can be changed
	 *	at runtime. Changing other flags requires closing the
	 *	environment and re-opening it with the new flags.
	 */
#define	CHANGEABLE	(MDB_NOSYNC|MDB_NOMETASYNC|MDB_MAPASYNC|MDB_NOMEMINIT)
#define	CHANGELESS	(MDB_FIXEDMAP|MDB_NOSUBDIR|MDB_RDONLY| \
	MDB_WRITEMAP|MDB_NOTLS|MDB_NOLOCK|MDB_NORDAHEAD|MDB_PREVSNAPSHOT)

#if VALID_FLAGS & PERSISTENT_FLAGS & (CHANGEABLE|CHANGELESS)
# error "Persistent DB flags & env flags overlap, but both go in mm_flags"
#endif

int ESECT
mdb_env_open(MDB_env *env, const char *path, unsigned int flags, mdb_mode_t mode)
{
	int rc, excl = -1;
	MDB_name fname;

	if (env->me_fd!=INVALID_HANDLE_VALUE || (flags & ~(CHANGEABLE|CHANGELESS)))
		return EINVAL;

	flags |= env->me_flags;

	rc = mdb_fname_init(path, flags, &fname);
	if (rc)
		return rc;

	flags |= MDB_ENV_ACTIVE;	/* tell mdb_env_close0() to clean up */

	if (flags & MDB_RDONLY) {
		/* silently ignore WRITEMAP when we're only getting read access */
		flags &= ~MDB_WRITEMAP;
	} else {
		if (!((env->me_free_pgs = mdb_midl_alloc(MDB_IDL_UM_MAX)) &&
			  (env->me_dirty_list = (MDB_ID2L)calloc(MDB_IDL_UM_SIZE, sizeof(MDB_ID2)))))
			rc = ENOMEM;
	}

	env->me_flags = flags;
	if (rc)
		goto leave;

	env->me_path = mdb_strdup(path);
	env->me_dbxs = (MDB_dbx*)calloc(env->me_maxdbs, sizeof(MDB_dbx));
	env->me_dbflags = (uint16_t*)calloc(env->me_maxdbs, sizeof(uint16_t));
	env->me_dbiseqs = (unsigned int*)calloc(env->me_maxdbs, sizeof(unsigned int));
	if (!(env->me_dbxs && env->me_path && env->me_dbflags && env->me_dbiseqs)) {
		rc = ENOMEM;
		goto leave;
	}
	env->me_dbxs[FREE_DBI].md_cmp = mdb_cmp_long; /* aligned MDB_INTEGERKEY */

	/* For RDONLY, get lockfile after we know datafile exists */
	if (!(flags & (MDB_RDONLY|MDB_NOLOCK))) {
		rc = mdb_env_setup_locks(env, &fname, mode, &excl);
		if (rc)
			goto leave;
		if ((flags & MDB_PREVSNAPSHOT) && !excl) {
			rc = EAGAIN;
			goto leave;
		}
	}

	rc = mdb_fopen(env, &fname,
		(flags & MDB_RDONLY) ? MDB_O_RDONLY : MDB_O_RDWR,
		mode, &env->me_fd);
	if (rc)
		goto leave;
#ifdef _WIN32
	rc = mdb_fopen(env, &fname, MDB_O_OVERLAPPED, mode, &env->me_ovfd);
	if (rc)
		goto leave;
#endif

	if ((flags & (MDB_RDONLY|MDB_NOLOCK)) == MDB_RDONLY) {
		rc = mdb_env_setup_locks(env, &fname, mode, &excl);
		if (rc)
			goto leave;
	}

	if ((rc = mdb_env_open2(env, flags & MDB_PREVSNAPSHOT)) == MDB_SUCCESS) {
		/* Synchronous fd for meta writes. Needed even with
		 * MDB_NOSYNC/MDB_NOMETASYNC, in case these get reset.
		 */
		if (!(flags & (MDB_RDONLY|MDB_WRITEMAP))) {
			rc = mdb_fopen(env, &fname, MDB_O_META, mode, &env->me_mfd);
			if (rc)
				goto leave;
		}
		DPRINTF(("opened dbenv %p", (void *) env));
		if (excl > 0 && !(flags & MDB_PREVSNAPSHOT)) {
			rc = mdb_env_share_locks(env, &excl);
			if (rc)
				goto leave;
		}
		if (!(flags & MDB_RDONLY)) {
			MDB_txn *txn;
			int tsize = sizeof(MDB_txn), size = tsize + env->me_maxdbs *
				(sizeof(MDB_db)+sizeof(MDB_cursor *)+sizeof(unsigned int)+1);
			if ((env->me_pbuf = calloc(1, env->me_psize)) &&
				(txn = (MDB_txn*)calloc(1, size)))
			{
				txn->mt_dbs = (MDB_db *)((char *)txn + tsize);
				txn->mt_cursors = (MDB_cursor **)(txn->mt_dbs + env->me_maxdbs);
				txn->mt_dbiseqs = (unsigned int *)(txn->mt_cursors + env->me_maxdbs);
				txn->mt_dbflags = (unsigned char *)(txn->mt_dbiseqs + env->me_maxdbs);
				txn->mt_env = env;
				txn->mt_dbxs = env->me_dbxs;
				txn->mt_flags = MDB_TXN_FINISHED;
				env->me_txn0 = txn;
			} else {
				rc = ENOMEM;
			}
		}
	}

leave:
	MDB_TRACE(("%p, %s, %u, %04o", env, path, flags & (CHANGEABLE|CHANGELESS), mode));
	if (rc) {
		mdb_env_close0(env, excl);
	}
	mdb_fname_destroy(fname);
	return rc;
}

/** Destroy resources from mdb_env_open(), clear our readers & DBIs */
void ESECT
mdb_env_close0(MDB_env *env, int excl)
{
	int i;

	if (!(env->me_flags & MDB_ENV_ACTIVE))
		return;

	/* Doing this here since me_dbxs may not exist during mdb_env_close */
	if (env->me_dbxs) {
		for (i = env->me_maxdbs; --i >= CORE_DBS; )
			free(env->me_dbxs[i].md_name.mv_data);
		free(env->me_dbxs);
	}

	free(env->me_pbuf);
	free(env->me_dbiseqs);
	free(env->me_dbflags);
	free(env->me_path);
	free(env->me_dirty_list);
	free(env->me_txn0);
	mdb_midl_free(env->me_free_pgs);

	if (env->me_flags & MDB_ENV_TXKEY) {
		pthread_key_delete(env->me_txkey);
#ifdef _WIN32
		/* Delete our key from the global list */
		for (i=0; i<mdb_tls_nkeys; i++)
			if (mdb_tls_keys[i] == env->me_txkey) {
				mdb_tls_keys[i] = mdb_tls_keys[mdb_tls_nkeys-1];
				mdb_tls_nkeys--;
				break;
			}
#endif
	}

	if (env->me_map) {
		munmap(env->me_map, env->me_mapsize);
	}
	if (env->me_mfd != INVALID_HANDLE_VALUE)
		(void) close(env->me_mfd);
#ifdef _WIN32
	if (env->ovs > 0) {
		for (i = 0; i < env->ovs; i++) {
			CloseHandle(env->ov[i].hEvent);
		}
		free(env->ov);
	}
	if (env->me_ovfd != INVALID_HANDLE_VALUE)
		(void) close(env->me_ovfd);
#endif
	if (env->me_fd != INVALID_HANDLE_VALUE)
		(void) close(env->me_fd);
	if (env->me_txns) {
		MDB_PID_T pid = getpid();
		/* Clearing readers is done in this function because
		 * me_txkey with its destructor must be disabled first.
		 *
		 * We skip the the reader mutex, so we touch only
		 * data owned by this process (me_close_readers and
		 * our readers), and clear each reader atomically.
		 */
		for (i = env->me_close_readers; --i >= 0; )
			if (env->me_txns->mti_readers[i].mr_pid == pid)
				env->me_txns->mti_readers[i].mr_pid = 0;
#ifdef _WIN32
		if (env->me_rmutex) {
			CloseHandle(env->me_rmutex);
			if (env->me_wmutex) CloseHandle(env->me_wmutex);
		}
		/* Windows automatically destroys the mutexes when
		 * the last handle closes.
		 */
#elif defined(MDB_USE_POSIX_SEM)
		if (env->me_rmutex != SEM_FAILED) {
			sem_close(env->me_rmutex);
			if (env->me_wmutex != SEM_FAILED)
				sem_close(env->me_wmutex);
			/* If we have the filelock:  If we are the
			 * only remaining user, clean up semaphores.
			 */
			if (excl == 0)
				mdb_env_excl_lock(env, &excl);
			if (excl > 0) {
				sem_unlink(MUTEXNAME(env, 'r'));
				sem_unlink(MUTEXNAME(env, 'w'));
			}
		}
#elif defined(MDB_USE_SYSV_SEM)
		if (env->me_rmutex->semid != -1) {
			/* If we have the filelock:  If we are the
			 * only remaining user, clean up semaphores.
			 */
			if (excl == 0)
				mdb_env_excl_lock(env, &excl);
			if (excl > 0)
				semctl(env->me_rmutex->semid, 0, IPC_RMID);
		}
#endif
		munmap((void *)env->me_txns, (env->me_maxreaders-1)*sizeof(MDB_reader)+sizeof(MDB_txninfo));
	}
	if (env->me_lfd != INVALID_HANDLE_VALUE) {
#ifdef _WIN32
		if (excl >= 0) {
			/* Unlock the lockfile.  Windows would have unlocked it
			 * after closing anyway, but not necessarily at once.
			 */
			UnlockFile(env->me_lfd, 0, 0, 1, 0);
		}
#endif
		(void) close(env->me_lfd);
	}

	env->me_flags &= ~(MDB_ENV_ACTIVE|MDB_ENV_TXKEY);
}

void ESECT
mdb_env_close(MDB_env *env)
{
	MDB_page *dp;

	if (env == NULL)
		return;

	MDB_TRACE(("%p", env));
	VGMEMP_DESTROY(env);
	while ((dp = env->me_dpages) != NULL) {
		VGMEMP_DEFINED(&dp->mp_next, sizeof(dp->mp_next));
		env->me_dpages = dp->mp_next;
		free(dp);
	}

	mdb_env_close0(env, 0);
	free(env);
}

#ifndef MDB_WBUF
#define MDB_WBUF	(1024*1024)
#endif
#define MDB_EOF		0x10	/**< #mdb_env_copyfd1() is done reading */

	/** State needed for a double-buffering compacting copy. */
struct mdb_copy {
	MDB_env *mc_env;
	MDB_txn *mc_txn;
	pthread_mutex_t mc_mutex;
	pthread_cond_t mc_cond;	/**< Condition variable for #mc_new */
	char *mc_wbuf[2];
	char *mc_over[2];
	int mc_wlen[2];
	int mc_olen[2];
	pgno_t mc_next_pgno;
	HANDLE mc_fd;
	int mc_toggle;			/**< Buffer number in provider */
	int mc_new;				/**< (0-2 buffers to write) | (#MDB_EOF at end) */
	/** Error code.  Never cleared if set.  Both threads can set nonzero
	 *	to fail the copy.  Not mutex-protected, LMDB expects atomic int.
	 */
	volatile int mc_error;
};

	/** Dedicated writer thread for compacting copy. */
THREAD_RET ESECT CALL_CONV
mdb_env_copythr(void *arg)
{
	mdb_copy *my = (mdb_copy*)arg;
	char *ptr;
	int toggle = 0, wsize, rc;
#ifdef _WIN32
	DWORD len;
#define DO_WRITE(rc, fd, ptr, w2, len)	rc = WriteFile(fd, ptr, w2, &len, NULL)
#else
	int len;
#define DO_WRITE(rc, fd, ptr, w2, len)	len = write(fd, ptr, w2); rc = (len >= 0)
#ifdef SIGPIPE
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	if ((rc = pthread_sigmask(SIG_BLOCK, &set, NULL)) != 0)
		my->mc_error = rc;
#endif
#endif

	pthread_mutex_lock(&my->mc_mutex);
	for(;;) {
		while (!my->mc_new)
			pthread_cond_wait(&my->mc_cond, &my->mc_mutex);
		if (my->mc_new == 0 + MDB_EOF) /* 0 buffers, just EOF */
			break;
		wsize = my->mc_wlen[toggle];
		ptr = my->mc_wbuf[toggle];
again:
		rc = MDB_SUCCESS;
		while (wsize > 0 && !my->mc_error) {
			DO_WRITE(rc, my->mc_fd, ptr, wsize, len);
			if (!rc) {
				rc = ErrCode();
#if defined(SIGPIPE) && !defined(_WIN32)
				if (rc == EPIPE) {
					/* Collect the pending SIGPIPE, otherwise at least OS X
					 * gives it to the process on thread-exit (ITS#8504).
					 */
					int tmp;
					sigwait(&set, &tmp);
				}
#endif
				break;
			} else if (len > 0) {
				rc = MDB_SUCCESS;
				ptr += len;
				wsize -= len;
				continue;
			} else {
				rc = EIO;
				break;
			}
		}
		if (rc) {
			my->mc_error = rc;
		}
		/* If there's an overflow page tail, write it too */
		if (my->mc_olen[toggle]) {
			wsize = my->mc_olen[toggle];
			ptr = my->mc_over[toggle];
			my->mc_olen[toggle] = 0;
			goto again;
		}
		my->mc_wlen[toggle] = 0;
		toggle ^= 1;
		/* Return the empty buffer to provider */
		my->mc_new--;
		pthread_cond_signal(&my->mc_cond);
	}
	pthread_mutex_unlock(&my->mc_mutex);
	return (THREAD_RET)0;
#undef DO_WRITE
}

	/** Give buffer and/or #MDB_EOF to writer thread, await unused buffer.
	 *
	 * @param[in] my control structure.
	 * @param[in] adjust (1 to hand off 1 buffer) | (MDB_EOF when ending).
	 */
int ESECT
mdb_env_cthr_toggle(mdb_copy *my, int adjust)
{
	pthread_mutex_lock(&my->mc_mutex);
	my->mc_new += adjust;
	pthread_cond_signal(&my->mc_cond);
	while (my->mc_new & 2)		/* both buffers in use */
		pthread_cond_wait(&my->mc_cond, &my->mc_mutex);
	pthread_mutex_unlock(&my->mc_mutex);

	my->mc_toggle ^= (adjust & 1);
	/* Both threads reset mc_wlen, to be safe from threading errors */
	my->mc_wlen[my->mc_toggle] = 0;
	return my->mc_error;
}

	/** Depth-first tree traversal for compacting copy.
	 * @param[in] my control structure.
	 * @param[in,out] pg database root.
	 * @param[in] flags includes #F_DUPDATA if it is a sorted-duplicate sub-DB.
	 */
int ESECT
mdb_env_cwalk(mdb_copy *my, pgno_t *pg, int flags)
{
	MDB_cursor mc = {0};
	MDB_node *ni;
	MDB_page *mo, *mp, *leaf;
	char *buf, *ptr;
	int rc, toggle;
	unsigned int i;

	/* Empty DB, nothing to do */
	if (*pg == P_INVALID)
		return MDB_SUCCESS;

	mc.mc_snum = 1;
	mc.mc_txn = my->mc_txn;
	mc.mc_flags = my->mc_txn->mt_flags & (C_ORIG_RDONLY|C_WRITEMAP);

	rc = mdb_page_get(&mc, *pg, &mc.mc_pg[0], NULL);
	if (rc)
		return rc;
	rc = mdb_page_search_root(&mc, NULL, MDB_PS_FIRST);
	if (rc)
		return rc;

	/* Make cursor pages writable */
	buf = ptr = (char*)malloc(my->mc_env->me_psize * mc.mc_snum);
	if (buf == NULL)
		return ENOMEM;

	for (i=0; i<mc.mc_top; i++) {
		mdb_page_copy((MDB_page *)ptr, mc.mc_pg[i], my->mc_env->me_psize);
		mc.mc_pg[i] = (MDB_page *)ptr;
		ptr += my->mc_env->me_psize;
	}

	/* This is writable space for a leaf page. Usually not needed. */
	leaf = (MDB_page *)ptr;

	toggle = my->mc_toggle;
	while (mc.mc_snum > 0) {
		unsigned n;
		mp = mc.mc_pg[mc.mc_top];
		n = NUMKEYS(mp);

		if (IS_LEAF(mp)) {
			if (!IS_LEAF2(mp) && !(flags & F_DUPDATA)) {
				for (i=0; i<n; i++) {
					ni = NODEPTR(mp, i);
					if (ni->mn_flags & F_BIGDATA) {
						MDB_page *omp;
						pgno_t pg;

						/* Need writable leaf */
						if (mp != leaf) {
							mc.mc_pg[mc.mc_top] = leaf;
							mdb_page_copy(leaf, mp, my->mc_env->me_psize);
							mp = leaf;
							ni = NODEPTR(mp, i);
						}

						memcpy(&pg, NODEDATA(ni), sizeof(pg));
						memcpy(NODEDATA(ni), &my->mc_next_pgno, sizeof(pgno_t));
						rc = mdb_page_get(&mc, pg, &omp, NULL);
						if (rc)
							goto done;
						if (my->mc_wlen[toggle] >= MDB_WBUF) {
							rc = mdb_env_cthr_toggle(my, 1);
							if (rc)
								goto done;
							toggle = my->mc_toggle;
						}
						mo = (MDB_page *)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
						memcpy(mo, omp, my->mc_env->me_psize);
						mo->mp_pgno = my->mc_next_pgno;
						my->mc_next_pgno += omp->mp_pages;
						my->mc_wlen[toggle] += my->mc_env->me_psize;
						if (omp->mp_pages > 1) {
							my->mc_olen[toggle] = my->mc_env->me_psize * (omp->mp_pages - 1);
							my->mc_over[toggle] = (char *)omp + my->mc_env->me_psize;
							rc = mdb_env_cthr_toggle(my, 1);
							if (rc)
								goto done;
							toggle = my->mc_toggle;
						}
					} else if (ni->mn_flags & F_SUBDATA) {
						MDB_db db;

						/* Need writable leaf */
						if (mp != leaf) {
							mc.mc_pg[mc.mc_top] = leaf;
							mdb_page_copy(leaf, mp, my->mc_env->me_psize);
							mp = leaf;
							ni = NODEPTR(mp, i);
						}

						memcpy(&db, NODEDATA(ni), sizeof(db));
						my->mc_toggle = toggle;
						rc = mdb_env_cwalk(my, &db.md_root, ni->mn_flags & F_DUPDATA);
						if (rc)
							goto done;
						toggle = my->mc_toggle;
						memcpy(NODEDATA(ni), &db, sizeof(db));
					}
				}
			}
		} else {
			mc.mc_ki[mc.mc_top]++;
			if (mc.mc_ki[mc.mc_top] < n) {
				pgno_t pg;
again:
				ni = NODEPTR(mp, mc.mc_ki[mc.mc_top]);
				pg = NODEPGNO(ni);
				rc = mdb_page_get(&mc, pg, &mp, NULL);
				if (rc)
					goto done;
				mc.mc_top++;
				mc.mc_snum++;
				mc.mc_ki[mc.mc_top] = 0;
				if (IS_BRANCH(mp)) {
					/* Whenever we advance to a sibling branch page,
					 * we must proceed all the way down to its first leaf.
					 */
					mdb_page_copy(mc.mc_pg[mc.mc_top], mp, my->mc_env->me_psize);
					goto again;
				} else
					mc.mc_pg[mc.mc_top] = mp;
				continue;
			}
		}
		if (my->mc_wlen[toggle] >= MDB_WBUF) {
			rc = mdb_env_cthr_toggle(my, 1);
			if (rc)
				goto done;
			toggle = my->mc_toggle;
		}
		mo = (MDB_page *)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
		mdb_page_copy(mo, mp, my->mc_env->me_psize);
		mo->mp_pgno = my->mc_next_pgno++;
		my->mc_wlen[toggle] += my->mc_env->me_psize;
		if (mc.mc_top) {
			/* Update parent if there is one */
			ni = NODEPTR(mc.mc_pg[mc.mc_top-1], mc.mc_ki[mc.mc_top-1]);
			SETPGNO(ni, mo->mp_pgno);
			mdb_cursor_pop(&mc);
		} else {
			/* Otherwise we're done */
			*pg = mo->mp_pgno;
			break;
		}
	}
done:
	free(buf);
	return rc;
}

	/** Copy environment with compaction. */
int ESECT
mdb_env_copyfd1(MDB_env *env, HANDLE fd)
{
	MDB_meta *mm;
	MDB_page *mp;
	mdb_copy my = {0};
	MDB_txn *txn = NULL;
	pthread_t thr;
	pgno_t root, new_root;
	int rc = MDB_SUCCESS;

#ifdef _WIN32
	if (!(my.mc_mutex = CreateMutex(NULL, FALSE, NULL)) ||
		!(my.mc_cond = CreateEvent(NULL, FALSE, FALSE, NULL))) {
		rc = ErrCode();
		goto done;
	}
	my.mc_wbuf[0] = (char*) _aligned_malloc(MDB_WBUF*2, env->me_os_psize);
	if (my.mc_wbuf[0] == NULL) {
		/* _aligned_malloc() sets errno, but we use Windows error codes */
		rc = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
#else
	if ((rc = pthread_mutex_init(&my.mc_mutex, NULL)) != 0)
		return rc;
	if ((rc = pthread_cond_init(&my.mc_cond, NULL)) != 0)
		goto done2;
#ifdef HAVE_MEMALIGN
	my.mc_wbuf[0] = memalign(env->me_os_psize, MDB_WBUF*2);
	if (my.mc_wbuf[0] == NULL) {
		rc = errno;
		goto done;
	}
#else
	{
		void *p;
		if ((rc = posix_memalign(&p, env->me_os_psize, MDB_WBUF*2)) != 0)
			goto done;
		my.mc_wbuf[0] = (char*)p;
	}
#endif
#endif
	memset(my.mc_wbuf[0], 0, MDB_WBUF*2);
	my.mc_wbuf[1] = my.mc_wbuf[0] + MDB_WBUF;
	my.mc_next_pgno = NUM_METAS;
	my.mc_env = env;
	my.mc_fd = fd;
	rc = THREAD_CREATE(thr, mdb_env_copythr, &my);
	if (rc)
		goto done;

	rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
	if (rc)
		goto finish;

	mp = (MDB_page *)my.mc_wbuf[0];
	memset(mp, 0, NUM_METAS * env->me_psize);
	mp->mp_pgno = 0;
	mp->mp_flags = P_META;
	mm = (MDB_meta *)METADATA(mp);
	mdb_env_init_meta0(env, mm);
	mm->mm_address = env->me_metas[0]->mm_address;

	mp = (MDB_page *)(my.mc_wbuf[0] + env->me_psize);
	mp->mp_pgno = 1;
	mp->mp_flags = P_META;
	*(MDB_meta *)METADATA(mp) = *mm;
	mm = (MDB_meta *)METADATA(mp);

	/* Set metapage 1 with current main DB */
	root = new_root = txn->mt_dbs[MAIN_DBI].md_root;
	if (root != P_INVALID) {
		/* Count free pages + freeDB pages.  Subtract from last_pg
		 * to find the new last_pg, which also becomes the new root.
		 */
		MDB_ID freecount = 0;
		MDB_cursor mc;
		MDB_val key, data;
		mdb_cursor_init(&mc, txn, FREE_DBI, NULL);
		while ((rc = mdb_cursor_get(&mc, &key, &data, MDB_NEXT)) == 0)
			freecount += *(MDB_ID *)data.mv_data;
		if (rc != MDB_NOTFOUND)
			goto finish;
		freecount += txn->mt_dbs[FREE_DBI].md_branch_pages +
			txn->mt_dbs[FREE_DBI].md_leaf_pages +
			txn->mt_dbs[FREE_DBI].md_overflow_pages;

		new_root = txn->mt_next_pgno - 1 - freecount;
		mm->mm_last_pg = new_root;
		mm->mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
		mm->mm_dbs[MAIN_DBI].md_root = new_root;
	} else {
		/* When the DB is empty, handle it specially to
		 * fix any breakage like page leaks from ITS#8174.
		 */
		mm->mm_dbs[MAIN_DBI].md_flags = txn->mt_dbs[MAIN_DBI].md_flags;
	}
	if (root != P_INVALID || mm->mm_dbs[MAIN_DBI].md_flags) {
		mm->mm_txnid = 1;		/* use metapage 1 */
	}

	my.mc_wlen[0] = env->me_psize * NUM_METAS;
	my.mc_txn = txn;
	rc = mdb_env_cwalk(&my, &root, 0);
	if (rc == MDB_SUCCESS && root != new_root) {
		rc = MDB_INCOMPATIBLE;	/* page leak or corrupt DB */
	}

finish:
	if (rc)
		my.mc_error = rc;
	mdb_env_cthr_toggle(&my, 1 | MDB_EOF);
	rc = THREAD_FINISH(thr);
	_mdb_txn_abort(txn);

done:
#ifdef _WIN32
	if (my.mc_wbuf[0]) _aligned_free(my.mc_wbuf[0]);
	if (my.mc_cond)  CloseHandle(my.mc_cond);
	if (my.mc_mutex) CloseHandle(my.mc_mutex);
#else
	free(my.mc_wbuf[0]);
	pthread_cond_destroy(&my.mc_cond);
done2:
	pthread_mutex_destroy(&my.mc_mutex);
#endif
	return rc ? rc : my.mc_error;
}

static int ESECT mdb_fsize(HANDLE fd, mdb_size_t *size)
{
#ifdef _WIN32
	LARGE_INTEGER fsize;

	if (!GetFileSizeEx(fd, &fsize))
		return ErrCode();

	*size = fsize.QuadPart;
#else
	struct stat st;

	if (fstat(fd, &st))
		return ErrCode();

	*size = st.st_size;
#endif
	return MDB_SUCCESS;
}

	/** Copy environment as-is. */
int ESECT
mdb_env_copyfd0(MDB_env *env, HANDLE fd)
{
	MDB_txn *txn = NULL;
	mdb_mutexref_t wmutex = NULL;
	int rc;
	mdb_size_t wsize, w3;
	char *ptr;
#ifdef _WIN32
	DWORD len, w2;
#define DO_WRITE(rc, fd, ptr, w2, len)	rc = WriteFile(fd, ptr, w2, &len, NULL)
#else
	ssize_t len;
	size_t w2;
#define DO_WRITE(rc, fd, ptr, w2, len)	len = write(fd, ptr, w2); rc = (len >= 0)
#endif

	/* Do the lock/unlock of the reader mutex before starting the
	 * write txn.  Otherwise other read txns could block writers.
	 */
	rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
	if (rc)
		return rc;

	if (env->me_txns) {
		/* We must start the actual read txn after blocking writers */
		mdb_txn_end(txn, MDB_END_RESET_TMP);

		/* Temporarily block writers until we snapshot the meta pages */
		wmutex = env->me_wmutex;
		if (LOCK_MUTEX(rc, env, wmutex))
			goto leave;

		rc = mdb_txn_renew0(txn);
		if (rc) {
			UNLOCK_MUTEX(wmutex);
			goto leave;
		}
	}

	wsize = env->me_psize * NUM_METAS;
	ptr = env->me_map;
	w2 = wsize;
	while (w2 > 0) {
		DO_WRITE(rc, fd, ptr, w2, len);
		if (!rc) {
			rc = ErrCode();
			break;
		} else if (len > 0) {
			rc = MDB_SUCCESS;
			ptr += len;
			w2 -= len;
			continue;
		} else {
			/* Non-blocking or async handles are not supported */
			rc = EIO;
			break;
		}
	}
	if (wmutex)
		UNLOCK_MUTEX(wmutex);

	if (rc)
		goto leave;

	w3 = txn->mt_next_pgno * env->me_psize;
	{
		mdb_size_t fsize = 0;
		if ((rc = mdb_fsize(env->me_fd, &fsize)))
			goto leave;
		if (w3 > fsize)
			w3 = fsize;
	}
	wsize = w3 - wsize;
	while (wsize > 0) {
		if (wsize > MAX_WRITE)
			w2 = MAX_WRITE;
		else
			w2 = wsize;
		DO_WRITE(rc, fd, ptr, w2, len);
		if (!rc) {
			rc = ErrCode();
			break;
		} else if (len > 0) {
			rc = MDB_SUCCESS;
			ptr += len;
			wsize -= len;
			continue;
		} else {
			rc = EIO;
			break;
		}
	}

leave:
	_mdb_txn_abort(txn);
	return rc;
}

int ESECT
mdb_env_copyfd2(MDB_env *env, HANDLE fd, unsigned int flags)
{
	if (flags & MDB_CP_COMPACT)
		return mdb_env_copyfd1(env, fd);
	else
		return mdb_env_copyfd0(env, fd);
}

int ESECT
mdb_env_copyfd(MDB_env *env, HANDLE fd)
{
	return mdb_env_copyfd2(env, fd, 0);
}

int ESECT
mdb_env_copy2(MDB_env *env, const char *path, unsigned int flags)
{
	int rc;
	MDB_name fname;
	HANDLE newfd = INVALID_HANDLE_VALUE;

	rc = mdb_fname_init(path, env->me_flags | MDB_NOLOCK, &fname);
	if (rc == MDB_SUCCESS) {
		rc = mdb_fopen(env, &fname, MDB_O_COPY, 0666, &newfd);
		mdb_fname_destroy(fname);
	}
	if (rc == MDB_SUCCESS) {
		rc = mdb_env_copyfd2(env, newfd, flags);
		if (close(newfd) < 0 && rc == MDB_SUCCESS)
			rc = ErrCode();
	}
	return rc;
}

int ESECT
mdb_env_copy(MDB_env *env, const char *path)
{
	return mdb_env_copy2(env, path, 0);
}

int ESECT
mdb_env_set_flags(MDB_env *env, unsigned int flag, int onoff)
{
	if (flag & ~CHANGEABLE)
		return EINVAL;
	if (onoff)
		env->me_flags |= flag;
	else
		env->me_flags &= ~flag;
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_flags(MDB_env *env, unsigned int *arg)
{
	if (!env || !arg)
		return EINVAL;

	*arg = env->me_flags & (CHANGEABLE|CHANGELESS);
	return MDB_SUCCESS;
}

int ESECT
mdb_env_set_userctx(MDB_env *env, void *ctx)
{
	if (!env)
		return EINVAL;
	env->me_userctx = ctx;
	return MDB_SUCCESS;
}

void * ESECT
mdb_env_get_userctx(MDB_env *env)
{
	return env ? env->me_userctx : NULL;
}

int ESECT
mdb_env_set_assert(MDB_env *env, MDB_assert_func *func)
{
	if (!env)
		return EINVAL;
#ifndef NDEBUG
	env->me_assert_func = func;
#endif
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_path(MDB_env *env, const char **arg)
{
	if (!env || !arg)
		return EINVAL;

	*arg = env->me_path;
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_fd(MDB_env *env, mdb_filehandle_t *arg)
{
	if (!env || !arg)
		return EINVAL;

	*arg = env->me_fd;
	return MDB_SUCCESS;
}

/** Common code for #mdb_stat() and #mdb_env_stat().
 * @param[in] env the environment to operate in.
 * @param[in] db the #MDB_db record containing the stats to return.
 * @param[out] arg the address of an #MDB_stat structure to receive the stats.
 * @return 0, this function always succeeds.
 */
static int ESECT mdb_stat0(MDB_env *env, MDB_db *db, MDB_stat *arg)
{
	arg->ms_psize = env->me_psize;
	arg->ms_depth = db->md_depth;
	arg->ms_branch_pages = db->md_branch_pages;
	arg->ms_leaf_pages = db->md_leaf_pages;
	arg->ms_overflow_pages = db->md_overflow_pages;
	arg->ms_entries = db->md_entries;

	return MDB_SUCCESS;
}

int ESECT mdb_stat(MDB_txn *txn, MDB_dbi dbi, MDB_stat *arg)
{
	if (!arg || !TXN_DBI_EXIST(txn, dbi, DB_VALID))
		return EINVAL;

	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	if (txn->mt_dbflags[dbi] & DB_STALE) {
		MDB_cursor mc;
		MDB_xcursor mx;
		/* Stale, must read the DB's root. cursor_init does it for us. */
		mdb_cursor_init(&mc, txn, dbi, &mx);
	}
	return mdb_stat0(txn->mt_env, &txn->mt_dbs[dbi], arg);
}

int ESECT
mdb_env_stat(MDB_env *env, MDB_stat *arg)
{
	MDB_meta *meta;

	if (env == NULL || arg == NULL)
		return EINVAL;

	meta = mdb_env_pick_meta(env);

	return mdb_stat0(env, &meta->mm_dbs[MAIN_DBI], arg);
}

int ESECT
mdb_env_info(MDB_env *env, MDB_envinfo *arg)
{
	MDB_meta *meta;

	if (env == NULL || arg == NULL)
		return EINVAL;

	meta = mdb_env_pick_meta(env);
	arg->me_mapaddr = meta->mm_address;
	arg->me_last_pgno = meta->mm_last_pg;
	arg->me_last_txnid = meta->mm_txnid;

	arg->me_mapsize = env->me_mapsize;
	arg->me_maxreaders = env->me_maxreaders;
	arg->me_numreaders = env->me_txns ? env->me_txns->mti_numreaders : 0;
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_maxkeysize(MDB_env *env)
{
	return ENV_MAXKEY(env);
}

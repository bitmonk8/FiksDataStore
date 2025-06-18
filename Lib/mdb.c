/** @file mdb.c
 *	@brief Lightning memory-mapped database library
 *
 *	A Btree-based database management library modeled loosely on the
 *	BerkeleyDB API, but much simplified.
 */
/*
 * Copyright 2011-2021 Howard Chu, Symas Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 *
 * This code is derived from btree.c written by Martin Hedenfalk.
 *
 * Copyright (c) 2009, 2010 Martin Hedenfalk <martin@bzero.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "mdb_internal.h"
#include "mdb_hash.h"
#include "mdb_page.h"
#include "mdb_compare.h"
#include "mdb_env.h"
#include "mdb_txn.h"

#ifdef _WIN32
struct MDB_name;
int utf8_to_utf16(const char *src, struct MDB_name *dst, int xtra);
#endif

#ifndef NDEBUG
void ESECT mdb_assert_fail(MDB_env *env, const char *expr_txt,
	const char *func, const char *file, int line)
{
	char buf[400];
	sprintf(buf, "%.100s:%d: Assertion '%.200s' failed in %.40s()",
		file, line, expr_txt, func);
	if (env->me_assert_func)
		env->me_assert_func(env, buf);
	fprintf(stderr, "%s\n", buf);
	abort();
}
#endif /* NDEBUG */

#if MDB_DEBUG
/** Return the page number of \b mp which may be sub-page, for debug output */
pgno_t
mdb_dbg_pgno(MDB_page *mp)
{
	pgno_t ret;
	COPY_PGNO(ret, MP_PGNO(mp));
	return ret;
}

/** Display a key in hexadecimal and return the address of the result.
 * @param[in] key the key to display
 * @param[in] buf the buffer to write into. Should always be #DKBUF.
 * @return The key in hexadecimal form.
 */
char *
mdb_dkey(MDB_val *key, char *buf)
{
	char *ptr = buf;
	unsigned char *c = key->mv_data;
	unsigned int i;

	if (!key)
		return "";

	if (key->mv_size > DKBUF_MAXKEYSIZE)
		return "MDB_MAXKEYSIZE";
	/* may want to make this a dynamic check: if the key is mostly
	 * printable characters, print it as-is instead of converting to hex.
	 */
	buf[0] = '\0';
	for (i=0; i<key->mv_size; i++)
		ptr += sprintf(ptr, "%02x", *c++);
	return buf;
}

char *
mdb_dval(MDB_txn *txn, MDB_dbi dbi, MDB_val *data, char *buf)
{
	if (txn->mt_dbs[dbi].md_flags & MDB_DUPSORT) {
		mdb_dkey(data, buf+1);
		*buf = '[';
		strcpy(buf + data->mv_size * 2 + 1, "]");
	} else
		*buf = '\0';
	return buf;
}

const char *
mdb_leafnode_type(MDB_node *n)
{
	static char *const tp[2][2] = {{"", ": DB"}, {": sub-page", ": sub-DB"}};
	return F_ISSET(n->mn_flags, F_BIGDATA) ? ": overflow page" :
		tp[F_ISSET(n->mn_flags, F_DUPDATA)][F_ISSET(n->mn_flags, F_SUBDATA)];
}

/** Display all the keys in the page. */
void
mdb_page_list(MDB_page *mp)
{
	pgno_t pgno = mdb_dbg_pgno(mp);
	const char *type, *state = (MP_FLAGS(mp) & P_DIRTY) ? ", dirty" : "";
	MDB_node *node;
	unsigned int i, nkeys, nsize, total = 0;
	MDB_val key;
	DKBUF;

	switch (MP_FLAGS(mp) & (P_BRANCH|P_LEAF|P_LEAF2|P_META|P_OVERFLOW|P_SUBP)) {
	case P_BRANCH:              type = "Branch page";		break;
	case P_LEAF:                type = "Leaf page";			break;
	case P_LEAF|P_SUBP:         type = "Sub-page";			break;
	case P_LEAF|P_LEAF2:        type = "LEAF2 page";		break;
	case P_LEAF|P_LEAF2|P_SUBP: type = "LEAF2 sub-page";	break;
	case P_OVERFLOW:
		fprintf(stderr, "Overflow page %"Yu" pages %u%s\n",
			pgno, mp->mp_pages, state);
		return;
	case P_META:
		fprintf(stderr, "Meta-page %"Yu" txnid %"Yu"\n",
			pgno, ((MDB_meta *)METADATA(mp))->mm_txnid);
		return;
	default:
		fprintf(stderr, "Bad page %"Yu" flags 0x%X\n", pgno, MP_FLAGS(mp));
		return;
	}

	nkeys = NUMKEYS(mp);
	fprintf(stderr, "%s %"Yu" numkeys %d%s\n", type, pgno, nkeys, state);

	for (i=0; i<nkeys; i++) {
		if (IS_LEAF2(mp)) {	/* LEAF2 pages have no mp_ptrs[] or node headers */
			key.mv_size = nsize = mp->mp_pad;
			key.mv_data = LEAF2KEY(mp, i, nsize);
			total += nsize;
			fprintf(stderr, "key %d: nsize %d, %s\n", i, nsize, DKEY(&key));
			continue;
		}
		node = NODEPTR(mp, i);
		key.mv_size = node->mn_ksize;
		key.mv_data = node->mn_data;
		nsize = NODESIZE + key.mv_size;
		if (IS_BRANCH(mp)) {
			fprintf(stderr, "key %d: page %"Yu", %s\n", i, NODEPGNO(node),
				DKEY(&key));
			total += nsize;
		} else {
			if (F_ISSET(node->mn_flags, F_BIGDATA))
				nsize += sizeof(pgno_t);
			else
				nsize += NODEDSZ(node);
			total += nsize;
			nsize += sizeof(indx_t);
			fprintf(stderr, "key %d: nsize %d, %s%s\n",
				i, nsize, DKEY(&key), mdb_leafnode_type(node));
		}
		total = EVEN(total);
	}
	fprintf(stderr, "Total: header %d + contents %d + unused %d\n",
		IS_LEAF2(mp) ? PAGEHDRSZ : PAGEBASE + MP_LOWER(mp), total, SIZELEFT(mp));
}
#endif

#if (MDB_DEBUG) > 2
/** Count all the pages in each DB and in the freelist
 *  and make sure it matches the actual number of pages
 *  being used.
 *  All named DBs must be open for a correct count.
 */
void mdb_audit(MDB_txn *txn)
{
	MDB_cursor mc;
	MDB_val key, data;
	MDB_ID freecount, count;
	MDB_dbi i;
	int rc;

	freecount = 0;
	mdb_cursor_init(&mc, txn, FREE_DBI, NULL);
	while ((rc = mdb_cursor_get(&mc, &key, &data, MDB_NEXT)) == 0)
		freecount += *(MDB_ID *)data.mv_data;
	mdb_tassert(txn, rc == MDB_NOTFOUND);

	count = 0;
	for (i = 0; i<txn->mt_numdbs; i++) {
		MDB_xcursor mx;
		if (!(txn->mt_dbflags[i] & DB_VALID))
			continue;
		mdb_cursor_init(&mc, txn, i, &mx);
		if (txn->mt_dbs[i].md_root == P_INVALID)
			continue;
		count += txn->mt_dbs[i].md_branch_pages +
			txn->mt_dbs[i].md_leaf_pages +
			txn->mt_dbs[i].md_overflow_pages;
		if (txn->mt_dbs[i].md_flags & MDB_DUPSORT) {
			rc = mdb_page_search(&mc, NULL, MDB_PS_FIRST);
			for (; rc == MDB_SUCCESS; rc = mdb_cursor_sibling(&mc, 1)) {
				unsigned j;
				MDB_page *mp;
				mp = mc.mc_pg[mc.mc_top];
				for (j=0; j<NUMKEYS(mp); j++) {
					MDB_node *leaf = NODEPTR(mp, j);
					if (leaf->mn_flags & F_SUBDATA) {
						MDB_db db;
						memcpy(&db, NODEDATA(leaf), sizeof(db));
						count += db.md_branch_pages + db.md_leaf_pages +
							db.md_overflow_pages;
					}
				}
			}
			mdb_tassert(txn, rc == MDB_NOTFOUND);
		}
	}
	if (freecount + count + NUM_METAS != txn->mt_next_pgno) {
		fprintf(stderr, "audit: %"Yu" freecount: %"Yu" count: %"Yu" total: %"Yu" next_pgno: %"Yu"\n",
			txn->mt_txnid, freecount, count+NUM_METAS,
			freecount+count+NUM_METAS, txn->mt_next_pgno);
	}
}
#endif

int
mdb_cmp(MDB_txn *txn, MDB_dbi dbi, const MDB_val *a, const MDB_val *b)
{
	return txn->mt_dbxs[dbi].md_cmp(a, b);
}

int
mdb_dcmp(MDB_txn *txn, MDB_dbi dbi, const MDB_val *a, const MDB_val *b)
{
	MDB_cmp_func *dcmp = txn->mt_dbxs[dbi].md_dcmp;
	if (NEED_CMP_CLONG(dcmp, a->mv_size))
		dcmp = mdb_cmp_clong;
	return dcmp(a, b);
}

/** Filename suffixes [datafile,lockfile][without,with MDB_NOSUBDIR] */
const mdb_nchar_t *const mdb_suffixes[2][2] = {
	{ MDB_NAME("/data.mdb"), MDB_NAME("")      },
	{ MDB_NAME("/lock.mdb"), MDB_NAME("-lock") }
};

#define MDB_SUFFLEN 9	/**< Max string length in #mdb_suffixes[] */

/** Set up filename + scratch area for filename suffix, for opening files.
 * It should be freed with #mdb_fname_destroy().
 * On Windows, paths are converted from char *UTF-8 to wchar_t *UTF-16.
 *
 * @param[in] path Pathname for #mdb_env_open().
 * @param[in] envflags Whether a subdir and/or lockfile will be used.
 * @param[out] fname Resulting filename, with room for a suffix if necessary.
 */
int ESECT
mdb_fname_init(const char *path, unsigned envflags, MDB_name *fname)
{
	int no_suffix = F_ISSET(envflags, MDB_NOSUBDIR|MDB_NOLOCK);
	fname->mn_alloced = 0;
#ifdef _WIN32
	return utf8_to_utf16(path, fname, no_suffix ? 0 : MDB_SUFFLEN);
#else
	fname->mn_len = strlen(path);
	if (no_suffix)
		fname->mn_val = (char *) path;
	else if ((fname->mn_val = malloc(fname->mn_len + MDB_SUFFLEN+1)) != NULL) {
		fname->mn_alloced = 1;
		strcpy(fname->mn_val, path);
	}
	else
		return ENOMEM;
	return MDB_SUCCESS;
#endif
}

/** Open an LMDB file.
 * @param[in] env	The LMDB environment.
 * @param[in,out] fname	Path from from #mdb_fname_init().  A suffix is
 * appended if necessary to create the filename, without changing mn_len.
 * @param[in] which	Determines file type, access mode, etc.
 * @param[in] mode	The Unix permissions for the file, if we create it.
 * @param[out] res	Resulting file handle.
 * @return 0 on success, non-zero on failure.
 */
int ESECT
mdb_fopen(const MDB_env *env, MDB_name *fname,
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

#ifdef _WIN32
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
			MDB_reader *r = pthread_getspecific(mdb_tls_keys[i]);
			if (r) {
				mdb_env_reader_dest(r);
			}
		}
		break;
	case DLL_PROCESS_DETACH: break;
	}
}
#ifdef __GNUC__
#ifdef _WIN64
const PIMAGE_TLS_CALLBACK mdb_tls_cbp __attribute__((section (".CRT$XLB"))) = mdb_tls_callback;
#else
PIMAGE_TLS_CALLBACK mdb_tls_cbp __attribute__((section (".CRT$XLB"))) = mdb_tls_callback;
#endif
#else
#ifdef _WIN64
/* Force some symbol references.
 *	_tls_used forces the linker to create the TLS directory if not already done
 *	mdb_tls_cbp prevents whole-program-optimizer from dropping the symbol.
 */
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:mdb_tls_cbp")
#pragma const_seg(".CRT$XLB")
extern const PIMAGE_TLS_CALLBACK mdb_tls_cbp;
const PIMAGE_TLS_CALLBACK mdb_tls_cbp = mdb_tls_callback;
#pragma const_seg()
#else	/* _WIN32 */
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_mdb_tls_cbp")
#pragma data_seg(".CRT$XLB")
PIMAGE_TLS_CALLBACK mdb_tls_cbp = mdb_tls_callback;
#pragma data_seg()
#endif	/* WIN 32/64 */
#endif	/* !__GNUC__ */
#endif

int
mdb_get(MDB_txn *txn, MDB_dbi dbi,
    MDB_val *key, MDB_val *data)
{
	MDB_cursor	mc;
	MDB_xcursor	mx;
	int exact = 0, rc;
	DKBUF;

	DPRINTF(("===> get db %u key [%s]", dbi, DKEY(key)));

	if (!key || !data || !TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	mdb_cursor_init(&mc, txn, dbi, &mx);
	rc = mdb_cursor_set(&mc, key, data, MDB_SET, &exact);
	MDB_CURSOR_UNREF(&mc, 1);
	return rc;
}

/** Replace the key for a branch node with a new key.
 * Set #MDB_TXN_ERROR on failure.
 * @param[in] mc Cursor pointing to the node to operate on.
 * @param[in] key The new key to use.
 * @return 0 on success, non-zero on failure.
 */
int
mdb_update_key(MDB_cursor *mc, MDB_val *key)
{
	MDB_page		*mp;
	MDB_node		*node;
	char			*base;
	size_t			 len;
	int				 delta, ksize, oksize;
	indx_t			 ptr, i, numkeys, indx;
	DKBUF;

	indx = mc->mc_ki[mc->mc_top];
	mp = mc->mc_pg[mc->mc_top];
	node = NODEPTR(mp, indx);
	ptr = mp->mp_ptrs[indx];
#if MDB_DEBUG
	{
		MDB_val	k2;
		char kbuf2[DKBUF_MAXKEYSIZE*2+1];
		k2.mv_data = NODEKEY(node);
		k2.mv_size = node->mn_ksize;
		DPRINTF(("update key %u (ofs %u) [%s] to [%s] on page %"Yu,
			indx, ptr,
			mdb_dkey(&k2, kbuf2),
			DKEY(key),
			mp->mp_pgno));
	}
#endif

	/* Sizes must be 2-byte aligned. */
	ksize = EVEN(key->mv_size);
	oksize = EVEN(node->mn_ksize);
	delta = ksize - oksize;

	/* Shift node contents if EVEN(key length) changed. */
	if (delta) {
		if (delta > 0 && SIZELEFT(mp) < delta) {
			pgno_t pgno;
			/* not enough space left, do a delete and split */
			DPRINTF(("Not enough room, delta = %d, splitting...", delta));
			pgno = NODEPGNO(node);
			mdb_node_del(mc, 0);
			return mdb_page_split(mc, key, NULL, pgno, MDB_SPLIT_REPLACE);
		}

		numkeys = NUMKEYS(mp);
		for (i = 0; i < numkeys; i++) {
			if (mp->mp_ptrs[i] <= ptr)
				mp->mp_ptrs[i] -= delta;
		}

		base = (char *)mp + mp->mp_upper + PAGEBASE;
		len = ptr - mp->mp_upper + NODESIZE;
		memmove(base - delta, base, len);
		mp->mp_upper -= delta;

		node = NODEPTR(mp, indx);
	}

	/* But even if no shift was needed, update ksize */
	if (node->mn_ksize != key->mv_size)
		node->mn_ksize = key->mv_size;

	if (key->mv_size)
		memcpy(NODEKEY(node), key->mv_data, key->mv_size);

	return MDB_SUCCESS;
}

int
mdb_del(MDB_txn *txn, MDB_dbi dbi,
    MDB_val *key, MDB_val *data)
{
	DKBUF;
	DDBUF;
	if (!key || !TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	if (txn->mt_flags & (MDB_TXN_RDONLY|MDB_TXN_BLOCKED))
		return (txn->mt_flags & MDB_TXN_RDONLY) ? EACCES : MDB_BAD_TXN;

	if (!F_ISSET(txn->mt_dbs[dbi].md_flags, MDB_DUPSORT)) {
		/* must ignore any data */
		data = NULL;
	}

	MDB_TRACE(("%p, %u, %"Z"u[%s], %"Z"u%s",
		txn, dbi, key ? key->mv_size:0, DKEY(key), data ? data->mv_size:0,
		data ? mdb_dval(txn, dbi, data, dbuf):""));
	return mdb_del0(txn, dbi, key, data, 0);
}

int
mdb_del0(MDB_txn *txn, MDB_dbi dbi,
	MDB_val *key, MDB_val *data, unsigned flags)
{
	MDB_cursor mc;
	MDB_xcursor mx;
	MDB_cursor_op op;
	MDB_val rdata, *xdata;
	int		 rc, exact = 0;
	DKBUF;

	DPRINTF(("====> delete db %u key [%s]", dbi, DKEY(key)));

	mdb_cursor_init(&mc, txn, dbi, &mx);

	if (data) {
		op = MDB_GET_BOTH;
		rdata = *data;
		xdata = &rdata;
	} else {
		op = MDB_SET;
		xdata = NULL;
		flags |= MDB_NODUPDATA;
	}
	rc = mdb_cursor_set(&mc, key, xdata, op, &exact);
	if (rc == 0) {
		/* let mdb_page_split know about this cursor if needed:
		 * delete will trigger a rebalance; if it needs to move
		 * a node from one page to another, it will have to
		 * update the parent's separator key(s). If the new sepkey
		 * is larger than the current one, the parent page may
		 * run out of space, triggering a split. We need this
		 * cursor to be consistent until the end of the rebalance.
		 */
		mc.mc_next = txn->mt_cursors[dbi];
		txn->mt_cursors[dbi] = &mc;
		rc = _mdb_cursor_del(&mc, flags);
		txn->mt_cursors[dbi] = mc.mc_next;
	}
	return rc;
}

int
mdb_put(MDB_txn *txn, MDB_dbi dbi,
    MDB_val *key, MDB_val *data, unsigned int flags)
{
	MDB_cursor mc;
	MDB_xcursor mx;
	int rc;
	DKBUF;
	DDBUF;

	if (!key || !data || !TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	if (flags & ~(MDB_NOOVERWRITE|MDB_NODUPDATA|MDB_RESERVE|MDB_APPEND|MDB_APPENDDUP))
		return EINVAL;

	if (txn->mt_flags & (MDB_TXN_RDONLY|MDB_TXN_BLOCKED))
		return (txn->mt_flags & MDB_TXN_RDONLY) ? EACCES : MDB_BAD_TXN;

	MDB_TRACE(("%p, %u, %"Z"u[%s], %"Z"u%s, %u",
		txn, dbi, key ? key->mv_size:0, DKEY(key), data->mv_size, mdb_dval(txn, dbi, data, dbuf), flags));
	mdb_cursor_init(&mc, txn, dbi, &mx);
	mc.mc_next = txn->mt_cursors[dbi];
	txn->mt_cursors[dbi] = &mc;
	rc = _mdb_cursor_put(&mc, key, data, flags);
	txn->mt_cursors[dbi] = mc.mc_next;
	return rc;
}

#if defined(_WIN32)
/** Convert \b src to new wchar_t[] string with room for \b xtra extra chars */
int ESECT
utf8_to_utf16(const char *src, MDB_name *dst, int xtra)
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
			result = malloc(sizeof(wchar_t) * (need + xtra));
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


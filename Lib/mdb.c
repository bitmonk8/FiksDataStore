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


/** @cond */
MDB_cmp_func	mdb_cmp_memn, mdb_cmp_memnr, mdb_cmp_int, mdb_cmp_cint, mdb_cmp_long;
/** @endcond */

/** Compare two items pointing at '#mdb_size_t's of unknown alignment. */
#ifdef MISALIGNED_OK
# define mdb_cmp_clong mdb_cmp_long
#else
# define mdb_cmp_clong mdb_cmp_cint
#endif

/** True if we need #mdb_cmp_clong() instead of \b cmp for #MDB_INTEGERDUP */
#define NEED_CMP_CLONG(cmp, ksize) \
	(UINT_MAX < MDB_SIZE_MAX && \
	 (cmp) == mdb_cmp_int && (ksize) == sizeof(mdb_size_t))

#ifdef _WIN32
SECURITY_DESCRIPTOR mdb_null_sd;
SECURITY_ATTRIBUTES mdb_all_sa;
int mdb_sec_inited;

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

void
mdb_cursor_chk(MDB_cursor *mc)
{
	unsigned int i;
	MDB_node *node;
	MDB_page *mp;

	if (!mc->mc_snum || !(mc->mc_flags & C_INITIALIZED)) return;
	for (i=0; i<mc->mc_top; i++) {
		mp = mc->mc_pg[i];
		node = NODEPTR(mp, mc->mc_ki[i]);
		if (NODEPGNO(node) != mc->mc_pg[i+1]->mp_pgno)
			printf("oops!\n");
	}
	if (mc->mc_ki[i] >= NUMKEYS(mc->mc_pg[i]))
		printf("ack!\n");
	if (XCURSOR_INITED(mc)) {
		node = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
		if (((node->mn_flags & (F_DUPDATA|F_SUBDATA)) == F_DUPDATA) &&
			mc->mc_xcursor->mx_cursor.mc_pg[0] != NODEDATA(node)) {
			printf("blah!\n");
		}
	}
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

/** Back up parent txn's cursors, then grab the originals for tracking */
int
mdb_cursor_shadow(MDB_txn *src, MDB_txn *dst)
{
	MDB_cursor *mc, *bk;
	MDB_xcursor *mx;
	size_t size;
	int i;

	for (i = src->mt_numdbs; --i >= 0; ) {
		if ((mc = src->mt_cursors[i]) != NULL) {
			size = sizeof(MDB_cursor);
			if (mc->mc_xcursor)
				size += sizeof(MDB_xcursor);
			for (; mc; mc = bk->mc_next) {
				bk = malloc(size);
				if (!bk)
					return ENOMEM;
				*bk = *mc;
				mc->mc_backup = bk;
				mc->mc_db = &dst->mt_dbs[i];
				/* Kill pointers into src to reduce abuse: The
				 * user may not use mc until dst ends. But we need a valid
				 * txn pointer here for cursor fixups to keep working.
				 */
				mc->mc_txn    = dst;
				mc->mc_dbflag = &dst->mt_dbflags[i];
				if ((mx = mc->mc_xcursor) != NULL) {
					*(MDB_xcursor *)(bk+1) = *mx;
					mx->mx_cursor.mc_txn = dst;
				}
				mc->mc_next = dst->mt_cursors[i];
				dst->mt_cursors[i] = mc;
			}
		}
	}
	return MDB_SUCCESS;
}

/** Close this write txn's cursors, give parent txn's cursors back to parent.
 * @param[in] txn the transaction handle.
 * @param[in] merge true to keep changes to parent cursors, false to revert.
 * @return 0 on success, non-zero on failure.
 */
void
mdb_cursors_close(MDB_txn *txn, unsigned merge)
{
	MDB_cursor **cursors = txn->mt_cursors, *mc, *next, *bk;
	MDB_xcursor *mx;
	int i;

	for (i = txn->mt_numdbs; --i >= 0; ) {
		for (mc = cursors[i]; mc; mc = next) {
			next = mc->mc_next;
			if ((bk = mc->mc_backup) != NULL) {
				if (merge) {
					/* Commit changes to parent txn */
					mc->mc_next = bk->mc_next;
					mc->mc_backup = bk->mc_backup;
					mc->mc_txn = bk->mc_txn;
					mc->mc_db = bk->mc_db;
					mc->mc_dbflag = bk->mc_dbflag;
					if ((mx = mc->mc_xcursor) != NULL)
						mx->mx_cursor.mc_txn = bk->mc_txn;
				} else {
					/* Abort nested txn */
					*mc = *bk;
					if ((mx = mc->mc_xcursor) != NULL)
						*mx = *(MDB_xcursor *)(bk+1);
				}
				mc = bk;
			}
			/* Only malloced cursors are permanently tracked. */
			free(mc);
		}
		cursors[i] = NULL;
	}
}

#if !(MDB_PIDLOCK)		/* Currently the same as defined(_WIN32) */
enum Pidlock_op {
	Pidset, Pidcheck
};
#else
enum Pidlock_op {
	Pidset = F_SETLK, Pidcheck = F_GETLK
};
#endif

/** Set or check a pid lock. Set returns 0 on success.
 * Check returns 0 if the process is certainly dead, nonzero if it may
 * be alive (the lock exists or an error happened so we do not know).
 *
 * On Windows Pidset is a no-op, we merely check for the existence
 * of the process with the given pid. On POSIX we use a single byte
 * lock on the lockfile, set at an offset equal to the pid.
 */
int
mdb_reader_pid(MDB_env *env, enum Pidlock_op op, MDB_PID_T pid)
{
#if !(MDB_PIDLOCK)		/* Currently the same as defined(_WIN32) */
	int ret = 0;
	HANDLE h;
	if (op == Pidcheck) {
		h = OpenProcess(env->me_pidquery, FALSE, pid);
		/* No documented "no such process" code, but other program use this: */
		if (!h)
			return ErrCode() != ERROR_INVALID_PARAMETER;
		/* A process exists until all handles to it close. Has it exited? */
		ret = WaitForSingleObject(h, 0) != 0;
		CloseHandle(h);
	}
	return ret;
#else
	for (;;) {
		int rc;
		struct flock lock_info;
		memset(&lock_info, 0, sizeof(lock_info));
		lock_info.l_type = F_WRLCK;
		lock_info.l_whence = SEEK_SET;
		lock_info.l_start = pid;
		lock_info.l_len = 1;
		if ((rc = fcntl(env->me_lfd, op, &lock_info)) == 0) {
			if (op == F_GETLK && lock_info.l_type != F_UNLCK)
				rc = -1;
		} else if ((rc = ErrCode()) == EINTR) {
			continue;
		}
		return rc;
	}
#endif
}

/** Common code for #mdb_txn_begin() and #mdb_txn_renew().
 * @param[in] txn the transaction handle to initialize
 * @return 0 on success, non-zero on failure.
 */
int
mdb_txn_renew0(MDB_txn *txn)
{
	MDB_env *env = txn->mt_env;
	MDB_txninfo *ti = env->me_txns;
	MDB_meta *meta;
	unsigned int i, nr, flags = txn->mt_flags;
	uint16_t x;
	int rc, new_notls = 0;

	if ((flags &= MDB_TXN_RDONLY) != 0) {
		if (!ti) {
			meta = mdb_env_pick_meta(env);
			txn->mt_txnid = meta->mm_txnid;
			txn->mt_u.reader = NULL;
		} else {
			MDB_reader *r = (env->me_flags & MDB_NOTLS) ? txn->mt_u.reader :
				pthread_getspecific(env->me_txkey);
			if (r) {
				if (r->mr_pid != env->me_pid || r->mr_txnid != (txnid_t)-1)
					return MDB_BAD_RSLOT;
			} else {
				MDB_PID_T pid = env->me_pid;
				MDB_THR_T tid = pthread_self();
				mdb_mutexref_t rmutex = env->me_rmutex;

				if (!env->me_live_reader) {
					rc = mdb_reader_pid(env, Pidset, pid);
					if (rc)
						return rc;
					env->me_live_reader = 1;
				}

				if (LOCK_MUTEX(rc, env, rmutex))
					return rc;
				nr = ti->mti_numreaders;
				for (i=0; i<nr; i++)
					if (ti->mti_readers[i].mr_pid == 0)
						break;
				if (i == env->me_maxreaders) {
					UNLOCK_MUTEX(rmutex);
					return MDB_READERS_FULL;
				}
				r = &ti->mti_readers[i];
				/* Claim the reader slot, carefully since other code
				 * uses the reader table un-mutexed: First reset the
				 * slot, next publish it in mti_numreaders.  After
				 * that, it is safe for mdb_env_close() to touch it.
				 * When it will be closed, we can finally claim it.
				 */
				r->mr_pid = 0;
				r->mr_txnid = (txnid_t)-1;
				r->mr_tid = tid;
				if (i == nr)
					ti->mti_numreaders = ++nr;
				env->me_close_readers = nr;
				r->mr_pid = pid;
				UNLOCK_MUTEX(rmutex);

				new_notls = (env->me_flags & MDB_NOTLS);
				if (!new_notls && (rc=pthread_setspecific(env->me_txkey, r))) {
					r->mr_pid = 0;
					return rc;
				}
			}
			do /* LY: Retry on a race, ITS#7970. */
				r->mr_txnid = ti->mti_txnid;
			while(r->mr_txnid != ti->mti_txnid);
			if (!r->mr_txnid && (env->me_flags & MDB_RDONLY)) {
				meta = mdb_env_pick_meta(env);
				r->mr_txnid = meta->mm_txnid;
			} else {
				meta = env->me_metas[r->mr_txnid & 1];
			}
			txn->mt_txnid = r->mr_txnid;
			txn->mt_u.reader = r;
		}

	} else {
		/* Not yet touching txn == env->me_txn0, it may be active */
		if (ti) {
			if (LOCK_MUTEX(rc, env, env->me_wmutex))
				return rc;
			txn->mt_txnid = ti->mti_txnid;
			meta = env->me_metas[txn->mt_txnid & 1];
		} else {
			meta = mdb_env_pick_meta(env);
			txn->mt_txnid = meta->mm_txnid;
		}
		txn->mt_txnid++;
#if MDB_DEBUG
		if (txn->mt_txnid == mdb_debug_start)
			mdb_debug = MDB_DBG_INFO;
#endif
		txn->mt_child = NULL;
		txn->mt_loose_pgs = NULL;
		txn->mt_loose_count = 0;
		txn->mt_dirty_room = MDB_IDL_UM_MAX;
		txn->mt_u.dirty_list = env->me_dirty_list;
		txn->mt_u.dirty_list[0].mid = 0;
		txn->mt_free_pgs = env->me_free_pgs;
		txn->mt_free_pgs[0] = 0;
		txn->mt_spill_pgs = NULL;
		env->me_txn = txn;
		memcpy(txn->mt_dbiseqs, env->me_dbiseqs, env->me_maxdbs * sizeof(unsigned int));
	}

	/* Copy the DB info and flags */
	memcpy(txn->mt_dbs, meta->mm_dbs, CORE_DBS * sizeof(MDB_db));

	/* Moved to here to avoid a data race in read TXNs */
	txn->mt_next_pgno = meta->mm_last_pg+1;
	txn->mt_flags = flags;

	/* Setup db info */
	txn->mt_numdbs = env->me_numdbs;
	for (i=CORE_DBS; i<txn->mt_numdbs; i++) {
		x = env->me_dbflags[i];
		txn->mt_dbs[i].md_flags = x & PERSISTENT_FLAGS;
		txn->mt_dbflags[i] = (x & MDB_VALID) ? DB_VALID|DB_USRVALID|DB_STALE : 0;
	}
	txn->mt_dbflags[MAIN_DBI] = DB_VALID|DB_USRVALID;
	txn->mt_dbflags[FREE_DBI] = DB_VALID;

	if (env->me_flags & MDB_FATAL_ERROR) {
		DPUTS("environment had fatal error, must shutdown!");
		rc = MDB_PANIC;
	} else if (env->me_maxpg < txn->mt_next_pgno) {
		rc = MDB_MAP_RESIZED;
	} else {
		return MDB_SUCCESS;
	}
	mdb_txn_end(txn, new_notls /*0 or MDB_END_SLOT*/ | MDB_END_FAIL_BEGIN);
	return rc;
}

int
mdb_txn_renew(MDB_txn *txn)
{
	int rc;

	if (!txn || !F_ISSET(txn->mt_flags, MDB_TXN_RDONLY|MDB_TXN_FINISHED))
		return EINVAL;

	rc = mdb_txn_renew0(txn);
	if (rc == MDB_SUCCESS) {
		DPRINTF(("renew txn %"Yu"%c %p on mdbenv %p, root page %"Yu,
			txn->mt_txnid, (txn->mt_flags & MDB_TXN_RDONLY) ? 'r' : 'w',
			(void *)txn, (void *)txn->mt_env, txn->mt_dbs[MAIN_DBI].md_root));
	}
	return rc;
}

int
mdb_txn_begin(MDB_env *env, MDB_txn *parent, unsigned int flags, MDB_txn **ret)
{
	MDB_txn *txn;
	MDB_ntxn *ntxn;
	int rc, size, tsize;

	flags &= MDB_TXN_BEGIN_FLAGS;
	flags |= env->me_flags & MDB_WRITEMAP;

	if (env->me_flags & MDB_RDONLY & ~flags) /* write txn in RDONLY env */
		return EACCES;

	if (parent) {
		/* Nested transactions: Max 1 child, write txns only, no writemap */
		flags |= parent->mt_flags;
		if (flags & (MDB_RDONLY|MDB_WRITEMAP|MDB_TXN_BLOCKED)) {
			return (parent->mt_flags & MDB_TXN_RDONLY) ? EINVAL : MDB_BAD_TXN;
		}
		/* Child txns save MDB_pgstate and use own copy of cursors */
		size = env->me_maxdbs * (sizeof(MDB_db)+sizeof(MDB_cursor *)+1);
		size += tsize = sizeof(MDB_ntxn);
	} else if (flags & MDB_RDONLY) {
		size = env->me_maxdbs * (sizeof(MDB_db)+1);
		size += tsize = sizeof(MDB_txn);
	} else {
		/* Reuse preallocated write txn. However, do not touch it until
		 * mdb_txn_renew0() succeeds, since it currently may be active.
		 */
		txn = env->me_txn0;
		goto renew;
	}
	if ((txn = calloc(1, size)) == NULL) {
		DPRINTF(("calloc: %s", strerror(errno)));
		return ENOMEM;
	}
	txn->mt_dbxs = env->me_dbxs;	/* static */
	txn->mt_dbs = (MDB_db *) ((char *)txn + tsize);
	txn->mt_dbflags = (unsigned char *)txn + size - env->me_maxdbs;
	txn->mt_flags = flags;
	txn->mt_env = env;

	if (parent) {
		unsigned int i;
		txn->mt_cursors = (MDB_cursor **)(txn->mt_dbs + env->me_maxdbs);
		txn->mt_dbiseqs = parent->mt_dbiseqs;
		txn->mt_u.dirty_list = malloc(sizeof(MDB_ID2)*MDB_IDL_UM_SIZE);
		if (!txn->mt_u.dirty_list ||
			!(txn->mt_free_pgs = mdb_midl_alloc(MDB_IDL_UM_MAX)))
		{
			free(txn->mt_u.dirty_list);
			free(txn);
			return ENOMEM;
		}
		txn->mt_txnid = parent->mt_txnid;
		txn->mt_dirty_room = parent->mt_dirty_room;
		txn->mt_u.dirty_list[0].mid = 0;
		txn->mt_spill_pgs = NULL;
		txn->mt_next_pgno = parent->mt_next_pgno;
		parent->mt_flags |= MDB_TXN_HAS_CHILD;
		parent->mt_child = txn;
		txn->mt_parent = parent;
		txn->mt_numdbs = parent->mt_numdbs;
		memcpy(txn->mt_dbs, parent->mt_dbs, txn->mt_numdbs * sizeof(MDB_db));
		/* Copy parent's mt_dbflags, but clear DB_NEW */
		for (i=0; i<txn->mt_numdbs; i++)
			txn->mt_dbflags[i] = parent->mt_dbflags[i] & ~DB_NEW;
		rc = 0;
		ntxn = (MDB_ntxn *)txn;
		ntxn->mnt_pgstate = env->me_pgstate; /* save parent me_pghead & co */
		if (env->me_pghead) {
			size = MDB_IDL_SIZEOF(env->me_pghead);
			env->me_pghead = mdb_midl_alloc(env->me_pghead[0]);
			if (env->me_pghead)
				memcpy(env->me_pghead, ntxn->mnt_pgstate.mf_pghead, size);
			else
				rc = ENOMEM;
		}
		if (!rc)
			rc = mdb_cursor_shadow(parent, txn);
		if (rc)
			mdb_txn_end(txn, MDB_END_FAIL_BEGINCHILD);
	} else { /* MDB_RDONLY */
		txn->mt_dbiseqs = env->me_dbiseqs;
renew:
		rc = mdb_txn_renew0(txn);
	}
	if (rc) {
		if (txn != env->me_txn0) {
			free(txn);
		}
	} else {
		txn->mt_flags |= flags;	/* could not change txn=me_txn0 earlier */
		*ret = txn;
		DPRINTF(("begin txn %"Yu"%c %p on mdbenv %p, root page %"Yu,
			txn->mt_txnid, (flags & MDB_RDONLY) ? 'r' : 'w',
			(void *) txn, (void *) env, txn->mt_dbs[MAIN_DBI].md_root));
	}
	MDB_TRACE(("%p, %p, %u = %p", env, parent, flags, txn));

	return rc;
}

MDB_env *
mdb_txn_env(MDB_txn *txn)
{
	if(!txn) return NULL;
	return txn->mt_env;
}

mdb_size_t
mdb_txn_id(MDB_txn *txn)
{
    if(!txn) return 0;
    return txn->mt_txnid;
}

/** Export or close DBI handles opened in this txn. */
void
mdb_dbis_update(MDB_txn *txn, int keep)
{
	int i;
	MDB_dbi n = txn->mt_numdbs;
	MDB_env *env = txn->mt_env;
	unsigned char *tdbflags = txn->mt_dbflags;

	for (i = n; --i >= CORE_DBS;) {
		if (tdbflags[i] & DB_NEW) {
			if (keep) {
				env->me_dbflags[i] = txn->mt_dbs[i].md_flags | MDB_VALID;
			} else {
				char *ptr = env->me_dbxs[i].md_name.mv_data;
				if (ptr) {
					env->me_dbxs[i].md_name.mv_data = NULL;
					env->me_dbxs[i].md_name.mv_size = 0;
					env->me_dbflags[i] = 0;
					env->me_dbiseqs[i]++;
					free(ptr);
				}
			}
		}
	}
	if (keep && env->me_numdbs < n)
		env->me_numdbs = n;
}

/** End a transaction, except successful commit of a nested transaction.
 * May be called twice for readonly txns: First reset it, then abort.
 * @param[in] txn the transaction handle to end
 * @param[in] mode why and how to end the transaction
 */
void
mdb_txn_end(MDB_txn *txn, unsigned mode)
{
	MDB_env	*env = txn->mt_env;
#if MDB_DEBUG
	static const char *const names[] = MDB_END_NAMES;
#endif

	/* Export or close DBI handles opened in this txn */
	mdb_dbis_update(txn, mode & MDB_END_UPDATE);

	DPRINTF(("%s txn %"Yu"%c %p on mdbenv %p, root page %"Yu,
		names[mode & MDB_END_OPMASK],
		txn->mt_txnid, (txn->mt_flags & MDB_TXN_RDONLY) ? 'r' : 'w',
		(void *) txn, (void *)env, txn->mt_dbs[MAIN_DBI].md_root));

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY)) {
		if (txn->mt_u.reader) {
			txn->mt_u.reader->mr_txnid = (txnid_t)-1;
			if (!(env->me_flags & MDB_NOTLS)) {
				txn->mt_u.reader = NULL; /* txn does not own reader */
			} else if (mode & MDB_END_SLOT) {
				txn->mt_u.reader->mr_pid = 0;
				txn->mt_u.reader = NULL;
			} /* else txn owns the slot until it does MDB_END_SLOT */
		}
		txn->mt_numdbs = 0;		/* prevent further DBI activity */
		txn->mt_flags |= MDB_TXN_FINISHED;

	} else if (!F_ISSET(txn->mt_flags, MDB_TXN_FINISHED)) {
		pgno_t *pghead = env->me_pghead;

		if (!(mode & MDB_END_UPDATE)) /* !(already closed cursors) */
			mdb_cursors_close(txn, 0);
		if (!(env->me_flags & MDB_WRITEMAP)) {
			mdb_dlist_free(txn);
		}

		txn->mt_numdbs = 0;
		txn->mt_flags = MDB_TXN_FINISHED;

		if (!txn->mt_parent) {
			mdb_midl_shrink(&txn->mt_free_pgs);
			env->me_free_pgs = txn->mt_free_pgs;
			/* me_pgstate: */
			env->me_pghead = NULL;
			env->me_pglast = 0;

			env->me_txn = NULL;
			mode = 0;	/* txn == env->me_txn0, do not free() it */

			/* The writer mutex was locked in mdb_txn_begin. */
			if (env->me_txns)
				UNLOCK_MUTEX(env->me_wmutex);
		} else {
			txn->mt_parent->mt_child = NULL;
			txn->mt_parent->mt_flags &= ~MDB_TXN_HAS_CHILD;
			env->me_pgstate = ((MDB_ntxn *)txn)->mnt_pgstate;
			mdb_midl_free(txn->mt_free_pgs);
			free(txn->mt_u.dirty_list);
		}
		mdb_midl_free(txn->mt_spill_pgs);

		mdb_midl_free(pghead);
	}
	if (mode & MDB_END_FREE)
		free(txn);
}

void
mdb_txn_reset(MDB_txn *txn)
{
	if (txn == NULL)
		return;

	/* This call is only valid for read-only txns */
	if (!(txn->mt_flags & MDB_TXN_RDONLY))
		return;

	mdb_txn_end(txn, MDB_END_RESET);
}

void
_mdb_txn_abort(MDB_txn *txn)
{
	if (txn == NULL)
		return;

	if (txn->mt_child)
		_mdb_txn_abort(txn->mt_child);

	mdb_txn_end(txn, MDB_END_ABORT|MDB_END_SLOT|MDB_END_FREE);
}

void
mdb_txn_abort(MDB_txn *txn)
{
	MDB_TRACE(("%p", txn));
	_mdb_txn_abort(txn);
}

/** Save the freelist as of this transaction to the freeDB.
 * This changes the freelist. Keep trying until it stabilizes.
 *
 * When (MDB_DEVEL) & 2, the changes do not affect #mdb_page_alloc(),
 * it then uses the transaction's original snapshot of the freeDB.
 */
int
mdb_freelist_save(MDB_txn *txn)
{
	/* env->me_pghead[] can grow and shrink during this call.
	 * env->me_pglast and txn->mt_free_pgs[] can only grow.
	 * Page numbers cannot disappear from txn->mt_free_pgs[].
	 */
	MDB_cursor mc;
	MDB_env	*env = txn->mt_env;
	int rc, maxfree_1pg = env->me_maxfree_1pg, more = 1;
	txnid_t	pglast = 0, head_id = 0;
	pgno_t	freecnt = 0, *free_pgs, *mop;
	ssize_t	head_room = 0, total_room = 0, mop_len, clean_limit;

	mdb_cursor_init(&mc, txn, FREE_DBI, NULL);

	if (env->me_pghead) {
		/* Make sure first page of freeDB is touched and on freelist */
		rc = mdb_page_search(&mc, NULL, MDB_PS_FIRST|MDB_PS_MODIFY);
		if (rc && rc != MDB_NOTFOUND)
			return rc;
	}

	if (!env->me_pghead && txn->mt_loose_pgs) {
		/* Put loose page numbers in mt_free_pgs, since
		 * we may be unable to return them to me_pghead.
		 */
		MDB_page *mp = txn->mt_loose_pgs;
		MDB_ID2 *dl = txn->mt_u.dirty_list;
		unsigned x;
		if ((rc = mdb_midl_need(&txn->mt_free_pgs, txn->mt_loose_count)) != 0)
			return rc;
		for (; mp; mp = NEXT_LOOSE_PAGE(mp)) {
			mdb_midl_xappend(txn->mt_free_pgs, mp->mp_pgno);
			/* must also remove from dirty list */
			if (txn->mt_flags & MDB_TXN_WRITEMAP) {
				for (x=1; x<=dl[0].mid; x++)
					if (dl[x].mid == mp->mp_pgno)
						break;
				mdb_tassert(txn, x <= dl[0].mid);
			} else {
				x = mdb_mid2l_search(dl, mp->mp_pgno);
				mdb_tassert(txn, dl[x].mid == mp->mp_pgno);
				mdb_dpage_free(env, mp);
			}
			dl[x].mptr = NULL;
		}
		{
			/* squash freed slots out of the dirty list */
			unsigned y;
			for (y=1; dl[y].mptr && y <= dl[0].mid; y++);
			if (y <= dl[0].mid) {
				for(x=y, y++;;) {
					while (!dl[y].mptr && y <= dl[0].mid) y++;
					if (y > dl[0].mid) break;
					dl[x++] = dl[y++];
				}
				dl[0].mid = x-1;
			} else {
				/* all slots freed */
				dl[0].mid = 0;
			}
		}
		txn->mt_loose_pgs = NULL;
		txn->mt_loose_count = 0;
	}

	/* MDB_RESERVE cancels meminit in ovpage malloc (when no WRITEMAP) */
	clean_limit = (env->me_flags & (MDB_NOMEMINIT|MDB_WRITEMAP))
		? SSIZE_MAX : maxfree_1pg;

	for (;;) {
		/* Come back here after each Put() in case freelist changed */
		MDB_val key, data;
		pgno_t *pgs;
		ssize_t j;

		/* If using records from freeDB which we have not yet
		 * deleted, delete them and any we reserved for me_pghead.
		 */
		while (pglast < env->me_pglast) {
			rc = mdb_cursor_first(&mc, &key, NULL);
			if (rc)
				return rc;
			pglast = head_id = *(txnid_t *)key.mv_data;
			total_room = head_room = 0;
			mdb_tassert(txn, pglast <= env->me_pglast);
			rc = _mdb_cursor_del(&mc, 0);
			if (rc)
				return rc;
		}

		/* Save the IDL of pages freed by this txn, to a single record */
		if (freecnt < txn->mt_free_pgs[0]) {
			if (!freecnt) {
				/* Make sure last page of freeDB is touched and on freelist */
				rc = mdb_page_search(&mc, NULL, MDB_PS_LAST|MDB_PS_MODIFY);
				if (rc && rc != MDB_NOTFOUND)
					return rc;
			}
			free_pgs = txn->mt_free_pgs;
			/* Write to last page of freeDB */
			key.mv_size = sizeof(txn->mt_txnid);
			key.mv_data = &txn->mt_txnid;
			do {
				freecnt = free_pgs[0];
				data.mv_size = MDB_IDL_SIZEOF(free_pgs);
				rc = _mdb_cursor_put(&mc, &key, &data, MDB_RESERVE);
				if (rc)
					return rc;
				/* Retry if mt_free_pgs[] grew during the Put() */
				free_pgs = txn->mt_free_pgs;
			} while (freecnt < free_pgs[0]);
			mdb_midl_sort(free_pgs);
			memcpy(data.mv_data, free_pgs, data.mv_size);
#if (MDB_DEBUG) > 1
			{
				unsigned int i = free_pgs[0];
				DPRINTF(("IDL write txn %"Yu" root %"Yu" num %u",
					txn->mt_txnid, txn->mt_dbs[FREE_DBI].md_root, i));
				for (; i; i--)
					DPRINTF(("IDL %"Yu, free_pgs[i]));
			}
#endif
			continue;
		}

		mop = env->me_pghead;
		mop_len = (mop ? mop[0] : 0) + txn->mt_loose_count;

		/* Reserve records for me_pghead[]. Split it if multi-page,
		 * to avoid searching freeDB for a page range. Use keys in
		 * range [1,me_pglast]: Smaller than txnid of oldest reader.
		 */
		if (total_room >= mop_len) {
			if (total_room == mop_len || --more < 0)
				break;
		} else if (head_room >= maxfree_1pg && head_id > 1) {
			/* Keep current record (overflow page), add a new one */
			head_id--;
			head_room = 0;
		}
		/* (Re)write {key = head_id, IDL length = head_room} */
		total_room -= head_room;
		head_room = mop_len - total_room;
		if (head_room > maxfree_1pg && head_id > 1) {
			/* Overflow multi-page for part of me_pghead */
			head_room /= head_id; /* amortize page sizes */
			head_room += maxfree_1pg - head_room % (maxfree_1pg + 1);
		} else if (head_room < 0) {
			/* Rare case, not bothering to delete this record */
			head_room = 0;
		}
		key.mv_size = sizeof(head_id);
		key.mv_data = &head_id;
		data.mv_size = (head_room + 1) * sizeof(pgno_t);
		rc = _mdb_cursor_put(&mc, &key, &data, MDB_RESERVE);
		if (rc)
			return rc;
		/* IDL is initially empty, zero out at least the length */
		pgs = (pgno_t *)data.mv_data;
		j = head_room > clean_limit ? head_room : 0;
		do {
			pgs[j] = 0;
		} while (--j >= 0);
		total_room += head_room;
	}

	/* Return loose page numbers to me_pghead, though usually none are
	 * left at this point.  The pages themselves remain in dirty_list.
	 */
	if (txn->mt_loose_pgs) {
		MDB_page *mp = txn->mt_loose_pgs;
		unsigned count = txn->mt_loose_count;
		MDB_IDL loose;
		/* Room for loose pages + temp IDL with same */
		if ((rc = mdb_midl_need(&env->me_pghead, 2*count+1)) != 0)
			return rc;
		mop = env->me_pghead;
		loose = mop + MDB_IDL_ALLOCLEN(mop) - count;
		for (count = 0; mp; mp = NEXT_LOOSE_PAGE(mp))
			loose[ ++count ] = mp->mp_pgno;
		loose[0] = count;
		mdb_midl_sort(loose);
		mdb_midl_xmerge(mop, loose);
		txn->mt_loose_pgs = NULL;
		txn->mt_loose_count = 0;
		mop_len = mop[0];
	}

	/* Fill in the reserved me_pghead records */
	rc = MDB_SUCCESS;
	if (mop_len) {
		MDB_val key, data;

		mop += mop_len;
		rc = mdb_cursor_first(&mc, &key, &data);
		for (; !rc; rc = mdb_cursor_next(&mc, &key, &data, MDB_NEXT)) {
			txnid_t id = *(txnid_t *)key.mv_data;
			ssize_t	len = (ssize_t)(data.mv_size / sizeof(MDB_ID)) - 1;
			MDB_ID save;

			mdb_tassert(txn, len >= 0 && id <= env->me_pglast);
			key.mv_data = &id;
			if (len > mop_len) {
				len = mop_len;
				data.mv_size = (len + 1) * sizeof(MDB_ID);
			}
			data.mv_data = mop -= len;
			save = mop[0];
			mop[0] = len;
			rc = _mdb_cursor_put(&mc, &key, &data, MDB_CURRENT);
			mop[0] = save;
			if (rc || !(mop_len -= len))
				break;
		}
	}
	return rc;
}

int ESECT mdb_env_share_locks(MDB_env *env, int *excl);

int
_mdb_txn_commit(MDB_txn *txn)
{
	int		rc;
	unsigned int i, end_mode;
	MDB_env	*env;

	if (txn == NULL)
		return EINVAL;

	/* mdb_txn_end() mode for a commit which writes nothing */
	end_mode = MDB_END_EMPTY_COMMIT|MDB_END_UPDATE|MDB_END_SLOT|MDB_END_FREE;

	if (txn->mt_child) {
		rc = _mdb_txn_commit(txn->mt_child);
		if (rc)
			goto fail;
	}

	env = txn->mt_env;

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY)) {
		goto done;
	}

	if (txn->mt_flags & (MDB_TXN_FINISHED|MDB_TXN_ERROR)) {
		DPUTS("txn has failed/finished, can't commit");
		if (txn->mt_parent)
			txn->mt_parent->mt_flags |= MDB_TXN_ERROR;
		rc = MDB_BAD_TXN;
		goto fail;
	}

	if (txn->mt_parent) {
		MDB_txn *parent = txn->mt_parent;
		MDB_page **lp;
		MDB_ID2L dst, src;
		MDB_IDL pspill;
		unsigned x, y, len, ps_len;

		/* Append our free list to parent's */
		rc = mdb_midl_append_list(&parent->mt_free_pgs, txn->mt_free_pgs);
		if (rc)
			goto fail;
		mdb_midl_free(txn->mt_free_pgs);
		/* Failures after this must either undo the changes
		 * to the parent or set MDB_TXN_ERROR in the parent.
		 */

		parent->mt_next_pgno = txn->mt_next_pgno;
		parent->mt_flags = txn->mt_flags;

		/* Merge our cursors into parent's and close them */
		mdb_cursors_close(txn, 1);

		/* Update parent's DB table. */
		memcpy(parent->mt_dbs, txn->mt_dbs, txn->mt_numdbs * sizeof(MDB_db));
		parent->mt_numdbs = txn->mt_numdbs;
		parent->mt_dbflags[FREE_DBI] = txn->mt_dbflags[FREE_DBI];
		parent->mt_dbflags[MAIN_DBI] = txn->mt_dbflags[MAIN_DBI];
		for (i=CORE_DBS; i<txn->mt_numdbs; i++) {
			/* preserve parent's DB_NEW status */
			x = parent->mt_dbflags[i] & DB_NEW;
			parent->mt_dbflags[i] = txn->mt_dbflags[i] | x;
		}

		dst = parent->mt_u.dirty_list;
		src = txn->mt_u.dirty_list;
		/* Remove anything in our dirty list from parent's spill list */
		if ((pspill = parent->mt_spill_pgs) && (ps_len = pspill[0])) {
			x = y = ps_len;
			pspill[0] = (pgno_t)-1;
			/* Mark our dirty pages as deleted in parent spill list */
			for (i=0, len=src[0].mid; ++i <= len; ) {
				MDB_ID pn = src[i].mid << 1;
				while (pn > pspill[x])
					x--;
				if (pn == pspill[x]) {
					pspill[x] = 1;
					y = --x;
				}
			}
			/* Squash deleted pagenums if we deleted any */
			for (x=y; ++x <= ps_len; )
				if (!(pspill[x] & 1))
					pspill[++y] = pspill[x];
			pspill[0] = y;
		}

		/* Remove anything in our spill list from parent's dirty list */
		if (txn->mt_spill_pgs && txn->mt_spill_pgs[0]) {
			for (i=1; i<=txn->mt_spill_pgs[0]; i++) {
				MDB_ID pn = txn->mt_spill_pgs[i];
				if (pn & 1)
					continue;	/* deleted spillpg */
				pn >>= 1;
				y = mdb_mid2l_search(dst, pn);
				if (y <= dst[0].mid && dst[y].mid == pn) {
					free(dst[y].mptr);
					while (y < dst[0].mid) {
						dst[y] = dst[y+1];
						y++;
					}
					dst[0].mid--;
				}
			}
		}

		/* Find len = length of merging our dirty list with parent's */
		x = dst[0].mid;
		dst[0].mid = 0;		/* simplify loops */
		if (parent->mt_parent) {
			len = x + src[0].mid;
			y = mdb_mid2l_search(src, dst[x].mid + 1) - 1;
			for (i = x; y && i; y--) {
				pgno_t yp = src[y].mid;
				while (yp < dst[i].mid)
					i--;
				if (yp == dst[i].mid) {
					i--;
					len--;
				}
			}
		} else { /* Simplify the above for single-ancestor case */
			len = MDB_IDL_UM_MAX - txn->mt_dirty_room;
		}
		/* Merge our dirty list with parent's */
		y = src[0].mid;
		for (i = len; y; dst[i--] = src[y--]) {
			pgno_t yp = src[y].mid;
			while (yp < dst[x].mid)
				dst[i--] = dst[x--];
			if (yp == dst[x].mid)
				free(dst[x--].mptr);
		}
		mdb_tassert(txn, i == x);
		dst[0].mid = len;
		free(txn->mt_u.dirty_list);
		parent->mt_dirty_room = txn->mt_dirty_room;
		if (txn->mt_spill_pgs) {
			if (parent->mt_spill_pgs) {
				/* TODO: Prevent failure here, so parent does not fail */
				rc = mdb_midl_append_list(&parent->mt_spill_pgs, txn->mt_spill_pgs);
				if (rc)
					parent->mt_flags |= MDB_TXN_ERROR;
				mdb_midl_free(txn->mt_spill_pgs);
				mdb_midl_sort(parent->mt_spill_pgs);
			} else {
				parent->mt_spill_pgs = txn->mt_spill_pgs;
			}
		}

		/* Append our loose page list to parent's */
		for (lp = &parent->mt_loose_pgs; *lp; lp = &NEXT_LOOSE_PAGE(*lp))
			;
		*lp = txn->mt_loose_pgs;
		parent->mt_loose_count += txn->mt_loose_count;

		parent->mt_child = NULL;
		mdb_midl_free(((MDB_ntxn *)txn)->mnt_pgstate.mf_pghead);
		free(txn);
		return rc;
	}

	if (txn != env->me_txn) {
		DPUTS("attempt to commit unknown transaction");
		rc = EINVAL;
		goto fail;
	}

	mdb_cursors_close(txn, 0);

	if (!txn->mt_u.dirty_list[0].mid &&
		!(txn->mt_flags & (MDB_TXN_DIRTY|MDB_TXN_SPILLS)))
		goto done;

	DPRINTF(("committing txn %"Yu" %p on mdbenv %p, root page %"Yu,
	    txn->mt_txnid, (void*)txn, (void*)env, txn->mt_dbs[MAIN_DBI].md_root));

	/* Update DB root pointers */
	if (txn->mt_numdbs > CORE_DBS) {
		MDB_cursor mc;
		MDB_dbi i;
		MDB_val data;
		data.mv_size = sizeof(MDB_db);

		mdb_cursor_init(&mc, txn, MAIN_DBI, NULL);
		for (i = CORE_DBS; i < txn->mt_numdbs; i++) {
			if (txn->mt_dbflags[i] & DB_DIRTY) {
				if (TXN_DBI_CHANGED(txn, i)) {
					rc = MDB_BAD_DBI;
					goto fail;
				}
				data.mv_data = &txn->mt_dbs[i];
				rc = _mdb_cursor_put(&mc, &txn->mt_dbxs[i].md_name, &data,
					F_SUBDATA);
				if (rc)
					goto fail;
			}
		}
	}

	rc = mdb_freelist_save(txn);
	if (rc)
		goto fail;

	mdb_midl_free(env->me_pghead);
	env->me_pghead = NULL;
	mdb_midl_shrink(&txn->mt_free_pgs);

#if (MDB_DEBUG) > 2
	mdb_audit(txn);
#endif

	if ((rc = mdb_page_flush(txn, 0)))
		goto fail;
	if (!F_ISSET(txn->mt_flags, MDB_TXN_NOSYNC) &&
		(rc = mdb_env_sync0(env, 0, txn->mt_next_pgno)))
		goto fail;
	if ((rc = mdb_env_write_meta(txn)))
		goto fail;
	end_mode = MDB_END_COMMITTED|MDB_END_UPDATE;
	if (env->me_flags & MDB_PREVSNAPSHOT) {
		if (!(env->me_flags & MDB_NOLOCK)) {
			int excl;
			rc = mdb_env_share_locks(env, &excl);
			if (rc)
				goto fail;
		}
		env->me_flags ^= MDB_PREVSNAPSHOT;
	}

done:
	mdb_txn_end(txn, end_mode);
	return MDB_SUCCESS;

fail:
	_mdb_txn_abort(txn);
	return rc;
}

int
mdb_txn_commit(MDB_txn *txn)
{
	MDB_TRACE(("%p", txn));
	return _mdb_txn_commit(txn);
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
			DPRINTF(("page %"Yu" not a meta page", p->mp_pgno));
			return MDB_INVALID;
		}

		m = METADATA(p);
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

	p = calloc(NUM_METAS, psize);
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
	DPRINTF(("writing meta page %d for root page %"Yu,
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
	/* MIPS has cache coherency issues, this is a no-op everywhere else */
	CACHEFLUSH(env->me_map + off, len, DCACHE);
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

	e = calloc(1, sizeof(MDB_env));
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
DWORD
mdb_nt2win32(NTSTATUS st)
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
	env->me_map = map;
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
	env->me_map = mmap(addr, env->me_mapsize, prot, mmap_flags,
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
	env->me_metas[0] = METADATA(p);
	env->me_metas[1] = (MDB_meta *)((char *)env->me_metas[0] + env->me_psize);

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
	MDB_TRACE(("%p, %"Yu"", env, size));
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

int ESECT
mdb_fsize(HANDLE fd, mdb_size_t *size)
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

/** Filename - string of #mdb_nchar_t[] */
typedef struct MDB_name {
	int mn_len;					/**< Length  */
	int mn_alloced;				/**< True if #mn_val was malloced */
	mdb_nchar_t	*mn_val;		/**< Contents */
} MDB_name;

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

/** Destroy \b fname from #mdb_fname_init() */
#define mdb_fname_destroy(fname) \
	do { if ((fname).mn_alloced) free((fname).mn_val); } while (0)

#ifdef O_CLOEXEC /* POSIX.1-2008: Set FD_CLOEXEC atomically at open() */
# define MDB_CLOEXEC		O_CLOEXEC
#else
# define MDB_CLOEXEC		0
#endif

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
		DPRINTF(("entries: %"Yu,        db->md_entries));
		DPRINTF(("branch pages: %"Yu,   db->md_branch_pages));
		DPRINTF(("leaf pages: %"Yu,     db->md_leaf_pages));
		DPRINTF(("overflow pages: %"Yu, db->md_overflow_pages));
		DPRINTF(("root: %"Yu,           db->md_root));
	}
#endif

	return MDB_SUCCESS;
}


/** Release a reader thread's slot in the reader lock table.
 *	This function is called automatically when a thread exits.
 * @param[in] ptr This points to the slot in the reader lock table.
 */
void
mdb_env_reader_dest(void *ptr)
{
	MDB_reader *reader = ptr;

#ifndef _WIN32
	if (reader->mr_pid == getpid()) /* catch pthread_exit() in child process */
#endif
		/* We omit the mutex, so do this atomically (i.e. skip mr_txnid) */
		reader->mr_pid = 0;
}

#ifdef _WIN32
/** Junk for arranging thread-specific callbacks on Windows. This is
 *	necessarily platform and compiler-specific. Windows supports up
 *	to 1088 keys. Let's assume nobody opens more than 64 environments
 *	in a single process, for now. They can override this if needed.
 */
#ifndef MAX_TLS_KEYS
#define MAX_TLS_KEYS	64
#endif
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
		env->me_txns = MapViewOfFileEx(mh, FILE_MAP_WRITE, 0, 0, rsize, NULL);
		CloseHandle(mh);
		if (!env->me_txns) goto fail_errno;
#else
		void *m = mmap(NULL, rsize, PROT_READ|PROT_WRITE, MAP_SHARED,
			env->me_lfd, 0);
		if (m == MAP_FAILED) goto fail_errno;
		env->me_txns = m;
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
#ifdef MDB_ROBUST_SUPPORTED
		if (!rc) rc = pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
#endif
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
			  (env->me_dirty_list = calloc(MDB_IDL_UM_SIZE, sizeof(MDB_ID2)))))
			rc = ENOMEM;
	}

	env->me_flags = flags;
	if (rc)
		goto leave;

	env->me_path = strdup(path);
	env->me_dbxs = calloc(env->me_maxdbs, sizeof(MDB_dbx));
	env->me_dbflags = calloc(env->me_maxdbs, sizeof(uint16_t));
	env->me_dbiseqs = calloc(env->me_maxdbs, sizeof(unsigned int));
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
				(txn = calloc(1, size)))
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

/** Compare two items pointing at aligned #mdb_size_t's */
int
mdb_cmp_long(const MDB_val *a, const MDB_val *b)
{
	return (*(mdb_size_t *)a->mv_data < *(mdb_size_t *)b->mv_data) ? -1 :
		*(mdb_size_t *)a->mv_data > *(mdb_size_t *)b->mv_data;
}

/** Compare two items pointing at aligned unsigned int's.
 *
 *	This is also set as #MDB_INTEGERDUP|#MDB_DUPFIXED's #MDB_dbx.%md_dcmp,
 *	but #mdb_cmp_clong() is called instead if the data type is #mdb_size_t.
 */
int
mdb_cmp_int(const MDB_val *a, const MDB_val *b)
{
	return (*(unsigned int *)a->mv_data < *(unsigned int *)b->mv_data) ? -1 :
		*(unsigned int *)a->mv_data > *(unsigned int *)b->mv_data;
}

/** Compare two items pointing at unsigned ints of unknown alignment.
 *	Nodes and keys are guaranteed to be 2-byte aligned.
 */
int
mdb_cmp_cint(const MDB_val *a, const MDB_val *b)
{
#if BYTE_ORDER == LITTLE_ENDIAN
	unsigned short *u, *c;
	int x;

	u = (unsigned short *) ((char *) a->mv_data + a->mv_size);
	c = (unsigned short *) ((char *) b->mv_data + a->mv_size);
	do {
		x = *--u - *--c;
	} while(!x && u > (unsigned short *)a->mv_data);
	return x;
#else
	unsigned short *u, *c, *end;
	int x;

	end = (unsigned short *) ((char *) a->mv_data + a->mv_size);
	u = (unsigned short *)a->mv_data;
	c = (unsigned short *)b->mv_data;
	do {
		x = *u++ - *c++;
	} while(!x && u < end);
	return x;
#endif
}

/** Compare two items lexically */
int
mdb_cmp_memn(const MDB_val *a, const MDB_val *b)
{
	int diff;
	ssize_t len_diff;
	unsigned int len;

	len = a->mv_size;
	len_diff = (ssize_t) a->mv_size - (ssize_t) b->mv_size;
	if (len_diff > 0) {
		len = b->mv_size;
		len_diff = 1;
	}

	diff = memcmp(a->mv_data, b->mv_data, len);
	return diff ? diff : len_diff<0 ? -1 : len_diff;
}

/** Compare two items in reverse byte order */
int
mdb_cmp_memnr(const MDB_val *a, const MDB_val *b)
{
	const unsigned char	*p1, *p2, *p1_lim;
	ssize_t len_diff;
	int diff;

	p1_lim = (const unsigned char *)a->mv_data;
	p1 = (const unsigned char *)a->mv_data + a->mv_size;
	p2 = (const unsigned char *)b->mv_data + b->mv_size;

	len_diff = (ssize_t) a->mv_size - (ssize_t) b->mv_size;
	if (len_diff > 0) {
		p1_lim += len_diff;
		len_diff = 1;
	}

	while (p1 > p1_lim) {
		diff = *--p1 - *--p2;
		if (diff)
			return diff;
	}
	return len_diff<0 ? -1 : len_diff;
}

/** Search for key within a page, using binary search.
 * Returns the smallest entry larger or equal to the key.
 * If exactp is non-null, stores whether the found entry was an exact match
 * in *exactp (1 or 0).
 * Updates the cursor index with the index of the found entry.
 * If no entry larger or equal to the key is found, returns NULL.
 */
MDB_node *
mdb_node_search(MDB_cursor *mc, MDB_val *key, int *exactp)
{
	unsigned int	 i = 0, nkeys;
	int		 low, high;
	int		 rc = 0;
	MDB_page *mp = mc->mc_pg[mc->mc_top];
	MDB_node	*node = NULL;
	MDB_val	 nodekey;
	MDB_cmp_func *cmp;
	DKBUF;

	nkeys = NUMKEYS(mp);

	DPRINTF(("searching %u keys in %s %spage %"Yu,
	    nkeys, IS_LEAF(mp) ? "leaf" : "branch", IS_SUBP(mp) ? "sub-" : "",
	    mdb_dbg_pgno(mp)));

	low = IS_LEAF(mp) ? 0 : 1;
	high = nkeys - 1;
	cmp = mc->mc_dbx->md_cmp;

	/* Branch pages have no data, so if using integer keys,
	 * alignment is guaranteed. Use faster mdb_cmp_int.
	 */
	if (cmp == mdb_cmp_cint && IS_BRANCH(mp)) {
		if (NODEPTR(mp, 1)->mn_ksize == sizeof(mdb_size_t))
			cmp = mdb_cmp_long;
		else
			cmp = mdb_cmp_int;
	}

	if (IS_LEAF2(mp)) {
		nodekey.mv_size = mc->mc_db->md_pad;
		node = NODEPTR(mp, 0);	/* fake */
		while (low <= high) {
			i = (low + high) >> 1;
			nodekey.mv_data = LEAF2KEY(mp, i, nodekey.mv_size);
			rc = cmp(key, &nodekey);
			DPRINTF(("found leaf index %u [%s], rc = %i",
			    i, DKEY(&nodekey), rc));
			if (rc == 0)
				break;
			if (rc > 0)
				low = i + 1;
			else
				high = i - 1;
		}
	} else {
		while (low <= high) {
			i = (low + high) >> 1;

			node = NODEPTR(mp, i);
			nodekey.mv_size = NODEKSZ(node);
			nodekey.mv_data = NODEKEY(node);

			rc = cmp(key, &nodekey);
#if MDB_DEBUG
			if (IS_LEAF(mp))
				DPRINTF(("found leaf index %u [%s], rc = %i",
				    i, DKEY(&nodekey), rc));
			else
				DPRINTF(("found branch index %u [%s -> %"Yu"], rc = %i",
				    i, DKEY(&nodekey), NODEPGNO(node), rc));
#endif
			if (rc == 0)
				break;
			if (rc > 0)
				low = i + 1;
			else
				high = i - 1;
		}
	}

	if (rc > 0) {	/* Found entry is less than the key. */
		i++;	/* Skip to get the smallest entry larger than key. */
		if (!IS_LEAF2(mp))
			node = NODEPTR(mp, i);
	}
	if (exactp)
		*exactp = (rc == 0 && nkeys > 0);
	/* store the key index */
	mc->mc_ki[mc->mc_top] = i;
	if (i >= nkeys)
		/* There is no entry larger or equal to the key. */
		return NULL;

	/* nodeptr is fake for LEAF2 */
	return node;
}

/** Pop a page off the top of the cursor's stack. */
void
mdb_cursor_pop(MDB_cursor *mc)
{
	if (mc->mc_snum) {
		DPRINTF(("popping page %"Yu" off db %d cursor %p",
			mc->mc_pg[mc->mc_top]->mp_pgno, DDBI(mc), (void *) mc));

		mc->mc_snum--;
		if (mc->mc_snum) {
			mc->mc_top--;
		} else {
			mc->mc_flags &= ~C_INITIALIZED;
		}
	}
}

/** Push a page onto the top of the cursor's stack.
 * Set #MDB_TXN_ERROR on failure.
 */
int
mdb_cursor_push(MDB_cursor *mc, MDB_page *mp)
{
	DPRINTF(("pushing page %"Yu" on db %d cursor %p", mp->mp_pgno,
		DDBI(mc), (void *) mc));

	if (mc->mc_snum >= CURSOR_STACK) {
		mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
		return MDB_CURSOR_FULL;
	}

	mc->mc_top = mc->mc_snum++;
	mc->mc_pg[mc->mc_top] = mp;
	mc->mc_ki[mc->mc_top] = 0;

	return MDB_SUCCESS;
}

/** Return the data associated with a given node.
 * @param[in] mc The cursor for this operation.
 * @param[in] leaf The node being read.
 * @param[out] data Updated to point to the node's data.
 * @return 0 on success, non-zero on failure.
 */
int
mdb_node_read(MDB_cursor *mc, MDB_node *leaf, MDB_val *data)
{
	MDB_page	*omp;		/* overflow page */
	pgno_t		 pgno;
	int rc;

	if (MC_OVPG(mc)) {
		MDB_PAGE_UNREF(mc->mc_txn, MC_OVPG(mc));
		MC_SET_OVPG(mc, NULL);
	}
	if (!F_ISSET(leaf->mn_flags, F_BIGDATA)) {
		data->mv_size = NODEDSZ(leaf);
		data->mv_data = NODEDATA(leaf);
		return MDB_SUCCESS;
	}

	/* Read overflow data.
	 */
	data->mv_size = NODEDSZ(leaf);
	memcpy(&pgno, NODEDATA(leaf), sizeof(pgno));
	if ((rc = mdb_page_get(mc, pgno, &omp, NULL)) != 0) {
		DPRINTF(("read overflow page %"Yu" failed", pgno));
		return rc;
	}
	data->mv_data = METADATA(omp);
	MC_SET_OVPG(mc, omp);

	return MDB_SUCCESS;
}

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

/** Find a sibling for a page.
 * Replaces the page at the top of the cursor's stack with the
 * specified sibling, if one exists.
 * @param[in] mc The cursor for this operation.
 * @param[in] move_right Non-zero if the right sibling is requested,
 * otherwise the left sibling.
 * @return 0 on success, non-zero on failure.
 */
int
mdb_cursor_sibling(MDB_cursor *mc, int move_right)
{
	int		 rc;
	MDB_node	*indx;
	MDB_page	*mp;
	if (mc->mc_snum < 2) {
		return MDB_NOTFOUND;		/* root has no siblings */
	}

	mdb_cursor_pop(mc);
	DPRINTF(("parent page is page %"Yu", index %u",
		mc->mc_pg[mc->mc_top]->mp_pgno, mc->mc_ki[mc->mc_top]));

	if (move_right ? (mc->mc_ki[mc->mc_top] + 1u >= NUMKEYS(mc->mc_pg[mc->mc_top]))
		       : (mc->mc_ki[mc->mc_top] == 0)) {
		DPRINTF(("no more keys left, moving to %s sibling",
		    move_right ? "right" : "left"));
		if ((rc = mdb_cursor_sibling(mc, move_right)) != MDB_SUCCESS) {
			/* undo cursor_pop before returning */
			mc->mc_top++;
			mc->mc_snum++;
			return rc;
		}
	} else {
		if (move_right)
			mc->mc_ki[mc->mc_top]++;
		else
			mc->mc_ki[mc->mc_top]--;
		DPRINTF(("just moving to %s index key %u",
		    move_right ? "right" : "left", mc->mc_ki[mc->mc_top]));
	}
	mdb_cassert(mc, IS_BRANCH(mc->mc_pg[mc->mc_top]));

	MDB_PAGE_UNREF(mc->mc_txn, op);

	indx = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
	if ((rc = mdb_page_get(mc, NODEPGNO(indx), &mp, NULL)) != 0) {
		/* mc will be inconsistent if caller does mc_snum++ as above */
		mc->mc_flags &= ~(C_INITIALIZED|C_EOF);
		return rc;
	}

	mdb_cursor_push(mc, mp);
	if (!move_right)
		mc->mc_ki[mc->mc_top] = NUMKEYS(mp)-1;

	return MDB_SUCCESS;
}

/** Move the cursor to the next data item. */
int
mdb_cursor_next(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op)
{
	MDB_page	*mp;
	MDB_node	*leaf;
	int rc;

	if ((mc->mc_flags & C_DEL && op == MDB_NEXT_DUP))
		return MDB_NOTFOUND;

	if (!(mc->mc_flags & C_INITIALIZED))
		return mdb_cursor_first(mc, key, data);

	mp = mc->mc_pg[mc->mc_top];

	if (mc->mc_flags & C_EOF) {
		if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mp)-1)
			return MDB_NOTFOUND;
		mc->mc_flags ^= C_EOF;
	}

	if (mc->mc_db->md_flags & MDB_DUPSORT) {
		leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
		if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
			if (op == MDB_NEXT || op == MDB_NEXT_DUP) {
				rc = mdb_cursor_next(&mc->mc_xcursor->mx_cursor, data, NULL, MDB_NEXT);
				if (op != MDB_NEXT || rc != MDB_NOTFOUND) {
					if (rc == MDB_SUCCESS)
						MDB_GET_KEY(leaf, key);
					return rc;
				}
			}
			else {
				MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
			}
		} else {
			mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
			if (op == MDB_NEXT_DUP)
				return MDB_NOTFOUND;
		}
	}

	DPRINTF(("cursor_next: top page is %"Yu" in cursor %p",
		mdb_dbg_pgno(mp), (void *) mc));
	if (mc->mc_flags & C_DEL) {
		mc->mc_flags ^= C_DEL;
		goto skip;
	}

	if (mc->mc_ki[mc->mc_top] + 1u >= NUMKEYS(mp)) {
		DPUTS("=====> move to next sibling page");
		if ((rc = mdb_cursor_sibling(mc, 1)) != MDB_SUCCESS) {
			mc->mc_flags |= C_EOF;
			return rc;
		}
		mp = mc->mc_pg[mc->mc_top];
		DPRINTF(("next page is %"Yu", key index %u", mp->mp_pgno, mc->mc_ki[mc->mc_top]));
	} else
		mc->mc_ki[mc->mc_top]++;

skip:
	DPRINTF(("==> cursor points to page %"Yu" with %u keys, key index %u",
	    mdb_dbg_pgno(mp), NUMKEYS(mp), mc->mc_ki[mc->mc_top]));

	if (IS_LEAF2(mp)) {
		key->mv_size = mc->mc_db->md_pad;
		key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
		return MDB_SUCCESS;
	}

	mdb_cassert(mc, IS_LEAF(mp));
	leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		rc = mdb_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
		if (rc != MDB_SUCCESS)
			return rc;
	} else if (data) {
		if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
			return rc;
	}

	MDB_GET_KEY(leaf, key);
	return MDB_SUCCESS;
}

/** Move the cursor to the previous data item. */
int
mdb_cursor_prev(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op)
{
	MDB_page	*mp;
	MDB_node	*leaf;
	int rc;

	if (!(mc->mc_flags & C_INITIALIZED)) {
		rc = mdb_cursor_last(mc, key, data);
		if (rc)
			return rc;
		mc->mc_ki[mc->mc_top]++;
	}

	mp = mc->mc_pg[mc->mc_top];

	if ((mc->mc_db->md_flags & MDB_DUPSORT) &&
		mc->mc_ki[mc->mc_top] < NUMKEYS(mp)) {
		leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
		if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
			if (op == MDB_PREV || op == MDB_PREV_DUP) {
				rc = mdb_cursor_prev(&mc->mc_xcursor->mx_cursor, data, NULL, MDB_PREV);
				if (op != MDB_PREV || rc != MDB_NOTFOUND) {
					if (rc == MDB_SUCCESS) {
						MDB_GET_KEY(leaf, key);
						mc->mc_flags &= ~C_EOF;
					}
					return rc;
				}
			}
			else {
				MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
			}
		} else {
			mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
			if (op == MDB_PREV_DUP)
				return MDB_NOTFOUND;
		}
	}

	DPRINTF(("cursor_prev: top page is %"Yu" in cursor %p",
		mdb_dbg_pgno(mp), (void *) mc));

	mc->mc_flags &= ~(C_EOF|C_DEL);

	if (mc->mc_ki[mc->mc_top] == 0)  {
		DPUTS("=====> move to prev sibling page");
		if ((rc = mdb_cursor_sibling(mc, 0)) != MDB_SUCCESS) {
			return rc;
		}
		mp = mc->mc_pg[mc->mc_top];
		mc->mc_ki[mc->mc_top] = NUMKEYS(mp) - 1;
		DPRINTF(("prev page is %"Yu", key index %u", mp->mp_pgno, mc->mc_ki[mc->mc_top]));
	} else
		mc->mc_ki[mc->mc_top]--;

	DPRINTF(("==> cursor points to page %"Yu" with %u keys, key index %u",
	    mdb_dbg_pgno(mp), NUMKEYS(mp), mc->mc_ki[mc->mc_top]));

	if (!IS_LEAF(mp))
		return MDB_CORRUPTED;

	if (IS_LEAF2(mp)) {
		key->mv_size = mc->mc_db->md_pad;
		key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
		return MDB_SUCCESS;
	}

	leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		rc = mdb_cursor_last(&mc->mc_xcursor->mx_cursor, data, NULL);
		if (rc != MDB_SUCCESS)
			return rc;
	} else if (data) {
		if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
			return rc;
	}

	MDB_GET_KEY(leaf, key);
	return MDB_SUCCESS;
}

/** Set the cursor on a specific data item. */
int
mdb_cursor_set(MDB_cursor *mc, MDB_val *key, MDB_val *data,
    MDB_cursor_op op, int *exactp)
{
	int		 rc;
	MDB_page	*mp;
	MDB_node	*leaf = NULL;
	DKBUF;

	if (key->mv_size == 0)
		return MDB_BAD_VALSIZE;

	if (mc->mc_xcursor) {
		MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
		mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
	}

	/* See if we're already on the right page */
	if (mc->mc_flags & C_INITIALIZED) {
		MDB_val nodekey;

		mp = mc->mc_pg[mc->mc_top];
		if (!NUMKEYS(mp)) {
			mc->mc_ki[mc->mc_top] = 0;
			return MDB_NOTFOUND;
		}
		if (MP_FLAGS(mp) & P_LEAF2) {
			nodekey.mv_size = mc->mc_db->md_pad;
			nodekey.mv_data = LEAF2KEY(mp, 0, nodekey.mv_size);
		} else {
			leaf = NODEPTR(mp, 0);
			MDB_GET_KEY2(leaf, nodekey);
		}
		rc = mc->mc_dbx->md_cmp(key, &nodekey);
		if (rc == 0) {
			/* Probably happens rarely, but first node on the page
			 * was the one we wanted.
			 */
			mc->mc_ki[mc->mc_top] = 0;
			if (exactp)
				*exactp = 1;
			goto set1;
		}
		if (rc > 0) {
			unsigned int i;
			unsigned int nkeys = NUMKEYS(mp);
			if (nkeys > 1) {
				if (MP_FLAGS(mp) & P_LEAF2) {
					nodekey.mv_data = LEAF2KEY(mp,
						 nkeys-1, nodekey.mv_size);
				} else {
					leaf = NODEPTR(mp, nkeys-1);
					MDB_GET_KEY2(leaf, nodekey);
				}
				rc = mc->mc_dbx->md_cmp(key, &nodekey);
				if (rc == 0) {
					/* last node was the one we wanted */
					mc->mc_ki[mc->mc_top] = nkeys-1;
					if (exactp)
						*exactp = 1;
					goto set1;
				}
				if (rc < 0) {
					if (mc->mc_ki[mc->mc_top] < NUMKEYS(mp)) {
						/* This is definitely the right page, skip search_page */
						if (MP_FLAGS(mp) & P_LEAF2) {
							nodekey.mv_data = LEAF2KEY(mp,
								 mc->mc_ki[mc->mc_top], nodekey.mv_size);
						} else {
							leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
							MDB_GET_KEY2(leaf, nodekey);
						}
						rc = mc->mc_dbx->md_cmp(key, &nodekey);
						if (rc == 0) {
							/* current node was the one we wanted */
							if (exactp)
								*exactp = 1;
							goto set1;
						}
					}
					rc = 0;
					mc->mc_flags &= ~C_EOF;
					goto set2;
				}
			}
			/* If any parents have right-sibs, search.
			 * Otherwise, there's nothing further.
			 */
			for (i=0; i<mc->mc_top; i++)
				if (mc->mc_ki[i] <
					NUMKEYS(mc->mc_pg[i])-1)
					break;
			if (i == mc->mc_top) {
				/* There are no other pages */
				mc->mc_ki[mc->mc_top] = nkeys;
				return MDB_NOTFOUND;
			}
		}
		if (!mc->mc_top) {
			/* There are no other pages */
			mc->mc_ki[mc->mc_top] = 0;
			if (op == MDB_SET_RANGE && !exactp) {
				rc = 0;
				goto set1;
			} else
				return MDB_NOTFOUND;
		}
	} else {
		mc->mc_pg[0] = 0;
	}

	rc = mdb_page_search(mc, key, 0);
	if (rc != MDB_SUCCESS)
		return rc;

	mp = mc->mc_pg[mc->mc_top];
	mdb_cassert(mc, IS_LEAF(mp));

set2:
	leaf = mdb_node_search(mc, key, exactp);
	if (exactp != NULL && !*exactp) {
		/* MDB_SET specified and not an exact match. */
		return MDB_NOTFOUND;
	}

	if (leaf == NULL) {
		DPUTS("===> inexact leaf not found, goto sibling");
		if ((rc = mdb_cursor_sibling(mc, 1)) != MDB_SUCCESS) {
			mc->mc_flags |= C_EOF;
			return rc;		/* no entries matched */
		}
		mp = mc->mc_pg[mc->mc_top];
		mdb_cassert(mc, IS_LEAF(mp));
		leaf = NODEPTR(mp, 0);
	}

set1:
	mc->mc_flags |= C_INITIALIZED;
	mc->mc_flags &= ~C_EOF;

	if (IS_LEAF2(mp)) {
		if (op == MDB_SET_RANGE || op == MDB_SET_KEY) {
			key->mv_size = mc->mc_db->md_pad;
			key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
		}
		return MDB_SUCCESS;
	}

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		if (op == MDB_SET || op == MDB_SET_KEY || op == MDB_SET_RANGE) {
			rc = mdb_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
		} else {
			int ex2, *ex2p;
			if (op == MDB_GET_BOTH) {
				ex2p = &ex2;
				ex2 = 0;
			} else {
				ex2p = NULL;
			}
			rc = mdb_cursor_set(&mc->mc_xcursor->mx_cursor, data, NULL, MDB_SET_RANGE, ex2p);
			if (rc != MDB_SUCCESS)
				return rc;
		}
	} else if (data) {
		if (op == MDB_GET_BOTH || op == MDB_GET_BOTH_RANGE) {
			MDB_val olddata;
			MDB_cmp_func *dcmp;
			if ((rc = mdb_node_read(mc, leaf, &olddata)) != MDB_SUCCESS)
				return rc;
			dcmp = mc->mc_dbx->md_dcmp;
			if (NEED_CMP_CLONG(dcmp, olddata.mv_size))
				dcmp = mdb_cmp_clong;
			rc = dcmp(data, &olddata);
			if (rc) {
				if (op == MDB_GET_BOTH || rc > 0)
					return MDB_NOTFOUND;
				rc = 0;
			}
			*data = olddata;

		} else {
			if (mc->mc_xcursor)
				mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
			if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
				return rc;
		}
	}

	/* The key already matches in all other cases */
	if (op == MDB_SET_RANGE || op == MDB_SET_KEY)
		MDB_GET_KEY(leaf, key);
	DPRINTF(("==> cursor placed on key [%s]", DKEY(key)));

	return rc;
}

/** Move the cursor to the first item in the database. */
int
mdb_cursor_first(MDB_cursor *mc, MDB_val *key, MDB_val *data)
{
	int		 rc;
	MDB_node	*leaf;

	if (mc->mc_xcursor) {
		MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
		mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
	}

	if (!(mc->mc_flags & C_INITIALIZED) || mc->mc_top) {
		rc = mdb_page_search(mc, NULL, MDB_PS_FIRST);
		if (rc != MDB_SUCCESS)
			return rc;
	}
	mdb_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));

	leaf = NODEPTR(mc->mc_pg[mc->mc_top], 0);
	mc->mc_flags |= C_INITIALIZED;
	mc->mc_flags &= ~C_EOF;

	mc->mc_ki[mc->mc_top] = 0;

	if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
		if ( key ) {
			key->mv_size = mc->mc_db->md_pad;
			key->mv_data = LEAF2KEY(mc->mc_pg[mc->mc_top], 0, key->mv_size);
		}
		return MDB_SUCCESS;
	}

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		rc = mdb_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
		if (rc)
			return rc;
	} else if (data) {
		if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
			return rc;
	}

	MDB_GET_KEY(leaf, key);
	return MDB_SUCCESS;
}

/** Move the cursor to the last item in the database. */
int
mdb_cursor_last(MDB_cursor *mc, MDB_val *key, MDB_val *data)
{
	int		 rc;
	MDB_node	*leaf;

	if (mc->mc_xcursor) {
		MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
		mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
	}

	if (!(mc->mc_flags & C_INITIALIZED) || mc->mc_top) {
		rc = mdb_page_search(mc, NULL, MDB_PS_LAST);
		if (rc != MDB_SUCCESS)
			return rc;
	}
	mdb_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));

	mc->mc_ki[mc->mc_top] = NUMKEYS(mc->mc_pg[mc->mc_top]) - 1;
	mc->mc_flags |= C_INITIALIZED|C_EOF;
	leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);

	if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
		if (key) {
			key->mv_size = mc->mc_db->md_pad;
			key->mv_data = LEAF2KEY(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top], key->mv_size);
		}
		return MDB_SUCCESS;
	}

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		rc = mdb_cursor_last(&mc->mc_xcursor->mx_cursor, data, NULL);
		if (rc)
			return rc;
	} else if (data) {
		if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
			return rc;
	}

	MDB_GET_KEY(leaf, key);
	return MDB_SUCCESS;
}

int
mdb_cursor_get(MDB_cursor *mc, MDB_val *key, MDB_val *data,
    MDB_cursor_op op)
{
	int		 rc;
	int		 exact = 0;
	int		 (*mfunc)(MDB_cursor *mc, MDB_val *key, MDB_val *data);

	if (mc == NULL)
		return EINVAL;

	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	switch (op) {
	case MDB_GET_CURRENT:
		if (!(mc->mc_flags & C_INITIALIZED)) {
			rc = EINVAL;
		} else {
			MDB_page *mp = mc->mc_pg[mc->mc_top];
			int nkeys = NUMKEYS(mp);
			if (!nkeys || mc->mc_ki[mc->mc_top] >= nkeys) {
				mc->mc_ki[mc->mc_top] = nkeys;
				rc = MDB_NOTFOUND;
				break;
			}
			rc = MDB_SUCCESS;
			if (IS_LEAF2(mp)) {
				key->mv_size = mc->mc_db->md_pad;
				key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
			} else {
				MDB_node *leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
				MDB_GET_KEY(leaf, key);
				if (data) {
					if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
						rc = mdb_cursor_get(&mc->mc_xcursor->mx_cursor, data, NULL, MDB_GET_CURRENT);
					} else {
						rc = mdb_node_read(mc, leaf, data);
					}
				}
			}
		}
		break;
	case MDB_GET_BOTH:
	case MDB_GET_BOTH_RANGE:
		if (data == NULL) {
			rc = EINVAL;
			break;
		}
		if (mc->mc_xcursor == NULL) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		/* FALLTHRU */
	case MDB_SET:
	case MDB_SET_KEY:
	case MDB_SET_RANGE:
		if (key == NULL) {
			rc = EINVAL;
		} else {
			rc = mdb_cursor_set(mc, key, data, op,
				op == MDB_SET_RANGE ? NULL : &exact);
		}
		break;
	case MDB_GET_MULTIPLE:
		if (data == NULL || !(mc->mc_flags & C_INITIALIZED)) {
			rc = EINVAL;
			break;
		}
		if (!(mc->mc_db->md_flags & MDB_DUPFIXED)) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		rc = MDB_SUCCESS;
		if (!(mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED) ||
			(mc->mc_xcursor->mx_cursor.mc_flags & C_EOF))
			break;
		goto fetchm;
	case MDB_NEXT_MULTIPLE:
		if (data == NULL) {
			rc = EINVAL;
			break;
		}
		if (!(mc->mc_db->md_flags & MDB_DUPFIXED)) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		rc = mdb_cursor_next(mc, key, data, MDB_NEXT_DUP);
		if (rc == MDB_SUCCESS) {
			if (mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED) {
				MDB_cursor *mx;
fetchm:
				mx = &mc->mc_xcursor->mx_cursor;
				data->mv_size = NUMKEYS(mx->mc_pg[mx->mc_top]) *
					mx->mc_db->md_pad;
				data->mv_data = METADATA(mx->mc_pg[mx->mc_top]);
				mx->mc_ki[mx->mc_top] = NUMKEYS(mx->mc_pg[mx->mc_top])-1;
			} else {
				rc = MDB_NOTFOUND;
			}
		}
		break;
	case MDB_PREV_MULTIPLE:
		if (data == NULL) {
			rc = EINVAL;
			break;
		}
		if (!(mc->mc_db->md_flags & MDB_DUPFIXED)) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		if (!(mc->mc_flags & C_INITIALIZED))
			rc = mdb_cursor_last(mc, key, data);
		else
			rc = MDB_SUCCESS;
		if (rc == MDB_SUCCESS) {
			MDB_cursor *mx = &mc->mc_xcursor->mx_cursor;
			if (mx->mc_flags & C_INITIALIZED) {
				rc = mdb_cursor_sibling(mx, 0);
				if (rc == MDB_SUCCESS)
					goto fetchm;
			} else {
				rc = MDB_NOTFOUND;
			}
		}
		break;
	case MDB_NEXT:
	case MDB_NEXT_DUP:
	case MDB_NEXT_NODUP:
		rc = mdb_cursor_next(mc, key, data, op);
		break;
	case MDB_PREV:
	case MDB_PREV_DUP:
	case MDB_PREV_NODUP:
		rc = mdb_cursor_prev(mc, key, data, op);
		break;
	case MDB_FIRST:
		rc = mdb_cursor_first(mc, key, data);
		break;
	case MDB_FIRST_DUP:
		mfunc = mdb_cursor_first;
	mmove:
		if (data == NULL || !(mc->mc_flags & C_INITIALIZED)) {
			rc = EINVAL;
			break;
		}
		if (mc->mc_xcursor == NULL) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mc->mc_pg[mc->mc_top])) {
			mc->mc_ki[mc->mc_top] = NUMKEYS(mc->mc_pg[mc->mc_top]);
			rc = MDB_NOTFOUND;
			break;
		}
		mc->mc_flags &= ~C_EOF;
		{
			MDB_node *leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
			if (!F_ISSET(leaf->mn_flags, F_DUPDATA)) {
				MDB_GET_KEY(leaf, key);
				rc = mdb_node_read(mc, leaf, data);
				break;
			}
		}
		if (!(mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED)) {
			rc = EINVAL;
			break;
		}
		rc = mfunc(&mc->mc_xcursor->mx_cursor, data, NULL);
		break;
	case MDB_LAST:
		rc = mdb_cursor_last(mc, key, data);
		break;
	case MDB_LAST_DUP:
		mfunc = mdb_cursor_last;
		goto mmove;
	default:
		DPRINTF(("unhandled/unimplemented cursor operation %u", op));
		rc = EINVAL;
		break;
	}

	if (mc->mc_flags & C_DEL)
		mc->mc_flags ^= C_DEL;

	return rc;
}

/** Touch all the pages in the cursor stack. Set mc_top.
 *	Makes sure all the pages are writable, before attempting a write operation.
 * @param[in] mc The cursor to operate on.
 */
int
mdb_cursor_touch(MDB_cursor *mc)
{
	int rc = MDB_SUCCESS;

	if (mc->mc_dbi >= CORE_DBS && !(*mc->mc_dbflag & (DB_DIRTY|DB_DUPDATA))) {
		/* Touch DB record of named DB */
		MDB_cursor mc2;
		MDB_xcursor mcx;
		if (TXN_DBI_CHANGED(mc->mc_txn, mc->mc_dbi))
			return MDB_BAD_DBI;
		mdb_cursor_init(&mc2, mc->mc_txn, MAIN_DBI, &mcx);
		rc = mdb_page_search(&mc2, &mc->mc_dbx->md_name, MDB_PS_MODIFY);
		if (rc)
			 return rc;
		*mc->mc_dbflag |= DB_DIRTY;
	}
	mc->mc_top = 0;
	if (mc->mc_snum) {
		do {
			rc = mdb_page_touch(mc);
		} while (!rc && ++(mc->mc_top) < mc->mc_snum);
		mc->mc_top = mc->mc_snum-1;
	}
	return rc;
}

/** Do not spill pages to disk if txn is getting full, may fail instead */
#define MDB_NOSPILL	0x8000

int
_mdb_cursor_put(MDB_cursor *mc, MDB_val *key, MDB_val *data,
    unsigned int flags)
{
	MDB_env		*env;
	MDB_node	*leaf = NULL;
	MDB_page	*fp, *mp, *sub_root = NULL;
	uint16_t	fp_flags;
	MDB_val		xdata, *rdata, dkey, olddata;
	MDB_db dummy;
	int do_sub = 0, insert_key, insert_data;
	unsigned int mcount = 0, dcount = 0, nospill;
	size_t nsize;
	int rc, rc2;
	unsigned int nflags;
	DKBUF;

	if (mc == NULL || key == NULL)
		return EINVAL;

	env = mc->mc_txn->mt_env;

	/* Check this first so counter will always be zero on any
	 * early failures.
	 */
	if (flags & MDB_MULTIPLE) {
		dcount = data[1].mv_size;
		data[1].mv_size = 0;
		if (!F_ISSET(mc->mc_db->md_flags, MDB_DUPFIXED))
			return MDB_INCOMPATIBLE;
	}

	nospill = flags & MDB_NOSPILL;
	flags &= ~MDB_NOSPILL;

	if (mc->mc_txn->mt_flags & (MDB_TXN_RDONLY|MDB_TXN_BLOCKED))
		return (mc->mc_txn->mt_flags & MDB_TXN_RDONLY) ? EACCES : MDB_BAD_TXN;

	if (key->mv_size-1 >= ENV_MAXKEY(env))
		return MDB_BAD_VALSIZE;

#if SIZE_MAX > MAXDATASIZE
	if (data->mv_size > ((mc->mc_db->md_flags & MDB_DUPSORT) ? ENV_MAXKEY(env) : MAXDATASIZE))
		return MDB_BAD_VALSIZE;
#else
	if ((mc->mc_db->md_flags & MDB_DUPSORT) && data->mv_size > ENV_MAXKEY(env))
		return MDB_BAD_VALSIZE;
#endif

	DPRINTF(("==> put db %d key [%s], size %"Z"u, data size %"Z"u",
		DDBI(mc), DKEY(key), key ? key->mv_size : 0, data->mv_size));

	dkey.mv_size = 0;

	if (flags & MDB_CURRENT) {
		if (!(mc->mc_flags & C_INITIALIZED))
			return EINVAL;
		rc = MDB_SUCCESS;
	} else if (mc->mc_db->md_root == P_INVALID) {
		/* new database, cursor has nothing to point to */
		mc->mc_snum = 0;
		mc->mc_top = 0;
		mc->mc_flags &= ~C_INITIALIZED;
		rc = MDB_NO_ROOT;
	} else {
		int exact = 0;
		MDB_val d2;
		if (flags & MDB_APPEND) {
			MDB_val k2;
			rc = mdb_cursor_last(mc, &k2, &d2);
			if (rc == 0) {
				rc = mc->mc_dbx->md_cmp(key, &k2);
				if (rc > 0) {
					rc = MDB_NOTFOUND;
					mc->mc_ki[mc->mc_top]++;
				} else {
					/* new key is <= last key */
					rc = MDB_KEYEXIST;
				}
			}
		} else {
			rc = mdb_cursor_set(mc, key, &d2, MDB_SET, &exact);
		}
		if ((flags & MDB_NOOVERWRITE) && rc == 0) {
			DPRINTF(("duplicate key [%s]", DKEY(key)));
			*data = d2;
			return MDB_KEYEXIST;
		}
		if (rc && rc != MDB_NOTFOUND)
			return rc;
	}

	if (mc->mc_flags & C_DEL)
		mc->mc_flags ^= C_DEL;

	/* Cursor is positioned, check for room in the dirty list */
	if (!nospill) {
		if (flags & MDB_MULTIPLE) {
			rdata = &xdata;
			xdata.mv_size = data->mv_size * dcount;
		} else {
			rdata = data;
		}
		if ((rc2 = mdb_page_spill(mc, key, rdata)))
			return rc2;
	}

	if (rc == MDB_NO_ROOT) {
		MDB_page *np;
		/* new database, write a root leaf page */
		DPUTS("allocating new root leaf page");
		if ((rc2 = mdb_page_new(mc, P_LEAF, 1, &np))) {
			return rc2;
		}
		mdb_cursor_push(mc, np);
		mc->mc_db->md_root = np->mp_pgno;
		mc->mc_db->md_depth++;
		*mc->mc_dbflag |= DB_DIRTY;
		if ((mc->mc_db->md_flags & (MDB_DUPSORT|MDB_DUPFIXED))
			== MDB_DUPFIXED)
			MP_FLAGS(np) |= P_LEAF2;
		mc->mc_flags |= C_INITIALIZED;
	} else {
		/* make sure all cursor pages are writable */
		rc2 = mdb_cursor_touch(mc);
		if (rc2)
			return rc2;
	}

	insert_key = insert_data = rc;
	if (insert_key) {
		/* The key does not exist */
		DPRINTF(("inserting key at index %i", mc->mc_ki[mc->mc_top]));
		if ((mc->mc_db->md_flags & MDB_DUPSORT) &&
			LEAFSIZE(key, data) > env->me_nodemax)
		{
			/* Too big for a node, insert in sub-DB.  Set up an empty
			 * "old sub-page" for prep_subDB to expand to a full page.
			 */
			fp_flags = P_LEAF|P_DIRTY;
			fp = env->me_pbuf;
			fp->mp_pad = data->mv_size; /* used if MDB_DUPFIXED */
			MP_LOWER(fp) = MP_UPPER(fp) = (PAGEHDRSZ-PAGEBASE);
			olddata.mv_size = PAGEHDRSZ;
			goto prep_subDB;
		}
	} else {
		/* there's only a key anyway, so this is a no-op */
		if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
			char *ptr;
			unsigned int ksize = mc->mc_db->md_pad;
			if (key->mv_size != ksize)
				return MDB_BAD_VALSIZE;
			ptr = LEAF2KEY(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top], ksize);
			memcpy(ptr, key->mv_data, ksize);
fix_parent:
			/* if overwriting slot 0 of leaf, need to
			 * update branch key if there is a parent page
			 */
			if (mc->mc_top && !mc->mc_ki[mc->mc_top]) {
				unsigned short dtop = 1;
				mc->mc_top--;
				/* slot 0 is always an empty key, find real slot */
				while (mc->mc_top && !mc->mc_ki[mc->mc_top]) {
					mc->mc_top--;
					dtop++;
				}
				if (mc->mc_ki[mc->mc_top])
					rc2 = mdb_update_key(mc, key);
				else
					rc2 = MDB_SUCCESS;
				mc->mc_top += dtop;
				if (rc2)
					return rc2;
			}
			return MDB_SUCCESS;
		}

more:
		leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
		olddata.mv_size = NODEDSZ(leaf);
		olddata.mv_data = NODEDATA(leaf);

		/* DB has dups? */
		if (F_ISSET(mc->mc_db->md_flags, MDB_DUPSORT)) {
			/* Prepare (sub-)page/sub-DB to accept the new item,
			 * if needed.  fp: old sub-page or a header faking
			 * it.  mp: new (sub-)page.  offset: growth in page
			 * size.  xdata: node data with new page or DB.
			 */
			unsigned	i, offset = 0;
			mp = fp = xdata.mv_data = env->me_pbuf;
			mp->mp_pgno = mc->mc_pg[mc->mc_top]->mp_pgno;

			/* Was a single item before, must convert now */
			if (!F_ISSET(leaf->mn_flags, F_DUPDATA)) {
				MDB_cmp_func *dcmp;
				/* Just overwrite the current item */
				if (flags == MDB_CURRENT)
					goto current;
				dcmp = mc->mc_dbx->md_dcmp;
				if (NEED_CMP_CLONG(dcmp, olddata.mv_size))
					dcmp = mdb_cmp_clong;
				/* does data match? */
				if (!dcmp(data, &olddata)) {
					if (flags & (MDB_NODUPDATA|MDB_APPENDDUP))
						return MDB_KEYEXIST;
					/* overwrite it */
					goto current;
				}

				/* Back up original data item */
				dkey.mv_size = olddata.mv_size;
				dkey.mv_data = memcpy(fp+1, olddata.mv_data, olddata.mv_size);

				/* Make sub-page header for the dup items, with dummy body */
				MP_FLAGS(fp) = P_LEAF|P_DIRTY|P_SUBP;
				MP_LOWER(fp) = (PAGEHDRSZ-PAGEBASE);
				xdata.mv_size = PAGEHDRSZ + dkey.mv_size + data->mv_size;
				if (mc->mc_db->md_flags & MDB_DUPFIXED) {
					MP_FLAGS(fp) |= P_LEAF2;
					fp->mp_pad = data->mv_size;
					xdata.mv_size += 2 * data->mv_size;	/* leave space for 2 more */
				} else {
					xdata.mv_size += 2 * (sizeof(indx_t) + NODESIZE) +
						(dkey.mv_size & 1) + (data->mv_size & 1);
				}
				MP_UPPER(fp) = xdata.mv_size - PAGEBASE;
				olddata.mv_size = xdata.mv_size; /* pretend olddata is fp */
			} else if (leaf->mn_flags & F_SUBDATA) {
				/* Data is on sub-DB, just store it */
				flags |= F_DUPDATA|F_SUBDATA;
				goto put_sub;
			} else {
				/* Data is on sub-page */
				fp = olddata.mv_data;
				switch (flags) {
				default:
					if (!(mc->mc_db->md_flags & MDB_DUPFIXED)) {
						offset = EVEN(NODESIZE + sizeof(indx_t) +
							data->mv_size);
						break;
					}
					offset = fp->mp_pad;
					if (SIZELEFT(fp) < offset) {
						offset *= 4; /* space for 4 more */
						break;
					}
					/* FALLTHRU */ /* Big enough MDB_DUPFIXED sub-page */
				case MDB_CURRENT:
					MP_FLAGS(fp) |= P_DIRTY;
					COPY_PGNO(MP_PGNO(fp), MP_PGNO(mp));
					mc->mc_xcursor->mx_cursor.mc_pg[0] = fp;
					flags |= F_DUPDATA;
					goto put_sub;
				}
				xdata.mv_size = olddata.mv_size + offset;
			}

			fp_flags = MP_FLAGS(fp);
			if (NODESIZE + NODEKSZ(leaf) + xdata.mv_size > env->me_nodemax) {
					/* Too big for a sub-page, convert to sub-DB */
					fp_flags &= ~P_SUBP;
prep_subDB:
					if (mc->mc_db->md_flags & MDB_DUPFIXED) {
						fp_flags |= P_LEAF2;
						dummy.md_pad = fp->mp_pad;
						dummy.md_flags = MDB_DUPFIXED;
						if (mc->mc_db->md_flags & MDB_INTEGERDUP)
							dummy.md_flags |= MDB_INTEGERKEY;
					} else {
						dummy.md_pad = 0;
						dummy.md_flags = 0;
					}
					dummy.md_depth = 1;
					dummy.md_branch_pages = 0;
					dummy.md_leaf_pages = 1;
					dummy.md_overflow_pages = 0;
					dummy.md_entries = NUMKEYS(fp);
					xdata.mv_size = sizeof(MDB_db);
					xdata.mv_data = &dummy;
					if ((rc = mdb_page_alloc(mc, 1, &mp)))
						return rc;
					offset = env->me_psize - olddata.mv_size;
					flags |= F_DUPDATA|F_SUBDATA;
					dummy.md_root = mp->mp_pgno;
					sub_root = mp;
			}
			if (mp != fp) {
				MP_FLAGS(mp) = fp_flags | P_DIRTY;
				MP_PAD(mp)   = MP_PAD(fp);
				MP_LOWER(mp) = MP_LOWER(fp);
				MP_UPPER(mp) = MP_UPPER(fp) + offset;
				if (fp_flags & P_LEAF2) {
					memcpy(METADATA(mp), METADATA(fp), NUMKEYS(fp) * fp->mp_pad);
				} else {
					memcpy((char *)mp + MP_UPPER(mp) + PAGEBASE, (char *)fp + MP_UPPER(fp) + PAGEBASE,
						olddata.mv_size - MP_UPPER(fp) - PAGEBASE);
					memcpy((char *)MP_PTRS(mp), (char *)MP_PTRS(fp), NUMKEYS(fp) * sizeof(mp->mp_ptrs[0]));
					for (i=0; i<NUMKEYS(fp); i++)
						mp->mp_ptrs[i] += offset;
				}
			}

			rdata = &xdata;
			flags |= F_DUPDATA;
			do_sub = 1;
			if (!insert_key)
				mdb_node_del(mc, 0);
			goto new_sub;
		}
current:
		/* LMDB passes F_SUBDATA in 'flags' to write a DB record */
		if ((leaf->mn_flags ^ flags) & F_SUBDATA)
			return MDB_INCOMPATIBLE;
		/* overflow page overwrites need special handling */
		if (F_ISSET(leaf->mn_flags, F_BIGDATA)) {
			MDB_page *omp;
			pgno_t pg;
			int level, ovpages, dpages = OVPAGES(data->mv_size, env->me_psize);

			memcpy(&pg, olddata.mv_data, sizeof(pg));
			if ((rc2 = mdb_page_get(mc, pg, &omp, &level)) != 0)
				return rc2;
			ovpages = omp->mp_pages;

			/* Is the ov page large enough? */
			if (ovpages >= dpages) {
			  if (!(omp->mp_flags & P_DIRTY) &&
				  (level || (env->me_flags & MDB_WRITEMAP)))
			  {
				rc = mdb_page_unspill(mc->mc_txn, omp, &omp);
				if (rc)
					return rc;
				level = 0;		/* dirty in this txn or clean */
			  }
			  /* Is it dirty? */
			  if (omp->mp_flags & P_DIRTY) {
				/* yes, overwrite it. Note in this case we don't
				 * bother to try shrinking the page if the new data
				 * is smaller than the overflow threshold.
				 */
				if (level > 1) {
					/* It is writable only in a parent txn */
					size_t sz = (size_t) env->me_psize * ovpages, off;
					MDB_page *np = mdb_page_malloc(mc->mc_txn, ovpages);
					MDB_ID2 id2;
					if (!np)
						return ENOMEM;
					id2.mid = pg;
					id2.mptr = np;
					/* Note - this page is already counted in parent's dirty_room */
					rc2 = mdb_mid2l_insert(mc->mc_txn->mt_u.dirty_list, &id2);
					mdb_cassert(mc, rc2 == 0);
					/* Currently we make the page look as with put() in the
					 * parent txn, in case the user peeks at MDB_RESERVEd
					 * or unused parts. Some users treat ovpages specially.
					 */
					if (!(flags & MDB_RESERVE)) {
						/* Skip the part where LMDB will put *data.
						 * Copy end of page, adjusting alignment so
						 * compiler may copy words instead of bytes.
						 */
						off = (PAGEHDRSZ + data->mv_size) & -(int)sizeof(size_t);
						memcpy((size_t *)((char *)np + off),
							(size_t *)((char *)omp + off), sz - off);
						sz = PAGEHDRSZ;
					}
					memcpy(np, omp, sz); /* Copy beginning of page */
					omp = np;
				}
				SETDSZ(leaf, data->mv_size);
				if (F_ISSET(flags, MDB_RESERVE))
					data->mv_data = METADATA(omp);
				else
					memcpy(METADATA(omp), data->mv_data, data->mv_size);
				return MDB_SUCCESS;
			  }
			}
			if ((rc2 = mdb_ovpage_free(mc, omp)) != MDB_SUCCESS)
				return rc2;
		} else if (data->mv_size == olddata.mv_size) {
			/* same size, just replace it. Note that we could
			 * also reuse this node if the new data is smaller,
			 * but instead we opt to shrink the node in that case.
			 */
			if (F_ISSET(flags, MDB_RESERVE))
				data->mv_data = olddata.mv_data;
			else if (!(mc->mc_flags & C_SUB))
				memcpy(olddata.mv_data, data->mv_data, data->mv_size);
			else {
				if (key->mv_size != NODEKSZ(leaf))
					goto new_ksize;
				memcpy(NODEKEY(leaf), key->mv_data, key->mv_size);
				goto fix_parent;
			}
			return MDB_SUCCESS;
		}
new_ksize:
		mdb_node_del(mc, 0);
	}

	rdata = data;

new_sub:
	nflags = flags & NODE_ADD_FLAGS;
	nsize = IS_LEAF2(mc->mc_pg[mc->mc_top]) ? key->mv_size : mdb_leaf_size(env, key, rdata);
	if (SIZELEFT(mc->mc_pg[mc->mc_top]) < nsize) {
		if (( flags & (F_DUPDATA|F_SUBDATA)) == F_DUPDATA )
			nflags &= ~MDB_APPEND; /* sub-page may need room to grow */
		if (!insert_key)
			nflags |= MDB_SPLIT_REPLACE;
		rc = mdb_page_split(mc, key, rdata, P_INVALID, nflags);
	} else {
		/* There is room already in this leaf page. */
		rc = mdb_node_add(mc, mc->mc_ki[mc->mc_top], key, rdata, 0, nflags);
		if (rc == 0) {
			/* Adjust other cursors pointing to mp */
			MDB_cursor *m2, *m3;
			MDB_dbi dbi = mc->mc_dbi;
			unsigned i = mc->mc_top;
			MDB_page *mp = mc->mc_pg[i];

			for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
				if (mc->mc_flags & C_SUB)
					m3 = &m2->mc_xcursor->mx_cursor;
				else
					m3 = m2;
				if (m3 == mc || m3->mc_snum < mc->mc_snum || m3->mc_pg[i] != mp) continue;
				if (m3->mc_ki[i] >= mc->mc_ki[i] && insert_key) {
					m3->mc_ki[i]++;
				}
				XCURSOR_REFRESH(m3, i, mp);
			}
		}
	}

	if (rc == MDB_SUCCESS) {
		/* Now store the actual data in the child DB. Note that we're
		 * storing the user data in the keys field, so there are strict
		 * size limits on dupdata. The actual data fields of the child
		 * DB are all zero size.
		 */
		if (do_sub) {
			int xflags, new_dupdata;
			mdb_size_t ecount;
put_sub:
			xdata.mv_size = 0;
			xdata.mv_data = "";
			leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
			if ((flags & (MDB_CURRENT|MDB_APPENDDUP)) == MDB_CURRENT) {
				xflags = MDB_CURRENT|MDB_NOSPILL;
			} else {
				mdb_xcursor_init1(mc, leaf);
				xflags = (flags & MDB_NODUPDATA) ?
					MDB_NOOVERWRITE|MDB_NOSPILL : MDB_NOSPILL;
			}
			if (sub_root)
				mc->mc_xcursor->mx_cursor.mc_pg[0] = sub_root;
			new_dupdata = (int)dkey.mv_size;
			/* converted, write the original data first */
			if (dkey.mv_size) {
				rc = _mdb_cursor_put(&mc->mc_xcursor->mx_cursor, &dkey, &xdata, xflags);
				if (rc)
					goto bad_sub;
				/* we've done our job */
				dkey.mv_size = 0;
			}
			if (!(leaf->mn_flags & F_SUBDATA) || sub_root) {
				/* Adjust other cursors pointing to mp */
				MDB_cursor *m2;
				MDB_xcursor *mx = mc->mc_xcursor;
				unsigned i = mc->mc_top;
				MDB_page *mp = mc->mc_pg[i];

				for (m2 = mc->mc_txn->mt_cursors[mc->mc_dbi]; m2; m2=m2->mc_next) {
					if (m2 == mc || m2->mc_snum < mc->mc_snum) continue;
					if (!(m2->mc_flags & C_INITIALIZED)) continue;
					if (m2->mc_pg[i] == mp) {
						if (m2->mc_ki[i] == mc->mc_ki[i]) {
							mdb_xcursor_init2(m2, mx, new_dupdata);
						} else if (!insert_key) {
							XCURSOR_REFRESH(m2, i, mp);
						}
					}
				}
			}
			ecount = mc->mc_xcursor->mx_db.md_entries;
			if (flags & MDB_APPENDDUP)
				xflags |= MDB_APPEND;
			rc = _mdb_cursor_put(&mc->mc_xcursor->mx_cursor, data, &xdata, xflags);
			if (flags & F_SUBDATA) {
				void *db = NODEDATA(leaf);
				memcpy(db, &mc->mc_xcursor->mx_db, sizeof(MDB_db));
			}
			insert_data = mc->mc_xcursor->mx_db.md_entries - ecount;
		}
		/* Increment count unless we just replaced an existing item. */
		if (insert_data)
			mc->mc_db->md_entries++;
		if (insert_key) {
			/* Invalidate txn if we created an empty sub-DB */
			if (rc)
				goto bad_sub;
			/* If we succeeded and the key didn't exist before,
			 * make sure the cursor is marked valid.
			 */
			mc->mc_flags |= C_INITIALIZED;
		}
		if (flags & MDB_MULTIPLE) {
			if (!rc) {
				mcount++;
				/* let caller know how many succeeded, if any */
				data[1].mv_size = mcount;
				if (mcount < dcount) {
					data[0].mv_data = (char *)data[0].mv_data + data[0].mv_size;
					insert_key = insert_data = 0;
					goto more;
				}
			}
		}
		return rc;
bad_sub:
		if (rc == MDB_KEYEXIST)	/* should not happen, we deleted that item */
			rc = MDB_PROBLEM;
	}
	mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
	return rc;
}

int
mdb_cursor_put(MDB_cursor *mc, MDB_val *key, MDB_val *data,
    unsigned int flags)
{
	DKBUF;
	DDBUF;
	int rc = _mdb_cursor_put(mc, key, data, flags);
	MDB_TRACE(("%p, %"Z"u[%s], %"Z"u%s, %u",
		mc, key ? key->mv_size:0, DKEY(key), data ? data->mv_size:0,
			data ? mdb_dval(mc->mc_txn, mc->mc_dbi, data, dbuf):"", flags));
	return rc;
}

int
_mdb_cursor_del(MDB_cursor *mc, unsigned int flags)
{
	MDB_node	*leaf;
	MDB_page	*mp;
	int rc;

	if (mc->mc_txn->mt_flags & (MDB_TXN_RDONLY|MDB_TXN_BLOCKED))
		return (mc->mc_txn->mt_flags & MDB_TXN_RDONLY) ? EACCES : MDB_BAD_TXN;

	if (!(mc->mc_flags & C_INITIALIZED))
		return EINVAL;

	if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mc->mc_pg[mc->mc_top]))
		return MDB_NOTFOUND;

	if (!(flags & MDB_NOSPILL) && (rc = mdb_page_spill(mc, NULL, NULL)))
		return rc;

	rc = mdb_cursor_touch(mc);
	if (rc)
		return rc;

	mp = mc->mc_pg[mc->mc_top];
	if (!IS_LEAF(mp))
		return MDB_CORRUPTED;
	if (IS_LEAF2(mp))
		goto del_key;
	leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		if (flags & MDB_NODUPDATA) {
			/* mdb_cursor_del0() will subtract the final entry */
			mc->mc_db->md_entries -= mc->mc_xcursor->mx_db.md_entries - 1;
			mc->mc_xcursor->mx_cursor.mc_flags &= ~C_INITIALIZED;
		} else {
			if (!F_ISSET(leaf->mn_flags, F_SUBDATA)) {
				mc->mc_xcursor->mx_cursor.mc_pg[0] = NODEDATA(leaf);
			}
			rc = _mdb_cursor_del(&mc->mc_xcursor->mx_cursor, MDB_NOSPILL);
			if (rc)
				return rc;
			/* If sub-DB still has entries, we're done */
			if (mc->mc_xcursor->mx_db.md_entries) {
				if (leaf->mn_flags & F_SUBDATA) {
					/* update subDB info */
					void *db = NODEDATA(leaf);
					memcpy(db, &mc->mc_xcursor->mx_db, sizeof(MDB_db));
				} else {
					MDB_cursor *m2;
					/* shrink fake page */
					mdb_node_shrink(mp, mc->mc_ki[mc->mc_top]);
					leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
					mc->mc_xcursor->mx_cursor.mc_pg[0] = NODEDATA(leaf);
					/* fix other sub-DB cursors pointed at fake pages on this page */
					for (m2 = mc->mc_txn->mt_cursors[mc->mc_dbi]; m2; m2=m2->mc_next) {
						if (m2 == mc || m2->mc_snum < mc->mc_snum) continue;
						if (!(m2->mc_flags & C_INITIALIZED)) continue;
						if (m2->mc_pg[mc->mc_top] == mp) {
							XCURSOR_REFRESH(m2, mc->mc_top, mp);
						}
					}
				}
				mc->mc_db->md_entries--;
				return rc;
			} else {
				mc->mc_xcursor->mx_cursor.mc_flags &= ~C_INITIALIZED;
			}
			/* otherwise fall thru and delete the sub-DB */
		}

		if (leaf->mn_flags & F_SUBDATA) {
			/* add all the child DB's pages to the free list */
			rc = mdb_drop0(&mc->mc_xcursor->mx_cursor, 0);
			if (rc)
				goto fail;
		}
	}
	/* LMDB passes F_SUBDATA in 'flags' to delete a DB record */
	else if ((leaf->mn_flags ^ flags) & F_SUBDATA) {
		rc = MDB_INCOMPATIBLE;
		goto fail;
	}

	/* add overflow pages to free list */
	if (F_ISSET(leaf->mn_flags, F_BIGDATA)) {
		MDB_page *omp;
		pgno_t pg;

		memcpy(&pg, NODEDATA(leaf), sizeof(pg));
		if ((rc = mdb_page_get(mc, pg, &omp, NULL)) ||
			(rc = mdb_ovpage_free(mc, omp)))
			goto fail;
	}

del_key:
	return mdb_cursor_del0(mc);

fail:
	mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
	return rc;
}

int
mdb_cursor_del(MDB_cursor *mc, unsigned int flags)
{
	MDB_TRACE(("%p, %u",
		mc, flags));
	return _mdb_cursor_del(mc, flags);
}

/** Initial setup of a sorted-dups cursor.
 * Sorted duplicates are implemented as a sub-database for the given key.
 * The duplicate data items are actually keys of the sub-database.
 * Operations on the duplicate data items are performed using a sub-cursor
 * initialized when the sub-database is first accessed. This function does
 * the preliminary setup of the sub-cursor, filling in the fields that
 * depend only on the parent DB.
 * @param[in] mc The main cursor whose sorted-dups cursor is to be initialized.
 */
void
mdb_xcursor_init0(MDB_cursor *mc)
{
	MDB_xcursor *mx = mc->mc_xcursor;

	mx->mx_cursor.mc_xcursor = NULL;
	mx->mx_cursor.mc_txn = mc->mc_txn;
	mx->mx_cursor.mc_db = &mx->mx_db;
	mx->mx_cursor.mc_dbx = &mx->mx_dbx;
	mx->mx_cursor.mc_dbi = mc->mc_dbi;
	mx->mx_cursor.mc_dbflag = &mx->mx_dbflag;
	mx->mx_cursor.mc_snum = 0;
	mx->mx_cursor.mc_top = 0;
	MC_SET_OVPG(&mx->mx_cursor, NULL);
	mx->mx_cursor.mc_flags = C_SUB | (mc->mc_flags & (C_ORIG_RDONLY|C_WRITEMAP));
	mx->mx_dbx.md_name.mv_size = 0;
	mx->mx_dbx.md_name.mv_data = NULL;
	mx->mx_dbx.md_cmp = mc->mc_dbx->md_dcmp;
	mx->mx_dbx.md_dcmp = NULL;
	mx->mx_dbx.md_rel = mc->mc_dbx->md_rel;
}

/** Final setup of a sorted-dups cursor.
 *	Sets up the fields that depend on the data from the main cursor.
 * @param[in] mc The main cursor whose sorted-dups cursor is to be initialized.
 * @param[in] node The data containing the #MDB_db record for the
 * sorted-dup database.
 */
void
mdb_xcursor_init1(MDB_cursor *mc, MDB_node *node)
{
	MDB_xcursor *mx = mc->mc_xcursor;

	mx->mx_cursor.mc_flags &= C_SUB|C_ORIG_RDONLY|C_WRITEMAP;
	if (node->mn_flags & F_SUBDATA) {
		memcpy(&mx->mx_db, NODEDATA(node), sizeof(MDB_db));
		mx->mx_cursor.mc_pg[0] = 0;
		mx->mx_cursor.mc_snum = 0;
		mx->mx_cursor.mc_top = 0;
	} else {
		MDB_page *fp = NODEDATA(node);
		mx->mx_db.md_pad = 0;
		mx->mx_db.md_flags = 0;
		mx->mx_db.md_depth = 1;
		mx->mx_db.md_branch_pages = 0;
		mx->mx_db.md_leaf_pages = 1;
		mx->mx_db.md_overflow_pages = 0;
		mx->mx_db.md_entries = NUMKEYS(fp);
		COPY_PGNO(mx->mx_db.md_root, MP_PGNO(fp));
		mx->mx_cursor.mc_snum = 1;
		mx->mx_cursor.mc_top = 0;
		mx->mx_cursor.mc_flags |= C_INITIALIZED;
		mx->mx_cursor.mc_pg[0] = fp;
		mx->mx_cursor.mc_ki[0] = 0;
		if (mc->mc_db->md_flags & MDB_DUPFIXED) {
			mx->mx_db.md_flags = MDB_DUPFIXED;
			mx->mx_db.md_pad = fp->mp_pad;
			if (mc->mc_db->md_flags & MDB_INTEGERDUP)
				mx->mx_db.md_flags |= MDB_INTEGERKEY;
		}
	}
	DPRINTF(("Sub-db -%u root page %"Yu, mx->mx_cursor.mc_dbi,
		mx->mx_db.md_root));
	mx->mx_dbflag = DB_VALID|DB_USRVALID|DB_DUPDATA;
	if (NEED_CMP_CLONG(mx->mx_dbx.md_cmp, mx->mx_db.md_pad))
		mx->mx_dbx.md_cmp = mdb_cmp_clong;
}


/** Fixup a sorted-dups cursor due to underlying update.
 *	Sets up some fields that depend on the data from the main cursor.
 *	Almost the same as init1, but skips initialization steps if the
 *	xcursor had already been used.
 * @param[in] mc The main cursor whose sorted-dups cursor is to be fixed up.
 * @param[in] src_mx The xcursor of an up-to-date cursor.
 * @param[in] new_dupdata True if converting from a non-#F_DUPDATA item.
 */
void
mdb_xcursor_init2(MDB_cursor *mc, MDB_xcursor *src_mx, int new_dupdata)
{
	MDB_xcursor *mx = mc->mc_xcursor;

	if (new_dupdata) {
		mx->mx_cursor.mc_snum = 1;
		mx->mx_cursor.mc_top = 0;
		mx->mx_cursor.mc_flags |= C_INITIALIZED;
		mx->mx_cursor.mc_ki[0] = 0;
		mx->mx_dbflag = DB_VALID|DB_USRVALID|DB_DUPDATA;
#if UINT_MAX < MDB_SIZE_MAX	/* matches mdb_xcursor_init1:NEED_CMP_CLONG() */
		mx->mx_dbx.md_cmp = src_mx->mx_dbx.md_cmp;
#endif
	} else if (!(mx->mx_cursor.mc_flags & C_INITIALIZED)) {
		return;
	}
	mx->mx_db = src_mx->mx_db;
	mx->mx_cursor.mc_pg[0] = src_mx->mx_cursor.mc_pg[0];
	DPRINTF(("Sub-db -%u root page %"Yu, mx->mx_cursor.mc_dbi,
		mx->mx_db.md_root));
}

/** Initialize a cursor for a given transaction and database. */
void
mdb_cursor_init(MDB_cursor *mc, MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx)
{
	mc->mc_next = NULL;
	mc->mc_backup = NULL;
	mc->mc_dbi = dbi;
	mc->mc_txn = txn;
	mc->mc_db = &txn->mt_dbs[dbi];
	mc->mc_dbx = &txn->mt_dbxs[dbi];
	mc->mc_dbflag = &txn->mt_dbflags[dbi];
	mc->mc_snum = 0;
	mc->mc_top = 0;
	mc->mc_pg[0] = 0;
	mc->mc_ki[0] = 0;
	MC_SET_OVPG(mc, NULL);
	mc->mc_flags = txn->mt_flags & (C_ORIG_RDONLY|C_WRITEMAP);
	if (txn->mt_dbs[dbi].md_flags & MDB_DUPSORT) {
		mdb_tassert(txn, mx != NULL);
		mc->mc_xcursor = mx;
		mdb_xcursor_init0(mc);
	} else {
		mc->mc_xcursor = NULL;
	}
	if (*mc->mc_dbflag & DB_STALE) {
		mdb_page_search(mc, NULL, MDB_PS_ROOTONLY);
	}
}

int
mdb_cursor_open(MDB_txn *txn, MDB_dbi dbi, MDB_cursor **ret)
{
	MDB_cursor	*mc;
	size_t size = sizeof(MDB_cursor);

	if (!ret || !TXN_DBI_EXIST(txn, dbi, DB_VALID))
		return EINVAL;

	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	if (dbi == FREE_DBI && !F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
		return EINVAL;

	if (txn->mt_dbs[dbi].md_flags & MDB_DUPSORT)
		size += sizeof(MDB_xcursor);

	if ((mc = malloc(size)) != NULL) {
		mdb_cursor_init(mc, txn, dbi, (MDB_xcursor *)(mc + 1));
		if (txn->mt_cursors) {
			mc->mc_next = txn->mt_cursors[dbi];
			txn->mt_cursors[dbi] = mc;
			mc->mc_flags |= C_UNTRACK;
		}
	} else {
		return ENOMEM;
	}

	MDB_TRACE(("%p, %u = %p", txn, dbi, mc));
	*ret = mc;

	return MDB_SUCCESS;
}

int
mdb_cursor_renew(MDB_txn *txn, MDB_cursor *mc)
{
	if (!mc || !TXN_DBI_EXIST(txn, mc->mc_dbi, DB_VALID))
		return EINVAL;

	if ((mc->mc_flags & C_UNTRACK) || txn->mt_cursors)
		return EINVAL;

	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	mdb_cursor_init(mc, txn, mc->mc_dbi, mc->mc_xcursor);
	return MDB_SUCCESS;
}

/* Return the count of duplicate data items for the current key */
int
mdb_cursor_count(MDB_cursor *mc, mdb_size_t *countp)
{
	MDB_node	*leaf;

	if (mc == NULL || countp == NULL)
		return EINVAL;

	if (mc->mc_xcursor == NULL)
		return MDB_INCOMPATIBLE;

	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	if (!(mc->mc_flags & C_INITIALIZED))
		return EINVAL;

	if (!mc->mc_snum)
		return MDB_NOTFOUND;

	if (mc->mc_flags & C_EOF) {
		if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mc->mc_pg[mc->mc_top]))
			return MDB_NOTFOUND;
		mc->mc_flags ^= C_EOF;
	}

	leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
	if (!F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		*countp = 1;
	} else {
		if (!(mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED))
			return EINVAL;

		*countp = mc->mc_xcursor->mx_db.md_entries;
	}
	return MDB_SUCCESS;
}

void
mdb_cursor_close(MDB_cursor *mc)
{
	MDB_TRACE(("%p", mc));
	if (mc) {
		MDB_CURSOR_UNREF(mc, 0);
	}
	if (mc && !mc->mc_backup) {
		/* Remove from txn, if tracked.
		 * A read-only txn (!C_UNTRACK) may have been freed already,
		 * so do not peek inside it.  Only write txns track cursors.
		 */
		if ((mc->mc_flags & C_UNTRACK) && mc->mc_txn->mt_cursors) {
			MDB_cursor **prev = &mc->mc_txn->mt_cursors[mc->mc_dbi];
			while (*prev && *prev != mc) prev = &(*prev)->mc_next;
			if (*prev == mc)
				*prev = mc->mc_next;
		}
		free(mc);
	}
}

MDB_txn *
mdb_cursor_txn(MDB_cursor *mc)
{
	if (!mc) return NULL;
	return mc->mc_txn;
}

MDB_dbi
mdb_cursor_dbi(MDB_cursor *mc)
{
	return mc->mc_dbi;
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

void
mdb_cursor_copy(const MDB_cursor *csrc, MDB_cursor *cdst);

/** Copy the contents of a cursor.
 * @param[in] csrc The cursor to copy from.
 * @param[out] cdst The cursor to copy to.
 */
void
mdb_cursor_copy(const MDB_cursor *csrc, MDB_cursor *cdst)
{
	unsigned int i;

	cdst->mc_txn = csrc->mc_txn;
	cdst->mc_dbi = csrc->mc_dbi;
	cdst->mc_db  = csrc->mc_db;
	cdst->mc_dbx = csrc->mc_dbx;
	cdst->mc_snum = csrc->mc_snum;
	cdst->mc_top = csrc->mc_top;
	cdst->mc_flags = csrc->mc_flags;
	MC_SET_OVPG(cdst, MC_OVPG(csrc));

	for (i=0; i<csrc->mc_snum; i++) {
		cdst->mc_pg[i] = csrc->mc_pg[i];
		cdst->mc_ki[i] = csrc->mc_ki[i];
	}
}

/** Complete a delete operation started by #mdb_cursor_del(). */
int
mdb_cursor_del0(MDB_cursor *mc)
{
	int rc;
	MDB_page *mp;
	indx_t ki;
	unsigned int nkeys;
	MDB_cursor *m2, *m3;
	MDB_dbi dbi = mc->mc_dbi;

	ki = mc->mc_ki[mc->mc_top];
	mp = mc->mc_pg[mc->mc_top];
	mdb_node_del(mc, mc->mc_db->md_pad);
	mc->mc_db->md_entries--;
	{
		/* Adjust other cursors pointing to mp */
		for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
			m3 = (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
			if (! (m2->mc_flags & m3->mc_flags & C_INITIALIZED))
				continue;
			if (m3 == mc || m3->mc_snum < mc->mc_snum)
				continue;
			if (m3->mc_pg[mc->mc_top] == mp) {
				if (m3->mc_ki[mc->mc_top] == ki) {
					m3->mc_flags |= C_DEL;
					if (mc->mc_db->md_flags & MDB_DUPSORT) {
						/* Sub-cursor referred into dataset which is gone */
						m3->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
					}
					continue;
				} else if (m3->mc_ki[mc->mc_top] > ki) {
					m3->mc_ki[mc->mc_top]--;
				}
				XCURSOR_REFRESH(m3, mc->mc_top, mp);
			}
		}
	}
	rc = mdb_rebalance(mc);
	if (rc)
		goto fail;

	/* DB is totally empty now, just bail out.
	 * Other cursors adjustments were already done
	 * by mdb_rebalance and aren't needed here.
	 */
	if (!mc->mc_snum) {
		mc->mc_flags |= C_EOF;
		return rc;
	}

	mp = mc->mc_pg[mc->mc_top];
	nkeys = NUMKEYS(mp);

	/* Adjust other cursors pointing to mp */
	for (m2 = mc->mc_txn->mt_cursors[dbi]; !rc && m2; m2=m2->mc_next) {
		m3 = (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
		if (!(m2->mc_flags & m3->mc_flags & C_INITIALIZED))
			continue;
		if (m3->mc_snum < mc->mc_snum)
			continue;
		if (m3->mc_pg[mc->mc_top] == mp) {
			if (m3->mc_ki[mc->mc_top] >= mc->mc_ki[mc->mc_top]) {
			/* if m3 points past last node in page, find next sibling */
				if (m3->mc_ki[mc->mc_top] >= nkeys) {
					rc = mdb_cursor_sibling(m3, 1);
					if (rc == MDB_NOTFOUND) {
						m3->mc_flags |= C_EOF;
						rc = MDB_SUCCESS;
						continue;
					}
					if (rc)
						goto fail;
				}
				if (m3->mc_xcursor && !(m3->mc_flags & C_EOF)) {
					MDB_node *node = NODEPTR(m3->mc_pg[m3->mc_top], m3->mc_ki[m3->mc_top]);
					/* If this node has dupdata, it may need to be reinited
					 * because its data has moved.
					 * If the xcursor was not initd it must be reinited.
					 * Else if node points to a subDB, nothing is needed.
					 * Else (xcursor was initd, not a subDB) needs mc_pg[0] reset.
					 */
					if (node->mn_flags & F_DUPDATA) {
						if (m3->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED) {
							if (!(node->mn_flags & F_SUBDATA))
								m3->mc_xcursor->mx_cursor.mc_pg[0] = NODEDATA(node);
						} else {
							mdb_xcursor_init1(m3, node);
							rc = mdb_cursor_first(&m3->mc_xcursor->mx_cursor, NULL, NULL);
							if (rc)
								goto fail;
						}
					}
					m3->mc_xcursor->mx_cursor.mc_flags |= C_DEL;
				}
			}
		}
	}
	mc->mc_flags |= C_DEL;

fail:
	if (rc)
		mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
	return rc;
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

#ifndef MDB_WBUF
#define MDB_WBUF	(1024*1024)
#endif
#define MDB_EOF		0x10	/**< #mdb_env_copyfd1() is done reading */

	/** State needed for a double-buffering compacting copy. */
typedef struct mdb_copy {
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
} mdb_copy;

	/** Dedicated writer thread for compacting copy. */
THREAD_RET ESECT CALL_CONV
mdb_env_copythr(void *arg)
{
	mdb_copy *my = arg;
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
	buf = ptr = malloc(my->mc_env->me_psize * mc.mc_snum);
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
	my.mc_wbuf[0] = _aligned_malloc(MDB_WBUF*2, env->me_os_psize);
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
		my.mc_wbuf[0] = p;
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
int ESECT
mdb_stat0(MDB_env *env, MDB_db *db, MDB_stat *arg)
{
	arg->ms_psize = env->me_psize;
	arg->ms_depth = db->md_depth;
	arg->ms_branch_pages = db->md_branch_pages;
	arg->ms_leaf_pages = db->md_leaf_pages;
	arg->ms_overflow_pages = db->md_overflow_pages;
	arg->ms_entries = db->md_entries;

	return MDB_SUCCESS;
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

/** Set the default comparison functions for a database.
 * Called immediately after a database is opened to set the defaults.
 * The user can then override them with #mdb_set_compare() or
 * #mdb_set_dupsort().
 * @param[in] txn A transaction handle returned by #mdb_txn_begin()
 * @param[in] dbi A database handle returned by #mdb_dbi_open()
 */
void
mdb_default_cmp(MDB_txn *txn, MDB_dbi dbi)
{
	uint16_t f = txn->mt_dbs[dbi].md_flags;

	txn->mt_dbxs[dbi].md_cmp =
		(f & MDB_REVERSEKEY) ? mdb_cmp_memnr :
		(f & MDB_INTEGERKEY) ? mdb_cmp_cint  : mdb_cmp_memn;

	txn->mt_dbxs[dbi].md_dcmp =
		!(f & MDB_DUPSORT) ? 0 :
		((f & MDB_INTEGERDUP)
		 ? ((f & MDB_DUPFIXED)   ? mdb_cmp_int   : mdb_cmp_cint)
		 : ((f & MDB_REVERSEDUP) ? mdb_cmp_memnr : mdb_cmp_memn));
}

int mdb_dbi_open(MDB_txn *txn, const char *name, unsigned int flags, MDB_dbi *dbi)
{
	MDB_val key, data;
	MDB_dbi i;
	MDB_cursor mc;
	MDB_db dummy;
	int rc, dbflag, exact;
	unsigned int unused = 0, seq;
	char *namedup;
	size_t len;

	if (flags & ~VALID_FLAGS)
		return EINVAL;
	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	/* main DB? */
	if (!name) {
		*dbi = MAIN_DBI;
		if (flags & PERSISTENT_FLAGS) {
			uint16_t f2 = flags & PERSISTENT_FLAGS;
			/* make sure flag changes get committed */
			if ((txn->mt_dbs[MAIN_DBI].md_flags | f2) != txn->mt_dbs[MAIN_DBI].md_flags) {
				txn->mt_dbs[MAIN_DBI].md_flags |= f2;
				txn->mt_flags |= MDB_TXN_DIRTY;
			}
		}
		mdb_default_cmp(txn, MAIN_DBI);
		MDB_TRACE(("%p, (null), %u = %u", txn, flags, MAIN_DBI));
		return MDB_SUCCESS;
	}

	if (txn->mt_dbxs[MAIN_DBI].md_cmp == NULL) {
		mdb_default_cmp(txn, MAIN_DBI);
	}

	/* Is the DB already open? */
	len = strlen(name);
	for (i=CORE_DBS; i<txn->mt_numdbs; i++) {
		if (!txn->mt_dbxs[i].md_name.mv_size) {
			/* Remember this free slot */
			if (!unused) unused = i;
			continue;
		}
		if (len == txn->mt_dbxs[i].md_name.mv_size &&
			!strncmp(name, txn->mt_dbxs[i].md_name.mv_data, len)) {
			*dbi = i;
			return MDB_SUCCESS;
		}
	}

	/* If no free slot and max hit, fail */
	if (!unused && txn->mt_numdbs >= txn->mt_env->me_maxdbs)
		return MDB_DBS_FULL;

	/* Cannot mix named databases with some mainDB flags */
	if (txn->mt_dbs[MAIN_DBI].md_flags & (MDB_DUPSORT|MDB_INTEGERKEY))
		return (flags & MDB_CREATE) ? MDB_INCOMPATIBLE : MDB_NOTFOUND;

	/* Find the DB info */
	dbflag = DB_NEW|DB_VALID|DB_USRVALID;
	exact = 0;
	key.mv_size = len;
	key.mv_data = (void *)name;
	mdb_cursor_init(&mc, txn, MAIN_DBI, NULL);
	rc = mdb_cursor_set(&mc, &key, &data, MDB_SET, &exact);
	if (rc == MDB_SUCCESS) {
		/* make sure this is actually a DB */
		MDB_node *node = NODEPTR(mc.mc_pg[mc.mc_top], mc.mc_ki[mc.mc_top]);
		if ((node->mn_flags & (F_DUPDATA|F_SUBDATA)) != F_SUBDATA)
			return MDB_INCOMPATIBLE;
	} else {
		if (rc != MDB_NOTFOUND || !(flags & MDB_CREATE))
			return rc;
		if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
			return EACCES;
	}

	/* Done here so we cannot fail after creating a new DB */
	if ((namedup = strdup(name)) == NULL)
		return ENOMEM;

	if (rc) {
		/* MDB_NOTFOUND and MDB_CREATE: Create new DB */
		data.mv_size = sizeof(MDB_db);
		data.mv_data = &dummy;
		memset(&dummy, 0, sizeof(dummy));
		dummy.md_root = P_INVALID;
		dummy.md_flags = flags & PERSISTENT_FLAGS;
		WITH_CURSOR_TRACKING(mc,
			rc = _mdb_cursor_put(&mc, &key, &data, F_SUBDATA));
		dbflag |= DB_DIRTY;
	}

	if (rc) {
		free(namedup);
	} else {
		/* Got info, register DBI in this txn */
		unsigned int slot = unused ? unused : txn->mt_numdbs;
		txn->mt_dbxs[slot].md_name.mv_data = namedup;
		txn->mt_dbxs[slot].md_name.mv_size = len;
		txn->mt_dbxs[slot].md_rel = NULL;
		txn->mt_dbflags[slot] = dbflag;
		/* txn-> and env-> are the same in read txns, use
		 * tmp variable to avoid undefined assignment
		 */
		seq = ++txn->mt_env->me_dbiseqs[slot];
		txn->mt_dbiseqs[slot] = seq;

		memcpy(&txn->mt_dbs[slot], data.mv_data, sizeof(MDB_db));
		*dbi = slot;
		mdb_default_cmp(txn, slot);
		if (!unused) {
			txn->mt_numdbs++;
		}
		MDB_TRACE(("%p, %s, %u = %u", txn, name, flags, slot));
	}

	return rc;
}

int ESECT
mdb_stat(MDB_txn *txn, MDB_dbi dbi, MDB_stat *arg)
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

void mdb_dbi_close(MDB_env *env, MDB_dbi dbi)
{
	char *ptr;
	if (dbi < CORE_DBS || dbi >= env->me_maxdbs)
		return;
	ptr = env->me_dbxs[dbi].md_name.mv_data;
	/* If there was no name, this was already closed */
	if (ptr) {
		MDB_TRACE(("%p, %u", env, dbi));
		env->me_dbxs[dbi].md_name.mv_data = NULL;
		env->me_dbxs[dbi].md_name.mv_size = 0;
		env->me_dbflags[dbi] = 0;
		env->me_dbiseqs[dbi]++;
		free(ptr);
	}
}

int mdb_dbi_flags(MDB_txn *txn, MDB_dbi dbi, unsigned int *flags)
{
	/* We could return the flags for the FREE_DBI too but what's the point? */
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;
	*flags = txn->mt_dbs[dbi].md_flags & PERSISTENT_FLAGS;
	return MDB_SUCCESS;
}

/** Add all the DB's pages to the free list.
 * @param[in] mc Cursor on the DB to free.
 * @param[in] subs non-Zero to check for sub-DBs in this DB.
 * @return 0 on success, non-zero on failure.
 */
int
mdb_drop0(MDB_cursor *mc, int subs)
{
	int rc;

	rc = mdb_page_search(mc, NULL, MDB_PS_FIRST);
	if (rc == MDB_SUCCESS) {
		MDB_txn *txn = mc->mc_txn;
		MDB_node *ni;
		MDB_cursor mx;
		unsigned int i;

		/* DUPSORT sub-DBs have no ovpages/DBs. Omit scanning leaves.
		 * This also avoids any P_LEAF2 pages, which have no nodes.
		 * Also if the DB doesn't have sub-DBs and has no overflow
		 * pages, omit scanning leaves.
		 */
		if ((mc->mc_flags & C_SUB) ||
			(!subs && !mc->mc_db->md_overflow_pages))
			mdb_cursor_pop(mc);

		mdb_cursor_copy(mc, &mx);
		while (mc->mc_snum > 0) {
			MDB_page *mp = mc->mc_pg[mc->mc_top];
			unsigned n = NUMKEYS(mp);
			if (IS_LEAF(mp)) {
				for (i=0; i<n; i++) {
					ni = NODEPTR(mp, i);
					if (ni->mn_flags & F_BIGDATA) {
						MDB_page *omp;
						pgno_t pg;
						memcpy(&pg, NODEDATA(ni), sizeof(pg));
						rc = mdb_page_get(mc, pg, &omp, NULL);
						if (rc != 0)
							goto done;
						mdb_cassert(mc, IS_OVERFLOW(omp));
						rc = mdb_midl_append_range(&txn->mt_free_pgs,
							pg, omp->mp_pages);
						if (rc)
							goto done;
						mc->mc_db->md_overflow_pages -= omp->mp_pages;
						if (!mc->mc_db->md_overflow_pages && !subs)
							break;
					} else if (subs && (ni->mn_flags & F_SUBDATA)) {
						mdb_xcursor_init1(mc, ni);
						rc = mdb_drop0(&mc->mc_xcursor->mx_cursor, 0);
						if (rc)
							goto done;
					}
				}
				if (!subs && !mc->mc_db->md_overflow_pages)
					goto pop;
			} else {
				if ((rc = mdb_midl_need(&txn->mt_free_pgs, n)) != 0)
					goto done;
				for (i=0; i<n; i++) {
					pgno_t pg;
					ni = NODEPTR(mp, i);
					pg = NODEPGNO(ni);
					/* free it */
					mdb_midl_xappend(txn->mt_free_pgs, pg);
				}
			}
			if (!mc->mc_top)
				break;
			mc->mc_ki[mc->mc_top] = i;
			rc = mdb_cursor_sibling(mc, 1);
			if (rc) {
				if (rc != MDB_NOTFOUND)
					goto done;
				/* no more siblings, go back to beginning
				 * of previous level.
				 */
pop:
				mdb_cursor_pop(mc);
				mc->mc_ki[0] = 0;
				for (i=1; i<mc->mc_snum; i++) {
					mc->mc_ki[i] = 0;
					mc->mc_pg[i] = mx.mc_pg[i];
				}
			}
		}
		/* free it */
		rc = mdb_midl_append(&txn->mt_free_pgs, mc->mc_db->md_root);
done:
		if (rc)
			txn->mt_flags |= MDB_TXN_ERROR;
		/* drop refcount for mx's pages */
		MDB_CURSOR_UNREF(&mx, 0);
	} else if (rc == MDB_NOTFOUND) {
		rc = MDB_SUCCESS;
	}
	mc->mc_flags &= ~C_INITIALIZED;
	return rc;
}

int mdb_drop(MDB_txn *txn, MDB_dbi dbi, int del)
{
	MDB_cursor *mc, *m2;
	int rc;

	if ((unsigned)del > 1 || !TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
		return EACCES;

	if (TXN_DBI_CHANGED(txn, dbi))
		return MDB_BAD_DBI;

	rc = mdb_cursor_open(txn, dbi, &mc);
	if (rc)
		return rc;

	MDB_TRACE(("%u, %d", dbi, del));
	rc = mdb_drop0(mc, mc->mc_db->md_flags & MDB_DUPSORT);
	/* Invalidate the dropped DB's cursors */
	for (m2 = txn->mt_cursors[dbi]; m2; m2 = m2->mc_next)
		m2->mc_flags &= ~(C_INITIALIZED|C_EOF);
	if (rc)
		goto leave;

	/* Can't delete the main DB */
	if (del && dbi >= CORE_DBS) {
		rc = mdb_del0(txn, MAIN_DBI, &mc->mc_dbx->md_name, NULL, F_SUBDATA);
		if (!rc) {
			txn->mt_dbflags[dbi] = DB_STALE;
			mdb_dbi_close(txn->mt_env, dbi);
		} else {
			txn->mt_flags |= MDB_TXN_ERROR;
		}
	} else {
		/* reset the DB record, mark it dirty */
		txn->mt_dbflags[dbi] |= DB_DIRTY;
		txn->mt_dbs[dbi].md_depth = 0;
		txn->mt_dbs[dbi].md_branch_pages = 0;
		txn->mt_dbs[dbi].md_leaf_pages = 0;
		txn->mt_dbs[dbi].md_overflow_pages = 0;
		txn->mt_dbs[dbi].md_entries = 0;
		txn->mt_dbs[dbi].md_root = P_INVALID;

		txn->mt_flags |= MDB_TXN_DIRTY;
	}
leave:
	mdb_cursor_close(mc);
	return rc;
}

int mdb_set_compare(MDB_txn *txn, MDB_dbi dbi, MDB_cmp_func *cmp)
{
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	txn->mt_dbxs[dbi].md_cmp = cmp;
	return MDB_SUCCESS;
}

int mdb_set_dupsort(MDB_txn *txn, MDB_dbi dbi, MDB_cmp_func *cmp)
{
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	txn->mt_dbxs[dbi].md_dcmp = cmp;
	return MDB_SUCCESS;
}

int mdb_set_relfunc(MDB_txn *txn, MDB_dbi dbi, MDB_rel_func *rel)
{
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	txn->mt_dbxs[dbi].md_rel = rel;
	return MDB_SUCCESS;
}

int mdb_set_relctx(MDB_txn *txn, MDB_dbi dbi, void *ctx)
{
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	txn->mt_dbxs[dbi].md_relctx = ctx;
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_maxkeysize(MDB_env *env)
{
	return ENV_MAXKEY(env);
}

int ESECT
mdb_reader_list(MDB_env *env, MDB_msg_func *func, void *ctx)
{
	unsigned int i, rdrs;
	MDB_reader *mr;
	char buf[64];
	int rc = 0, first = 1;

	if (!env || !func)
		return -1;
	if (!env->me_txns) {
		return func("(no reader locks)\n", ctx);
	}
	rdrs = env->me_txns->mti_numreaders;
	mr = env->me_txns->mti_readers;
	for (i=0; i<rdrs; i++) {
		if (mr[i].mr_pid) {
			txnid_t	txnid = mr[i].mr_txnid;
			sprintf(buf, txnid == (txnid_t)-1 ?
				"%10d %"Z"x -\n" : "%10d %"Z"x %"Yu"\n",
				(int)mr[i].mr_pid, (size_t)mr[i].mr_tid, txnid);
			if (first) {
				first = 0;
				rc = func("    pid     thread     txnid\n", ctx);
				if (rc < 0)
					break;
			}
			rc = func(buf, ctx);
			if (rc < 0)
				break;
		}
	}
	if (first) {
		rc = func("(no active readers)\n", ctx);
	}
	return rc;
}

/** Insert pid into list if not already present.
 * return -1 if already present.
 */
int ESECT
mdb_pid_insert(MDB_PID_T *ids, MDB_PID_T pid)
{
	/* binary search of pid in list */
	unsigned base = 0;
	unsigned cursor = 1;
	int val = 0;
	unsigned n = ids[0];

	while( 0 < n ) {
		unsigned pivot = n >> 1;
		cursor = base + pivot + 1;
		val = pid - ids[cursor];

		if( val < 0 ) {
			n = pivot;

		} else if ( val > 0 ) {
			base = cursor;
			n -= pivot + 1;

		} else {
			/* found, so it's a duplicate */
			return -1;
		}
	}

	if( val > 0 ) {
		++cursor;
	}
	ids[0]++;
	for (n = ids[0]; n > cursor; n--)
		ids[n] = ids[n-1];
	ids[n] = pid;
	return 0;
}

int ESECT
mdb_reader_check(MDB_env *env, int *dead)
{
	if (!env)
		return EINVAL;
	if (dead)
		*dead = 0;
	return env->me_txns ? mdb_reader_check0(env, 0, dead) : MDB_SUCCESS;
}

/** As #mdb_reader_check(). \b rlocked is set if caller locked #me_rmutex. */
int ESECT
mdb_reader_check0(MDB_env *env, int rlocked, int *dead)
{
	mdb_mutexref_t rmutex = rlocked ? NULL : env->me_rmutex;
	unsigned int i, j, rdrs;
	MDB_reader *mr;
	MDB_PID_T *pids, pid;
	int rc = MDB_SUCCESS, count = 0;

	rdrs = env->me_txns->mti_numreaders;
	pids = malloc((rdrs+1) * sizeof(MDB_PID_T));
	if (!pids)
		return ENOMEM;
	pids[0] = 0;
	mr = env->me_txns->mti_readers;
	for (i=0; i<rdrs; i++) {
		pid = mr[i].mr_pid;
		if (pid && pid != env->me_pid) {
			if (mdb_pid_insert(pids, pid) == 0) {
				if (!mdb_reader_pid(env, Pidcheck, pid)) {
					/* Stale reader found */
					j = i;
					if (rmutex) {
						if ((rc = LOCK_MUTEX0(rmutex)) != 0) {
							if ((rc = mdb_mutex_failed(env, rmutex, rc)))
								break;
							rdrs = 0; /* the above checked all readers */
						} else {
							/* Recheck, a new process may have reused pid */
							if (mdb_reader_pid(env, Pidcheck, pid))
								j = rdrs;
						}
					}
					for (; j<rdrs; j++)
							if (mr[j].mr_pid == pid) {
								DPRINTF(("clear stale reader pid %u txn %"Yd,
									(unsigned) pid, mr[j].mr_txnid));
								mr[j].mr_pid = 0;
								count++;
							}
					if (rmutex)
						UNLOCK_MUTEX(rmutex);
				}
			}
		}
	}
	free(pids);
	if (dead)
		*dead = count;
	return rc;
}

#ifdef MDB_ROBUST_SUPPORTED
/** Handle #LOCK_MUTEX0() failure.
 * Try to repair the lock file if the mutex owner died.
 * @param[in] env	the environment handle
 * @param[in] mutex	LOCK_MUTEX0() mutex
 * @param[in] rc	LOCK_MUTEX0() error (nonzero)
 * @return 0 on success with the mutex locked, or an error code on failure.
 */
int ESECT
mdb_mutex_failed(MDB_env *env, mdb_mutexref_t mutex, int rc)
{
	int rlocked, rc2;
	MDB_meta *meta;

	if (rc == MDB_OWNERDEAD) {
		/* We own the mutex. Clean up after dead previous owner. */
		rc = MDB_SUCCESS;
		rlocked = (mutex == env->me_rmutex);
		if (!rlocked) {
			/* Keep mti_txnid updated, otherwise next writer can
			 * overwrite data which latest meta page refers to.
			 */
			meta = mdb_env_pick_meta(env);
			env->me_txns->mti_txnid = meta->mm_txnid;
			/* env is hosed if the dead thread was ours */
			if (env->me_txn) {
				env->me_flags |= MDB_FATAL_ERROR;
				env->me_txn = NULL;
				rc = MDB_PANIC;
			}
		}
		DPRINTF(("%cmutex owner died, %s", (rlocked ? 'r' : 'w'),
			(rc ? "this process' env is hosed" : "recovering")));
		rc2 = mdb_reader_check0(env, rlocked, NULL);
		if (rc2 == 0)
			rc2 = mdb_mutex_consistent(mutex);
		if (rc || (rc = rc2)) {
			DPRINTF(("LOCK_MUTEX recovery failed, %s", mdb_strerror(rc)));
			UNLOCK_MUTEX(mutex);
		}
	} else {
#ifdef _WIN32
		rc = ErrCode();
#endif
		DPRINTF(("LOCK_MUTEX failed, %s", mdb_strerror(rc)));
	}

	return rc;
}
#endif	/* MDB_ROBUST_SUPPORTED */

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
/** @} */

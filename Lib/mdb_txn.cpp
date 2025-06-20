#include "mdb_txn.h"

#include "mdb_page.h"
#include "mdb_env.h"
#include "mdb_lock.h"
#include "mdb_debug.h"
#include "mdb_cursor.h"
#include "mdb_db.h"

	// Nested transaction
struct MDB_ntxn {
	MDB_txn		mnt_txn;		//< the transaction
	MDB_pgstate	mnt_pgstate;	//< parent transaction's saved freestate
};

// Common code for #mdb_txn_begin() and #mdb_txn_renew().
// [in] txn the transaction handle to initialize
//  0 on success, non-zero on failure.
int
mdb_txn_renew0(MDB_txn *txn)
{
	MDB_env *env = txn->mt_env;
	MDB_txninfo *ti = env->me_txns;
	MDB_meta *meta;
	unsigned int i, nr, flags = txn->mt_flags;
	uint16_t x;
	int rc, new_notls = 0;

	if ((flags &= MDB_TXN_RDONLY) != 0)
	{
		if (!ti)
		{
			meta = mdb_env_pick_meta(env);
			txn->mt_txnid = meta->mm_txnid;
			txn->mt_u.reader = NULL;
		}
		else
		{
			MDB_reader *r = (MDB_reader*) ((env->me_flags & MDB_NOTLS) ? txn->mt_u.reader : pthread_getspecific(env->me_txkey));
			if (r)
			{
				if (r->mr_pid != env->me_pid || r->mr_txnid != (txnid_t)-1)
					return MDB_BAD_RSLOT;
			}
			else
			{
				MDB_PID_T pid = env->me_pid;
				MDB_THR_T tid = pthread_self();
				mdb_mutexref_t rmutex = env->me_rmutex;

				if (!env->me_live_reader)
				{
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
				if (i == env->me_maxreaders)
				{
					UNLOCK_MUTEX(rmutex);
					return MDB_READERS_FULL;
				}
				r = &ti->mti_readers[i];
				// Claim the reader slot, carefully since other code
				// uses the reader table un-mutexed: First reset the
				// slot, next publish it in mti_numreaders.  After
				// that, it is safe for mdb_env_close() to touch it.
				// When it will be closed, we can finally claim it.
				r->mr_pid = 0;
				r->mr_txnid = (txnid_t)-1;
				r->mr_tid = tid;
				if (i == nr)
					ti->mti_numreaders = ++nr;
				env->me_close_readers = nr;
				r->mr_pid = pid;
				UNLOCK_MUTEX(rmutex);

				new_notls = (env->me_flags & MDB_NOTLS);
				if (!new_notls && (rc=pthread_setspecific(env->me_txkey, r)))
				{
					r->mr_pid = 0;
					return rc;
				}
			}
			do /* LY: Retry on a race, ITS#7970. */
				r->mr_txnid = ti->mti_txnid;
			while(r->mr_txnid != ti->mti_txnid);
			if (!r->mr_txnid && (env->me_flags & MDB_RDONLY))
			{
				meta = mdb_env_pick_meta(env);
				r->mr_txnid = meta->mm_txnid;
			}
			else
			{
				meta = env->me_metas[r->mr_txnid & 1];
			}
			txn->mt_txnid = r->mr_txnid;
			txn->mt_u.reader = r;
		}

	}
	else
	{
		// Not yet touching txn == env->me_txn0, it may be active
		if (ti)
		{
			if (LOCK_MUTEX(rc, env, env->me_wmutex))
				return rc;
			txn->mt_txnid = ti->mti_txnid;
			meta = env->me_metas[txn->mt_txnid & 1];
		}
		else
		{
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

	// Copy the DB info and flags
	memcpy(txn->mt_dbs, meta->mm_dbs, CORE_DBS * sizeof(MDB_db));

	// Moved to here to avoid a data race in read TXNs
	txn->mt_next_pgno = meta->mm_last_pg+1;
	txn->mt_flags = flags;

	// Setup db info
	txn->mt_numdbs = env->me_numdbs;
	for (i=CORE_DBS; i<txn->mt_numdbs; i++)
	{
		x = env->me_dbflags[i];
		txn->mt_dbs[i].md_flags = x & PERSISTENT_FLAGS;
		txn->mt_dbflags[i] = (x & MDB_VALID) ? DB_VALID|DB_USRVALID|DB_STALE : 0;
	}
	txn->mt_dbflags[MAIN_DBI] = DB_VALID|DB_USRVALID;
	txn->mt_dbflags[FREE_DBI] = DB_VALID;

	if (env->me_flags & MDB_FATAL_ERROR)
	{
		DPUTS("environment had fatal error, must shutdown!");
		rc = MDB_PANIC;
	}
	else if (env->me_maxpg < txn->mt_next_pgno)
	{
		rc = MDB_MAP_RESIZED;
	}
	else
	{
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
	if (rc == MDB_SUCCESS)
	{
		DPRINTF(("renew txn %" Yu "%c %p on mdbenv %p, root page %" Yu,
			txn->mt_txnid, (txn->mt_flags & MDB_TXN_RDONLY) ? 'r' : 'w',
			(void *)txn, (void *)txn->mt_env, txn->mt_dbs[MAIN_DBI].md_root));
	}
	return rc;
}

// Back up parent txn's cursors, then grab the originals for tracking
static int mdb_cursor_shadow(MDB_txn *src, MDB_txn *dst)
{
	MDB_cursor *mc, *bk;
	MDB_xcursor *mx;
	size_t size;
	int i;

	for (i = src->mt_numdbs; --i >= 0; )
	{
		if ((mc = src->mt_cursors[i]) != NULL)
		{
			size = sizeof(MDB_cursor);
			if (mc->mc_xcursor)
				size += sizeof(MDB_xcursor);
			for (; mc; mc = bk->mc_next)
			{
				bk = (MDB_cursor *)malloc(size);
				if (!bk)
					return ENOMEM;
				*bk = *mc;
				mc->mc_backup = bk;
				mc->mc_db = &dst->mt_dbs[i];
				// Kill pointers into src to reduce abuse: The
				// user may not use mc until dst ends. But we need a valid
				// txn pointer here for cursor fixups to keep working.
				mc->mc_txn    = dst;
				mc->mc_dbflag = &dst->mt_dbflags[i];
				if ((mx = mc->mc_xcursor) != NULL)
				{
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

	if (parent)
	{
		// Nested transactions: Max 1 child, write txns only, no writemap
		flags |= parent->mt_flags;
		if (flags & (MDB_RDONLY|MDB_WRITEMAP|MDB_TXN_BLOCKED))
		{
			return (parent->mt_flags & MDB_TXN_RDONLY) ? EINVAL : MDB_BAD_TXN;
		}
		// Child txns save MDB_pgstate and use own copy of cursors
		size = env->me_maxdbs * (sizeof(MDB_db)+sizeof(MDB_cursor *)+1);
		size += tsize = sizeof(MDB_ntxn);
	}
	else if (flags & MDB_RDONLY)
	{
		size = env->me_maxdbs * (sizeof(MDB_db)+1);
		size += tsize = sizeof(MDB_txn);
	}
	else
	{
		// Reuse preallocated write txn. However, do not touch it until
		// mdb_txn_renew0() succeeds, since it currently may be active.
		txn = env->me_txn0;
		goto renew;
	}
	if ((txn = (MDB_txn *)calloc(1, size)) == NULL)
	{
		DPRINTF(("calloc: %s", strerror(errno)));
		return ENOMEM;
	}
	txn->mt_dbxs = env->me_dbxs;	// static
	txn->mt_dbs = (MDB_db *) ((char *)txn + tsize);
	txn->mt_dbflags = (unsigned char *)txn + size - env->me_maxdbs;
	txn->mt_flags = flags;
	txn->mt_env = env;

	if (parent)
	{
		unsigned int i;
		txn->mt_cursors = (MDB_cursor **)(txn->mt_dbs + env->me_maxdbs);
		txn->mt_dbiseqs = parent->mt_dbiseqs;
		txn->mt_u.dirty_list = (MDB_ID2L)malloc(sizeof(MDB_ID2)*MDB_IDL_UM_SIZE);
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
		// Copy parent's mt_dbflags, but clear DB_NEW
		for (i=0; i<txn->mt_numdbs; i++)
			txn->mt_dbflags[i] = parent->mt_dbflags[i] & ~DB_NEW;
		rc = 0;
		ntxn = (MDB_ntxn *)txn;
		ntxn->mnt_pgstate = env->me_pgstate; // save parent me_pghead & co
		if (env->me_pghead)
		{
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
	}
	else
	{ /* MDB_RDONLY */
		txn->mt_dbiseqs = env->me_dbiseqs;
renew:
		rc = mdb_txn_renew0(txn);
	}
	if (rc)
	{
		if (txn != env->me_txn0)
		{
			free(txn);
		}
	}
	else
	{
		txn->mt_flags |= flags;	/* could not change txn=me_txn0 earlier */
		*ret = txn;
		DPRINTF(("begin txn %" Yu "%c %p on mdbenv %p, root page %" Yu,
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

// Export or close DBI handles opened in this txn.
static void mdb_dbis_update(MDB_txn *txn, int keep)
{
	int i;
	MDB_dbi n = txn->mt_numdbs;
	MDB_env *env = txn->mt_env;
	unsigned char *tdbflags = txn->mt_dbflags;

	for (i = n; --i >= CORE_DBS;)
	{
		if (tdbflags[i] & DB_NEW)
		{
			if (keep)
			{
				env->me_dbflags[i] = txn->mt_dbs[i].md_flags | MDB_VALID;
			}
			else
			{
				char *ptr = (char*) env->me_dbxs[i].md_name.mv_data;
				if (ptr)
				{
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

// Close this write txn's cursors, give parent txn's cursors back to parent.
// [in] txn the transaction handle.
// [in] merge true to keep changes to parent cursors, false to revert.
//  0 on success, non-zero on failure.
static void mdb_cursors_close(MDB_txn *txn, unsigned merge)
{
	MDB_cursor **cursors = txn->mt_cursors, *mc, *next, *bk;
	MDB_xcursor *mx;
	int i;

	for (i = txn->mt_numdbs; --i >= 0; )
	{
		for (mc = cursors[i]; mc; mc = next)
		{
			next = mc->mc_next;
			if ((bk = mc->mc_backup) != NULL)
			{
				if (merge)
				{
					// Commit changes to parent txn
					mc->mc_next = bk->mc_next;
					mc->mc_backup = bk->mc_backup;
					mc->mc_txn = bk->mc_txn;
					mc->mc_db = bk->mc_db;
					mc->mc_dbflag = bk->mc_dbflag;
					if ((mx = mc->mc_xcursor) != NULL)
						mx->mx_cursor.mc_txn = bk->mc_txn;
				}
				else
				{
					// Abort nested txn
					*mc = *bk;
					if ((mx = mc->mc_xcursor) != NULL)
						*mx = *(MDB_xcursor *)(bk+1);
				}
				mc = bk;
			}
			// Only malloced cursors are permanently tracked.
			free(mc);
		}
		cursors[i] = NULL;
	}
}

// Return all dirty pages to dpage list
static void mdb_dlist_free(MDB_txn *txn)
{
	MDB_env *env = txn->mt_env;
	MDB_ID2L dl = txn->mt_u.dirty_list;
	unsigned i, n = dl[0].mid;

	for (i = 1; i <= n; i++)
	{
		mdb_dpage_free(env, (MDB_page*)dl[i].mptr);
	}
	dl[0].mid = 0;
}

#define MDB_END_NAMES {"committed", "empty-commit", "abort", "reset", \
	"reset-tmp", "fail-begin", "fail-beginchild"}

// End a transaction, except successful commit of a nested transaction.
// May be called twice for readonly txns: First reset it, then abort.
// [in] txn the transaction handle to end
// [in] mode why and how to end the transaction
void
mdb_txn_end(MDB_txn *txn, unsigned mode)
{
	MDB_env	*env = txn->mt_env;
#if MDB_DEBUG
	static const char *const names[] = MDB_END_NAMES;
#endif

	// Export or close DBI handles opened in this txn
	mdb_dbis_update(txn, mode & MDB_END_UPDATE);

	DPRINTF(("%s txn %" Yu "%c %p on mdbenv %p, root page %" Yu,
		names[mode & MDB_END_OPMASK],
		txn->mt_txnid, (txn->mt_flags & MDB_TXN_RDONLY) ? 'r' : 'w',
		(void *) txn, (void *)env, txn->mt_dbs[MAIN_DBI].md_root));

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
	{
		if (txn->mt_u.reader)
		{
			txn->mt_u.reader->mr_txnid = (txnid_t)-1;
			if (!(env->me_flags & MDB_NOTLS))
			{
				txn->mt_u.reader = NULL; /* txn does not own reader */
			}
			else if (mode & MDB_END_SLOT)
			{
				txn->mt_u.reader->mr_pid = 0;
				txn->mt_u.reader = NULL;
			} /* else txn owns the slot until it does MDB_END_SLOT */
		}
		txn->mt_numdbs = 0;		/* prevent further DBI activity */
		txn->mt_flags |= MDB_TXN_FINISHED;

	}
	else if (!F_ISSET(txn->mt_flags, MDB_TXN_FINISHED))
	{
		pgno_t *pghead = env->me_pghead;

		if (!(mode & MDB_END_UPDATE)) // !(already closed cursors)
			mdb_cursors_close(txn, 0);
		if (!(env->me_flags & MDB_WRITEMAP))
		{
			mdb_dlist_free(txn);
		}

		txn->mt_numdbs = 0;
		txn->mt_flags = MDB_TXN_FINISHED;

		if (!txn->mt_parent)
		{
			mdb_midl_shrink(&txn->mt_free_pgs);
			env->me_free_pgs = txn->mt_free_pgs;
			// me_pgstate:
			env->me_pghead = NULL;
			env->me_pglast = 0;

			env->me_txn = NULL;
			mode = 0;	// txn == env->me_txn0, do not free() it

			/* The writer mutex was locked in mdb_txn_begin. */
			if (env->me_txns)
				UNLOCK_MUTEX(env->me_wmutex);
		}
		else
		{
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

	// This call is only valid for read-only txns
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

// Save the freelist as of this transaction to the freeDB.
// This changes the freelist. Keep trying until it stabilizes.
int
mdb_freelist_save(MDB_txn *txn)
{
	// env->me_pghead[] can grow and shrink during this call.
	// env->me_pglast and txn->mt_free_pgs[] can only grow.
	// Page numbers cannot disappear from txn->mt_free_pgs[].
	MDB_cursor mc;
	MDB_env	*env = txn->mt_env;
	int rc, maxfree_1pg = env->me_maxfree_1pg, more = 1;
	txnid_t	pglast = 0, head_id = 0;
	pgno_t	freecnt = 0, *free_pgs, *mop;
	ssize_t	head_room = 0, total_room = 0, mop_len, clean_limit;

	mdb_cursor_init(&mc, txn, FREE_DBI, NULL);

	if (env->me_pghead)
	{
		// Make sure first page of freeDB is touched and on freelist
		rc = mdb_page_search(&mc, NULL, MDB_PS_FIRST|MDB_PS_MODIFY);
		if (rc && rc != MDB_NOTFOUND)
			return rc;
	}

	if (!env->me_pghead && txn->mt_loose_pgs)
	{
		// Put loose page numbers in mt_free_pgs, since
		// we may be unable to return them to me_pghead.
		MDB_page *mp = txn->mt_loose_pgs;
		MDB_ID2 *dl = txn->mt_u.dirty_list;
		unsigned x;
		if ((rc = mdb_midl_need(&txn->mt_free_pgs, txn->mt_loose_count)) != 0)
			return rc;
		for (; mp; mp = NEXT_LOOSE_PAGE(mp))
		{
			mdb_midl_xappend(txn->mt_free_pgs, mp->mp_pgno);
			// must also remove from dirty list
			if (txn->mt_flags & MDB_TXN_WRITEMAP)
			{
				for (x=1; x<=dl[0].mid; x++)
					if (dl[x].mid == mp->mp_pgno)
						break;
				mdb_tassert(txn, x <= dl[0].mid);
			}
			else
			{
				x = mdb_mid2l_search(dl, mp->mp_pgno);
				mdb_tassert(txn, dl[x].mid == mp->mp_pgno);
				mdb_dpage_free(env, mp);
			}
			dl[x].mptr = NULL;
		}
		{
			// squash freed slots out of the dirty list
			unsigned y;
			for (y=1; dl[y].mptr && y <= dl[0].mid; y++);
			if (y <= dl[0].mid)
			{
				for(x=y, y++;;)
				{
					while (!dl[y].mptr && y <= dl[0].mid) y++;
					if (y > dl[0].mid) break;
					dl[x++] = dl[y++];
				}
				dl[0].mid = x-1;
			}
			else
			{
				// all slots freed
				dl[0].mid = 0;
			}
		}
		txn->mt_loose_pgs = NULL;
		txn->mt_loose_count = 0;
	}

	// MDB_RESERVE cancels meminit in ovpage malloc (when no WRITEMAP)
	clean_limit = (env->me_flags & (MDB_NOMEMINIT|MDB_WRITEMAP))
		? SSIZE_MAX : maxfree_1pg;

	for (;;)
	{
		// Come back here after each Put() in case freelist changed
		MDB_val key, data;
		pgno_t *pgs;
		ssize_t j;

		// If using records from freeDB which we have not yet
		// deleted, delete them and any we reserved for me_pghead.
		while (pglast < env->me_pglast)
		{
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

		// Save the IDL of pages freed by this txn, to a single record
		if (freecnt < txn->mt_free_pgs[0])
		{
			if (!freecnt)
			{
				// Make sure last page of freeDB is touched and on freelist
				rc = mdb_page_search(&mc, NULL, MDB_PS_LAST|MDB_PS_MODIFY);
				if (rc && rc != MDB_NOTFOUND)
					return rc;
			}
			free_pgs = txn->mt_free_pgs;
			// Write to last page of freeDB
			key.mv_size = sizeof(txn->mt_txnid);
			key.mv_data = &txn->mt_txnid;
			do
			{
				freecnt = free_pgs[0];
				data.mv_size = MDB_IDL_SIZEOF(free_pgs);
				rc = _mdb_cursor_put(&mc, &key, &data, MDB_RESERVE);
				if (rc)
					return rc;
				// Retry if mt_free_pgs[] grew during the Put()
				free_pgs = txn->mt_free_pgs;
			} while (freecnt < free_pgs[0]);
			mdb_midl_sort(free_pgs);
			memcpy(data.mv_data, free_pgs, data.mv_size);
#if (MDB_DEBUG) > 1
			{
				unsigned int i = free_pgs[0];
				DPRINTF(("IDL write txn %" Yu " root %" Yu " num %u",
					txn->mt_txnid, txn->mt_dbs[FREE_DBI].md_root, i));
				for (; i; i--)
					DPRINTF(("IDL %" Yu, free_pgs[i]));
			}
#endif
			continue;
		}

		mop = env->me_pghead;
		mop_len = (mop ? mop[0] : 0) + txn->mt_loose_count;

		// Reserve records for me_pghead[]. Split it if multi-page,
		// to avoid searching freeDB for a page range. Use keys in
		// range [1,me_pglast]: Smaller than txnid of oldest reader.
		if (total_room >= mop_len)
		{
			if (total_room == mop_len || --more < 0)
				break;
		}
		else if (head_room >= maxfree_1pg && head_id > 1)
		{
			// Keep current record (overflow page), add a new one
			head_id--;
			head_room = 0;
		}
		// (Re)write {key = head_id, IDL length = head_room}
		total_room -= head_room;
		head_room = mop_len - total_room;
		if (head_room > maxfree_1pg && head_id > 1)
		{
			// Overflow multi-page for part of me_pghead
			head_room /= head_id; // amortize page sizes
			head_room += maxfree_1pg - head_room % (maxfree_1pg + 1);
		}
		else if (head_room < 0)
		{
			// Rare case, not bothering to delete this record
			head_room = 0;
		}
		key.mv_size = sizeof(head_id);
		key.mv_data = &head_id;
		data.mv_size = (head_room + 1) * sizeof(pgno_t);
		rc = _mdb_cursor_put(&mc, &key, &data, MDB_RESERVE);
		if (rc)
			return rc;
		// IDL is initially empty, zero out at least the length
		pgs = (pgno_t *)data.mv_data;
		j = head_room > clean_limit ? head_room : 0;
		do
		{
			pgs[j] = 0;
		} while (--j >= 0);
		total_room += head_room;
	}

	// Return loose page numbers to me_pghead, though usually none are
	// left at this point.  The pages themselves remain in dirty_list.
	if (txn->mt_loose_pgs)
	{
		MDB_page *mp = txn->mt_loose_pgs;
		unsigned count = txn->mt_loose_count;
		MDB_IDL loose;
		// Room for loose pages + temp IDL with same
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

	// Fill in the reserved me_pghead records
	rc = MDB_SUCCESS;
	if (mop_len)
	{
		MDB_val key, data;

		mop += mop_len;
		rc = mdb_cursor_first(&mc, &key, &data);
		for (; !rc; rc = mdb_cursor_next(&mc, &key, &data, MDB_NEXT))
		{
			txnid_t id = *(txnid_t *)key.mv_data;
			ssize_t	len = (ssize_t)(data.mv_size / sizeof(MDB_ID)) - 1;
			MDB_ID save;

			mdb_tassert(txn, len >= 0 && id <= env->me_pglast);
			key.mv_data = &id;
			if (len > mop_len)
			{
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

int
_mdb_txn_commit(MDB_txn *txn)
{
	int		rc;
	unsigned int i, end_mode;
	MDB_env	*env;

	if (txn == NULL)
		return EINVAL;

	// mdb_txn_end() mode for a commit which writes nothing
	end_mode = MDB_END_EMPTY_COMMIT|MDB_END_UPDATE|MDB_END_SLOT|MDB_END_FREE;

	if (txn->mt_child)
	{
		rc = _mdb_txn_commit(txn->mt_child);
		if (rc)
			goto fail;
	}

	env = txn->mt_env;

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
	{
		goto done;
	}

	if (txn->mt_flags & (MDB_TXN_FINISHED|MDB_TXN_ERROR))
	{
		DPUTS("txn has failed/finished, can't commit");
		if (txn->mt_parent)
			txn->mt_parent->mt_flags |= MDB_TXN_ERROR;
		rc = MDB_BAD_TXN;
		goto fail;
	}

	if (txn->mt_parent)
	{
		MDB_txn *parent = txn->mt_parent;
		MDB_page **lp;
		MDB_ID2L dst, src;
		MDB_IDL pspill;
		unsigned x, y, len, ps_len;

		// Append our free list to parent's
		rc = mdb_midl_append_list(&parent->mt_free_pgs, txn->mt_free_pgs);
		if (rc)
			goto fail;
		mdb_midl_free(txn->mt_free_pgs);
		// Failures after this must either undo the changes
		// to the parent or set MDB_TXN_ERROR in the parent.

		parent->mt_next_pgno = txn->mt_next_pgno;
		parent->mt_flags = txn->mt_flags;

		// Merge our cursors into parent's and close them
		mdb_cursors_close(txn, 1);

		// Update parent's DB table.
		memcpy(parent->mt_dbs, txn->mt_dbs, txn->mt_numdbs * sizeof(MDB_db));
		parent->mt_numdbs = txn->mt_numdbs;
		parent->mt_dbflags[FREE_DBI] = txn->mt_dbflags[FREE_DBI];
		parent->mt_dbflags[MAIN_DBI] = txn->mt_dbflags[MAIN_DBI];
		for (i=CORE_DBS; i<txn->mt_numdbs; i++)
		{
			// preserve parent's DB_NEW status
			x = parent->mt_dbflags[i] & DB_NEW;
			parent->mt_dbflags[i] = txn->mt_dbflags[i] | x;
		}

		dst = parent->mt_u.dirty_list;
		src = txn->mt_u.dirty_list;
		// Remove anything in our dirty list from parent's spill list
		if ((pspill = parent->mt_spill_pgs) && (ps_len = pspill[0]))
		{
			x = y = ps_len;
			pspill[0] = (pgno_t)-1;
			// Mark our dirty pages as deleted in parent spill list
			for (i=0, len=src[0].mid; ++i <= len; )
			{
				MDB_ID pn = src[i].mid << 1;
				while (pn > pspill[x])
					x--;
				if (pn == pspill[x])
				{
					pspill[x] = 1;
					y = --x;
				}
			}
			// Squash deleted pagenums if we deleted any
			for (x=y; ++x <= ps_len; )
				if (!(pspill[x] & 1))
					pspill[++y] = pspill[x];
			pspill[0] = y;
		}

		// Remove anything in our spill list from parent's dirty list
		if (txn->mt_spill_pgs && txn->mt_spill_pgs[0])
		{
			for (i=1; i<=txn->mt_spill_pgs[0]; i++)
			{
				MDB_ID pn = txn->mt_spill_pgs[i];
				if (pn & 1)
					continue;	// deleted spillpg
				pn >>= 1;
				y = mdb_mid2l_search(dst, pn);
				if (y <= dst[0].mid && dst[y].mid == pn)
				{
					free(dst[y].mptr);
					while (y < dst[0].mid)
					{
						dst[y] = dst[y+1];
						y++;
					}
					dst[0].mid--;
				}
			}
		}

		// Find len = length of merging our dirty list with parent's
		x = dst[0].mid;
		dst[0].mid = 0;		// simplify loops
		if (parent->mt_parent)
		{
			len = x + src[0].mid;
			y = mdb_mid2l_search(src, dst[x].mid + 1) - 1;
			for (i = x; y && i; y--)
			{
				pgno_t yp = src[y].mid;
				while (yp < dst[i].mid)
					i--;
				if (yp == dst[i].mid)
				{
					i--;
					len--;
				}
			}
		}
		else
		{ // Simplify the above for single-ancestor case
			len = MDB_IDL_UM_MAX - txn->mt_dirty_room;
		}
		// Merge our dirty list with parent's
		y = src[0].mid;
		for (i = len; y; dst[i--] = src[y--])
		{
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
		if (txn->mt_spill_pgs)
		{
			if (parent->mt_spill_pgs)
			{
				// TODO: Prevent failure here, so parent does not fail
				rc = mdb_midl_append_list(&parent->mt_spill_pgs, txn->mt_spill_pgs);
				if (rc)
					parent->mt_flags |= MDB_TXN_ERROR;
				mdb_midl_free(txn->mt_spill_pgs);
				mdb_midl_sort(parent->mt_spill_pgs);
			}
			else
			{
				parent->mt_spill_pgs = txn->mt_spill_pgs;
			}
		}

		// Append our loose page list to parent's
		for (lp = &parent->mt_loose_pgs; *lp; lp = &NEXT_LOOSE_PAGE(*lp))
			;
		*lp = txn->mt_loose_pgs;
		parent->mt_loose_count += txn->mt_loose_count;

		parent->mt_child = NULL;
		mdb_midl_free(((MDB_ntxn *)txn)->mnt_pgstate.mf_pghead);
		free(txn);
		return rc;
	}

	if (txn != env->me_txn)
	{
		DPUTS("attempt to commit unknown transaction");
		rc = EINVAL;
		goto fail;
	}

	mdb_cursors_close(txn, 0);

	if (!txn->mt_u.dirty_list[0].mid &&
		!(txn->mt_flags & (MDB_TXN_DIRTY|MDB_TXN_SPILLS)))
		goto done;

	DPRINTF(("committing txn %" Yu " %p on mdbenv %p, root page %" Yu,
	    txn->mt_txnid, (void*)txn, (void*)env, txn->mt_dbs[MAIN_DBI].md_root));

	// Update DB root pointers
	if (txn->mt_numdbs > CORE_DBS)
	{
		MDB_cursor mc;
		MDB_dbi i;
		MDB_val data;
		data.mv_size = sizeof(MDB_db);

		mdb_cursor_init(&mc, txn, MAIN_DBI, NULL);
		for (i = CORE_DBS; i < txn->mt_numdbs; i++)
		{
			if (txn->mt_dbflags[i] & DB_DIRTY)
			{
				if (TXN_DBI_CHANGED(txn, i))
				{
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
	if (env->me_flags & MDB_PREVSNAPSHOT)
	{
		if (!(env->me_flags & MDB_NOLOCK))
		{
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

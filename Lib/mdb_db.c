#include "mdb_db.h"

#include "mdb_compare.h"
#include "mdb_page.h"
#include "mdb_env.h"
#include "mdb_cursor.h"
#include "mdb_debug.h"

/** Set the default comparison functions for a database.
 * Called immediately after a database is opened to set the defaults.
 * The user can then override them with #mdb_set_compare() or
 * #mdb_set_dupsort().
 * @param[in] txn A transaction handle returned by #mdb_txn_begin()
 * @param[in] dbi A database handle returned by #mdb_dbi_open()
 */
static void mdb_default_cmp(MDB_txn *txn, MDB_dbi dbi)
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
	} else if (rc == MDB_NOTFOUND) {
		rc = MDB_SUCCESS;
	}
	mc->mc_flags &= ~C_INITIALIZED;
	return rc;
}

static int mdb_del0(MDB_txn *txn, MDB_dbi dbi, MDB_val *key, MDB_val *data, unsigned flags)
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

int mdb_put(MDB_txn *txn, MDB_dbi dbi, MDB_val *key, MDB_val *data, unsigned int flags)
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

int mdb_get(MDB_txn *txn, MDB_dbi dbi, MDB_val *key, MDB_val *data)
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
	return rc;
}

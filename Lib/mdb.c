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
#include "mdb_cursor.h"

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

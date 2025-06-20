#include "mdb_cursor.h"

#include "mdb_compare.h"
#include "mdb_page.h"
#include "mdb_env.h"
#include "mdb_debug.h"
#include "mdb_db.h"
#include "mdb_txn.h"

#if MDB_DEBUG
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

	DPRINTF(("searching %u keys in %s %spage %" Yu,
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
				DPRINTF(("found branch index %u [%s -> %" Yu "], rc = %i",
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
		DPRINTF(("popping page %" Yu " off db %d cursor %p",
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
	DPRINTF(("pushing page %" Yu " on db %d cursor %p", mp->mp_pgno,
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
		DPRINTF(("read overflow page %" Yu " failed", pgno));
		return rc;
	}
	data->mv_data = METADATA(omp);
	MC_SET_OVPG(mc, omp);

	return MDB_SUCCESS;
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
	DPRINTF(("parent page is page %" Yu ", index %u",
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
		} else {
			mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
			if (op == MDB_NEXT_DUP)
				return MDB_NOTFOUND;
		}
	}

	DPRINTF(("cursor_next: top page is %" Yu " in cursor %p",
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
		DPRINTF(("next page is %" Yu ", key index %u", mp->mp_pgno, mc->mc_ki[mc->mc_top]));
	} else
		mc->mc_ki[mc->mc_top]++;

skip:
	DPRINTF(("==> cursor points to page %" Yu " with %u keys, key index %u",
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
		} else {
			mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
			if (op == MDB_PREV_DUP)
				return MDB_NOTFOUND;
		}
	}

	DPRINTF(("cursor_prev: top page is %" Yu " in cursor %p",
		mdb_dbg_pgno(mp), (void *) mc));

	mc->mc_flags &= ~(C_EOF|C_DEL);

	if (mc->mc_ki[mc->mc_top] == 0)  {
		DPUTS("=====> move to prev sibling page");
		if ((rc = mdb_cursor_sibling(mc, 0)) != MDB_SUCCESS) {
			return rc;
		}
		mp = mc->mc_pg[mc->mc_top];
		mc->mc_ki[mc->mc_top] = NUMKEYS(mp) - 1;
		DPRINTF(("prev page is %" Yu ", key index %u", mp->mp_pgno, mc->mc_ki[mc->mc_top]));
	} else
		mc->mc_ki[mc->mc_top]--;

	DPRINTF(("==> cursor points to page %" Yu " with %u keys, key index %u",
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

/* Internal error codes, not exposed outside liblmdb */
#define	MDB_NO_ROOT		(MDB_LAST_ERRCODE + 10)

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

	DPRINTF(("==> put db %d key [%s], size %" Z "u, data size %" Z "u",
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

	unsigned offset = 0;

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
			fp = (MDB_page*) env->me_pbuf;
			fp->mp_pad = data->mv_size; /* used if MDB_DUPFIXED */
			MP_LOWER(fp) = MP_UPPER(fp) = (PAGEHDRSZ-PAGEBASE);
			olddata.mv_size = PAGEHDRSZ;
			goto prep_subDB;
		}
	} else {
		/* there's only a key anyway, so this is a no-op */
		if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
			char *ptr;
			{
				unsigned int ksize = mc->mc_db->md_pad;
				if (key->mv_size != ksize)
					return MDB_BAD_VALSIZE;
				ptr = LEAF2KEY(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top], ksize);
				memcpy(ptr, key->mv_data, ksize);
			}
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
			unsigned	i;
			offset = 0;
			mp = fp = (MDB_page*)(xdata.mv_data = env->me_pbuf);
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
				fp = (MDB_page*) olddata.mv_data;
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
			xdata.mv_data = (void*) "";
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
	MDB_TRACE(("%p, %" Z "u[%s], %" Z "u%s, %u",
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
				mc->mc_xcursor->mx_cursor.mc_pg[0] = (MDB_page*)(NODEDATA(leaf));
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
					mc->mc_xcursor->mx_cursor.mc_pg[0] = (MDB_page*)(NODEDATA(leaf));
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
		MDB_page *fp = (MDB_page*) NODEDATA(node);
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
	DPRINTF(("Sub-db -%u root page %" Yu, mx->mx_cursor.mc_dbi,
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
	DPRINTF(("Sub-db -%u root page %" Yu, mx->mx_cursor.mc_dbi,
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

	if ((mc = (MDB_cursor*) malloc(size)) != NULL) {
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
								m3->mc_xcursor->mx_cursor.mc_pg[0] = (MDB_page*)(NODEDATA(node));
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

/** Replace the key for a branch node with a new key.
 * Set #MDB_TXN_ERROR on failure.
 * @param[in] mc Cursor pointing to the node to operate on.
 * @param[in] key The new key to use.
 * @return 0 on success, non-zero on failure.
 */
int mdb_update_key(MDB_cursor *mc, MDB_val *key)
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
		DPRINTF(("update key %u (ofs %u) [%s] to [%s] on page %" Yu,
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

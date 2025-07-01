#include "cursor.h"

#include "btree.h"
#include "compare.h"
#include "db.h"
#include "debug.h"
#include "env.h"
#include "txn.h"

#include <utility>

#if FDS_DEBUG
void fds_cursor_chk(FDS_cursor* mc)
{
    unsigned int i;
    FDS_node* node;
    FDS_page* mp;

    if (mc->mc_snum == 0 || (mc->mc_flags & C_INITIALIZED) == 0)
        return;
    for (i = 0; i < mc->mc_top; i++)
    {
        mp = mc->mc_pg[i];
        node = NODEPTR(mp, mc->mc_ki[i]);
        if (NODEPGNO(node) != mc->mc_pg[i + 1]->mp_pgno)
            printf("oops!\n");
    }
    if (mc->mc_ki[i] >= NUMKEYS(mc->mc_pg[i]))
        printf("ack!\n");
    // No duplicate support - xcursor checks removed
}
#endif

// Search for key within a page, using binary search.
// Returns the smallest entry larger or equal to the key.
// If exactp is non-null, stores whether the found entry was an exact match
// in *exactp (1 or 0).
// Updates the cursor index with the index of the found entry.
// If no entry larger or equal to the key is found, returns NULL.
auto fds_node_search(FDS_cursor* mc, FDS_val* key, int* exactp) -> FDS_node*
{
    unsigned int i = 0;
    FDS_page* mp = mc->mc_pg[mc->mc_top];
    FDS_node* node = nullptr;
    DKBUF;

    const unsigned int nkeys = NUMKEYS(mp);

    DPRINTF(("searching %u keys in %s %spage %" Yu,
             nkeys,
             IS_LEAF(mp) ? "leaf" : "branch",
             IS_SUBP(mp) ? "sub-" : "",
             fds_dbg_pgno(mp)));

    int low = IS_LEAF(mp) ? 0 : 1;
    int high = nkeys - 1;

    const auto cmp = mc->mc_dbx->md_cmp;

    int compareResult = 0;
    // No LEAF2 support - use standard node search
    while (low <= high)
    {
        i = (low + high) >> 1;

        node = NODEPTR(mp, i);

        FDS_val nodekey;
        nodekey.mv_size = NODEKSZ(node);
        nodekey.mv_data = NODEKEY(node);

        compareResult = cmp(key, &nodekey);
#if FDS_DEBUG
        if (IS_LEAF(mp))
            DPRINTF(("found leaf index %u [%s], rc = %i", i, DKEY(&nodekey), compareResult));
        else
            DPRINTF(("found branch index %u [%s -> %" Yu "], rc = %i", i, DKEY(&nodekey), NODEPGNO(node), compareResult));
#endif
        if (compareResult == 0)
            break;
        if (compareResult > 0)
            low = i + 1;
        else
            high = i - 1;
    }

    if (compareResult > 0)
    {
        // Found entry is less than the key.
        // Skip to get the smallest entry larger than key.
        i++;
        node = NODEPTR(mp, i);
    }

    if (exactp != nullptr)
        *exactp = static_cast<int>(compareResult == 0 && nkeys > 0);

    // store the key index
    mc->mc_ki[mc->mc_top] = i;
    if (i >= nkeys)
        return nullptr; // There is no entry larger or equal to the key.

    return node;
}

// Pop a page off the top of the cursor's stack.
void fds_cursor_pop(FDS_cursor* mc)
{
    if (mc->mc_snum != 0U)
    {
        DPRINTF(("popping page %" Yu " off db %d cursor %p", mc->mc_pg[mc->mc_top]->mp_pgno, DDBI(mc), (void*)mc));

        mc->mc_snum--;
        if (mc->mc_snum != 0U)
        {
            mc->mc_top--;
        }
        else
        {
            mc->mc_flags &= ~C_INITIALIZED;
        }
    }
}

// Push a page onto the top of the cursor's stack.
// Set #FDS_TXN_ERROR on failure.
auto fds_cursor_push(FDS_cursor* mc, FDS_page* mp) -> int
{
    DPRINTF(("pushing page %" Yu " on db %d cursor %p", mp->mp_pgno, DDBI(mc), (void*)mc));

    if (mc->mc_snum >= CURSOR_STACK)
    {
        mc->mc_txn->mt_flags |= FDS_TXN_ERROR;
        return FDS_CURSOR_FULL;
    }

    mc->mc_top = mc->mc_snum++;
    mc->mc_pg[mc->mc_top] = mp;
    mc->mc_ki[mc->mc_top] = 0;

    return FDS_SUCCESS;
}

// Return the data associated with a given node.
//
// mc The cursor for this operation.
// leaf The node being read.
// data Updated to point to the node's data.
// 0 on success, non-zero on failure.
auto fds_node_read(FDS_cursor* mc, FDS_node* leaf, FDS_val* data) -> int
{
    FDS_page* omp;  // overflow page
    pgno_t pgno;
    int rc;

    if (MC_OVPG(mc))
    {
        MC_SET_OVPG(mc, NULL);
    }
    if (!F_ISSET(leaf->mn_flags, F_BIGDATA))
    {
        data->mv_size = NODEDSZ(leaf);
        data->mv_data = NODEDATA(leaf);
        return FDS_SUCCESS;
    }

    // Read overflow data.
    data->mv_size = NODEDSZ(leaf);
    memcpy(&pgno, NODEDATA(leaf), sizeof(pgno));
    rc = fds_page_get(mc, pgno, &omp, nullptr);
    if (rc != 0)
    {
        DPRINTF(("read overflow page %" Yu " failed", pgno));
        return rc;
    }
    data->mv_data = METADATA(omp);
    MC_SET_OVPG(mc, omp);

    return FDS_SUCCESS;
}

// Find a sibling for a page.
// Replaces the page at the top of the cursor's stack with the
// specified sibling, if one exists.
//
// mc The cursor for this operation.
// move_right Non-zero if the right sibling is requested,
// otherwise the left sibling.
// 0 on success, non-zero on failure.
auto fds_cursor_sibling(FDS_cursor* mc, int move_right) -> int
{
    int rc;
    FDS_node* indx;
    FDS_page* mp;
    if (mc->mc_snum < 2)
    {
        return FDS_NOTFOUND;  // root has no siblings
    }

    fds_cursor_pop(mc);
    DPRINTF(("parent page is page %" Yu ", index %u", mc->mc_pg[mc->mc_top]->mp_pgno, mc->mc_ki[mc->mc_top]));

    if ((move_right != 0) ? (mc->mc_ki[mc->mc_top] + 1U >= NUMKEYS(mc->mc_pg[mc->mc_top]))
                          : (mc->mc_ki[mc->mc_top] == 0))
    {
        DPRINTF(("no more keys left, moving to %s sibling", move_right ? "right" : "left"));
        rc = fds_cursor_sibling(mc, move_right);
        if (rc != FDS_SUCCESS)
        {
            // undo cursor_pop before returning
            mc->mc_top++;
            mc->mc_snum++;
            return rc;
        }
    }
    else
    {
        if (move_right != 0)
            mc->mc_ki[mc->mc_top]++;
        else
            mc->mc_ki[mc->mc_top]--;
        DPRINTF(("just moving to %s index key %u", move_right ? "right" : "left", mc->mc_ki[mc->mc_top]));
    }
    fds_cassert(mc, IS_BRANCH(mc->mc_pg[mc->mc_top]));

    indx = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
    rc = fds_page_get(mc, NODEPGNO(indx), &mp, nullptr);
    if (rc != 0)
    {
        // mc will be inconsistent if caller does mc_snum++ as above
        mc->mc_flags &= ~(C_INITIALIZED | C_EOF);
        return rc;
    }

    fds_cursor_push(mc, mp);
    if (move_right == 0)
        mc->mc_ki[mc->mc_top] = NUMKEYS(mp) - 1;

    return FDS_SUCCESS;
}

// Move the cursor to the next data item.
auto fds_cursor_next(FDS_cursor* mc, FDS_val* key, FDS_val* data, FDS_cursor_op op) -> int
{
    FDS_page* mp;
    FDS_node* leaf;
    int rc;

    // No duplicate support - reject duplicate operations
    if (op != FDS_NEXT)
        return FDS_NOTFOUND;

    if ((mc->mc_flags & C_INITIALIZED) == 0U)
        return fds_cursor_first(mc, key, data);

    mp = mc->mc_pg[mc->mc_top];

    if ((mc->mc_flags & C_EOF) != 0U)
    {
        if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mp) - 1)
            return FDS_NOTFOUND;
        mc->mc_flags ^= C_EOF;
    }

    DPRINTF(("cursor_next: top page is %" Yu " in cursor %p", fds_dbg_pgno(mp), (void*)mc));
    if ((mc->mc_flags & C_DEL) != 0U)
    {
        mc->mc_flags ^= C_DEL;
        goto skip;
    }

    if (mc->mc_ki[mc->mc_top] + 1U >= NUMKEYS(mp))
    {
        DPUTS("=====> move to next sibling page");
        rc = fds_cursor_sibling(mc, 1);
        if (rc != FDS_SUCCESS)
        {
            mc->mc_flags |= C_EOF;
            return rc;
        }
        mp = mc->mc_pg[mc->mc_top];

        DPRINTF(("next page is %" Yu ", key index %u", mp->mp_pgno, mc->mc_ki[mc->mc_top]));
    }
    else
        mc->mc_ki[mc->mc_top]++;

skip:
    DPRINTF(("==> cursor points to page %" Yu " with %u keys, key index %u",
             fds_dbg_pgno(mp),
             NUMKEYS(mp),
             mc->mc_ki[mc->mc_top]));

    // No LEAF2 support - use standard nodes
    // Handle case where sibling navigation might land on a branch page
    if (!IS_LEAF(mp))
    {
        // If we're on a branch page, we need to navigate down to a leaf
        // This can happen after sibling navigation in complex tree structures
        mc->mc_flags |= C_EOF;
        return FDS_NOTFOUND;
    }
    leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

    // No duplicate support - read data directly
    if (data != nullptr)
    {
        rc = fds_node_read(mc, leaf, data);
        if (rc != FDS_SUCCESS)
            return rc;
    }

    FDS_GET_KEY(leaf, key);
    return FDS_SUCCESS;
}

// Move the cursor to the previous data item.
auto fds_cursor_prev(FDS_cursor* mc, FDS_val* key, FDS_val* data, FDS_cursor_op op) -> int
{
    FDS_page* mp;
    FDS_node* leaf;
    int rc;

    // No duplicate support - reject duplicate operations
    if (op != FDS_PREV)
        return FDS_NOTFOUND;

    if ((mc->mc_flags & C_INITIALIZED) == 0U)
    {
        rc = fds_cursor_last(mc, key, data);
        if (rc != 0)
            return rc;
        mc->mc_ki[mc->mc_top]++;
    }

    mp = mc->mc_pg[mc->mc_top];

    DPRINTF(("cursor_prev: top page is %" Yu " in cursor %p", fds_dbg_pgno(mp), (void*)mc));

    mc->mc_flags &= ~(C_EOF | C_DEL);

    if (mc->mc_ki[mc->mc_top] == 0)
    {
        DPUTS("=====> move to prev sibling page");
        rc = fds_cursor_sibling(mc, 0);
        if (rc != FDS_SUCCESS)
        {
            return rc;
        }
        mp = mc->mc_pg[mc->mc_top];
        mc->mc_ki[mc->mc_top] = NUMKEYS(mp) - 1;

        DPRINTF(("prev page is %" Yu ", key index %u", mp->mp_pgno, mc->mc_ki[mc->mc_top]));
    }
    else
        mc->mc_ki[mc->mc_top]--;

    DPRINTF(("==> cursor points to page %" Yu " with %u keys, key index %u",
             fds_dbg_pgno(mp),
             NUMKEYS(mp),
             mc->mc_ki[mc->mc_top]));

    if (!IS_LEAF(mp))
        return FDS_CORRUPTED;

    // No LEAF2 support - use standard nodes
    leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

    // No duplicate support - read data directly
    if (data != nullptr)
    {
        rc = fds_node_read(mc, leaf, data);
        if (rc != FDS_SUCCESS)
            return rc;
    }

    FDS_GET_KEY(leaf, key);
    return FDS_SUCCESS;
}

// Set the cursor on a specific data item.
auto fds_cursor_set(FDS_cursor* mc, FDS_val* key, FDS_val* data, FDS_cursor_op op, int* exactp) -> int
{
    int rc;
    int exact = 0;
    FDS_page* mp;
    FDS_node* leaf = nullptr;
    DKBUF;

    if (key->mv_size == 0)
        return FDS_BAD_VALSIZE;

    // No xcursor support - removed

    // See if we're already on the right page
    if ((mc->mc_flags & C_INITIALIZED) != 0U)
    {
        FDS_val nodekey;

        mp = mc->mc_pg[mc->mc_top];
        if (!NUMKEYS(mp))
        {
            mc->mc_ki[mc->mc_top] = 0;
            return FDS_NOTFOUND;
        }
        // No LEAF2 support - use standard nodes
        leaf = NODEPTR(mp, 0);
        FDS_GET_KEY2(leaf, nodekey);

        rc = mc->mc_dbx->md_cmp(key, &nodekey);
        if (rc == 0)
        {
            // Probably happens rarely, but first node on the page
            // was the one we wanted.
            mc->mc_ki[mc->mc_top] = 0;
            if (exactp != nullptr)
                *exactp = 1;
            goto set1;
        }
        if (rc > 0)
        {
            unsigned int i;
            unsigned int nkeys = NUMKEYS(mp);
            if (nkeys > 1)
            {
                // No LEAF2 support - use standard nodes
                leaf = NODEPTR(mp, nkeys - 1);
                FDS_GET_KEY2(leaf, nodekey);

                rc = mc->mc_dbx->md_cmp(key, &nodekey);
                if (rc == 0)
                {
                    // last node was the one we wanted
                    mc->mc_ki[mc->mc_top] = nkeys - 1;
                    if (exactp != nullptr)
                        *exactp = 1;
                    goto set1;
                }
                if (rc < 0)
                {
                    if (mc->mc_ki[mc->mc_top] < NUMKEYS(mp))
                    {
                        // This is definitely the right page, skip search_page
                        leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
                        FDS_GET_KEY2(leaf, nodekey);

                        rc = mc->mc_dbx->md_cmp(key, &nodekey);
                        if (rc == 0)
                        {
                            // current node was the one we wanted
                            if (exactp != nullptr)
                                *exactp = 1;
                            goto set1;
                        }
                    }
                    rc = 0;
                    mc->mc_flags &= ~C_EOF;
                    goto set2;
                }
            }
            // If any parents have right-sibs, search.
            // Otherwise, there's nothing further.
            for (i = 0; i < mc->mc_top; i++)
                if (mc->mc_ki[i] < NUMKEYS(mc->mc_pg[i]) - 1)
                    break;
            if (i == mc->mc_top)
            {
                // There are no other pages
                mc->mc_ki[mc->mc_top] = nkeys;
                return FDS_NOTFOUND;
            }
        }
        if (mc->mc_top == 0U)
        {
            // There are no other pages
            mc->mc_ki[mc->mc_top] = 0;
            if (op == FDS_SET_RANGE && (exactp == nullptr))
            {
                rc = 0;
                goto set1;
            }
            else
                return FDS_NOTFOUND;
        }
    }
    else
    {
        mc->mc_pg[0] = nullptr;
    }

    rc = fds_page_search(mc, key, 0);
    if (rc != FDS_SUCCESS)
        return rc;

    mp = mc->mc_pg[mc->mc_top];
    fds_cassert(mc, IS_LEAF(mp));

set2:
    leaf = fds_node_search(mc, key, exactp);
    if (exactp != nullptr && (*exactp == 0))
    {
        // FDS_SET specified and not an exact match.
        return FDS_NOTFOUND;
    }

    if (leaf == nullptr)
    {
        DPUTS("===> inexact leaf not found, goto sibling");
        rc = fds_cursor_sibling(mc, 1);
        if (rc != FDS_SUCCESS)
        {
            mc->mc_flags |= C_EOF;
            return rc;  // no entries matched
        }
        mp = mc->mc_pg[mc->mc_top];
        fds_cassert(mc, IS_LEAF(mp));
        leaf = NODEPTR(mp, 0);
    }

set1:
    mc->mc_flags |= C_INITIALIZED;
    mc->mc_flags &= ~C_EOF;

    // No LEAF2 support - use standard nodes
    // No duplicate support - read data directly
    if (data != nullptr)
    {
        // No duplicate operations supported - all handled in switch statement

        rc = fds_node_read(mc, leaf, data);
        if (rc != FDS_SUCCESS)
            return rc;
    }

    // The key already matches in all other cases
    if (op == FDS_SET_RANGE || op == FDS_SET_KEY)
        FDS_GET_KEY(leaf, key);
    DPRINTF(("==> cursor placed on key [%s]", DKEY(key)));

    return FDS_SUCCESS;
}

// Move the cursor to the first item in the database.
auto fds_cursor_first(FDS_cursor* mc, FDS_val* key, FDS_val* data) -> int
{
    int rc;
    FDS_node* leaf;

    // No xcursor support - removed

    if (((mc->mc_flags & C_INITIALIZED) == 0U) || (mc->mc_top != 0U))
    {
        rc = fds_page_search(mc, nullptr, FDS_PS_FIRST);
        if (rc != FDS_SUCCESS)
            return rc;
    }
    fds_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));

    leaf = NODEPTR(mc->mc_pg[mc->mc_top], 0);
    mc->mc_flags |= C_INITIALIZED;
    mc->mc_flags &= ~C_EOF;

    mc->mc_ki[mc->mc_top] = 0;

    // No LEAF2 support - use standard nodes
    // No duplicate support - read data directly
    if (data != nullptr)
    {
        rc = fds_node_read(mc, leaf, data);
        if (rc != FDS_SUCCESS)
            return rc;
    }

    FDS_GET_KEY(leaf, key);
    return FDS_SUCCESS;
}

// Move the cursor to the last item in the database.
auto fds_cursor_last(FDS_cursor* mc, FDS_val* key, FDS_val* data) -> int
{
    int rc;
    FDS_node* leaf;

    // No xcursor support - removed

    if (((mc->mc_flags & C_INITIALIZED) == 0U) || (mc->mc_top != 0U))
    {
        rc = fds_page_search(mc, nullptr, FDS_PS_LAST);
        if (rc != FDS_SUCCESS)
            return rc;
    }
    fds_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));

    mc->mc_ki[mc->mc_top] = NUMKEYS(mc->mc_pg[mc->mc_top]) - 1;
    mc->mc_flags |= C_INITIALIZED | C_EOF;
    leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);

    // No LEAF2 support - use standard nodes
    // No duplicate support - read data directly
    if (data != nullptr)
    {
        rc = fds_node_read(mc, leaf, data);
        if (rc != FDS_SUCCESS)
            return rc;
    }

    FDS_GET_KEY(leaf, key);
    return FDS_SUCCESS;
}

auto fds_cursor_get(FDS_cursor* cursor, FDS_val* key, FDS_val* data, FDS_cursor_op op) -> int
{
    int rc;
    int exact = 0;

    if (cursor == nullptr)
        return EINVAL;

    if ((cursor->mc_txn->mt_flags & FDS_TXN_BLOCKED) != 0U)
        return FDS_BAD_TXN;

    switch (op)
    {
    case FDS_GET_CURRENT:
        if ((cursor->mc_flags & C_INITIALIZED) == 0U)
        {
            rc = EINVAL;
        }
        else
        {
            FDS_page* mp = cursor->mc_pg[cursor->mc_top];
            int nkeys = NUMKEYS(mp);
            if ((nkeys == 0) || (cursor->mc_ki[cursor->mc_top] >= nkeys))
            {
                cursor->mc_ki[cursor->mc_top] = nkeys;
                rc = FDS_NOTFOUND;
                break;
            }
            rc = FDS_SUCCESS;
            // No LEAF2 support - use standard nodes
            FDS_node* leaf = NODEPTR(mp, cursor->mc_ki[cursor->mc_top]);
            FDS_GET_KEY(leaf, key);
            if (data != nullptr)
            {
                // No duplicate support - read data directly
                rc = fds_node_read(cursor, leaf, data);
            }
        }
        break;
    case FDS_SET:
    case FDS_SET_KEY:
    case FDS_SET_RANGE:
        if (key == nullptr)
        {
            rc = EINVAL;
        }
        else
        {
            rc = fds_cursor_set(cursor, key, data, op, op == FDS_SET_RANGE ? nullptr : &exact);
        }
        break;
    case FDS_NEXT:
        rc = fds_cursor_next(cursor, key, data, op);
        break;
    case FDS_PREV:
        rc = fds_cursor_prev(cursor, key, data, op);
        break;
    case FDS_FIRST:
        rc = fds_cursor_first(cursor, key, data);
        break;
    case FDS_LAST:
        rc = fds_cursor_last(cursor, key, data);
        break;
    default:
        // All duplicate operations are not supported
        DPRINTF(("unhandled/unimplemented cursor operation %u", op));
        rc = FDS_INCOMPATIBLE;
        break;
    }

    if ((cursor->mc_flags & C_DEL) != 0U)
        cursor->mc_flags ^= C_DEL;

    return rc;
}

// Touch all the pages in the cursor stack. Set mc_top.
// Makes sure all the pages are writable, before attempting a write operation.
//
// mc The cursor to operate on.
auto fds_cursor_touch(FDS_cursor* mc) -> int
{
    int rc = FDS_SUCCESS;

    if (mc->mc_dbi >= CORE_DBS && ((*mc->mc_dbflag & DB_DIRTY) == 0))
    {
        // Touch DB record of named DB
        FDS_cursor mc2;
        if (TXN_DBI_CHANGED(mc->mc_txn, mc->mc_dbi))
            return FDS_BAD_DBI;
        fds_cursor_init(&mc2, mc->mc_txn, MAIN_DBI);
        rc = fds_page_search(&mc2, &mc->mc_dbx->md_name, FDS_PS_MODIFY);
        if (rc != 0)
            return rc;
        *mc->mc_dbflag |= DB_DIRTY;
    }
    mc->mc_top = 0;
    if (mc->mc_snum != 0U)
    {
        do
        {
            rc = fds_page_touch(mc);
        } while ((rc == 0) && ++(mc->mc_top) < mc->mc_snum);
        mc->mc_top = mc->mc_snum - 1;
    }
    return rc;
}

// Do not spill pages to disk if txn is getting full, may fail instead
enum
{
    FDS_NOSPILL = 0x8000
};

// Internal error codes, not exposed outside FiksDataStore
#define FDS_NO_ROOT (FDS_LAST_ERRCODE + 10)

auto fds_cursor_put_impl(FDS_cursor* mc, FDS_val* key, FDS_val* data, unsigned int flags) -> int
{
    FDS_env* env;
    FDS_node* leaf = nullptr;
    FDS_page* fp;
    FDS_page* mp;
    FDS_page* sub_root = nullptr;
    uint16_t fp_flags;
    FDS_val xdata;
    FDS_val* rdata;
    FDS_val dkey;
    FDS_val olddata;
    FDS_db dummy;
    int do_sub = 0;
    int insert_key;
    int insert_data;
    unsigned int mcount = 0;
    unsigned int dcount = 0;
    unsigned int nospill;
    size_t nsize;
    int rc;
    int rc2;
    unsigned int nflags;
    DKBUF;

    if (mc == nullptr || key == nullptr)
        return EINVAL;

    env = mc->mc_txn->mt_env;

    // No FDS_MULTIPLE support - removed
    nospill = flags & FDS_NOSPILL;
    flags &= ~FDS_NOSPILL;

    if ((mc->mc_txn->mt_flags & (FDS_TXN_RDONLY | FDS_TXN_BLOCKED)) != 0U)
        return ((mc->mc_txn->mt_flags & FDS_TXN_RDONLY) != 0U) ? EACCES : FDS_BAD_TXN;

    if (key->mv_size - 1 >= ENV_MAXKEY(env))
        return FDS_BAD_VALSIZE;

    // No duplicate support - simplified size check
    if (data->mv_size > MAXDATASIZE)
        return FDS_BAD_VALSIZE;

    DPRINTF(("==> put db %d key [%s], size %" Z "u, data size %" Z "u",
             DDBI(mc),
             DKEY(key),
             key ? key->mv_size : 0,
             data->mv_size));

    dkey.mv_size = 0;

    if ((flags & FDS_CURRENT) != 0U)
    {
        if ((mc->mc_flags & C_INITIALIZED) == 0U)
            return EINVAL;
        rc = FDS_SUCCESS;
    }
    else if (mc->mc_db->md_root == P_INVALID)
    {
        // new database, cursor has nothing to point to
        mc->mc_snum = 0;
        mc->mc_top = 0;
        mc->mc_flags &= ~C_INITIALIZED;
        rc = FDS_NO_ROOT;
    }
    else
    {
        int exact = 0;
        FDS_val d2;
        if ((flags & FDS_APPEND) != 0U)
        {
            FDS_val k2;
            rc = fds_cursor_last(mc, &k2, &d2);
            if (rc == 0)
            {
                rc = mc->mc_dbx->md_cmp(key, &k2);
                if (rc > 0)
                {
                    rc = FDS_NOTFOUND;
                    mc->mc_ki[mc->mc_top]++;
                }
                else
                {
                    // new key is <= last key
                    rc = FDS_KEYEXIST;
                }
            }
        }
        else
        {
            rc = fds_cursor_set(mc, key, &d2, FDS_SET, &exact);
        }
        if (((flags & FDS_NOOVERWRITE) != 0U) && rc == 0)
        {
            DPRINTF(("duplicate key [%s]", DKEY(key)));
            *data = d2;
            return FDS_KEYEXIST;
        }
        if ((rc != 0) && rc != FDS_NOTFOUND)
            return rc;
    }

    if ((mc->mc_flags & C_DEL) != 0U)
        mc->mc_flags ^= C_DEL;

    // Cursor is positioned, check for room in the dirty list
    if (nospill == 0U)
    {
        // No FDS_MULTIPLE support - simplified
        rdata = data;
        rc2 = fds_page_spill(mc, key, rdata);
        if (rc2 != 0)
            return rc2;
    }

    if (rc == FDS_NO_ROOT)
    {
        FDS_page* np;
        // new database, write a root leaf page
        DPUTS("allocating new root leaf page");
        rc2 = fds_page_new(mc, P_LEAF, 1, &np);
        if (rc2 != 0)
        {
            return rc2;
        }
        fds_cursor_push(mc, np);
        mc->mc_db->md_root = np->mp_pgno;
        mc->mc_db->md_depth++;
        *mc->mc_dbflag |= DB_DIRTY;
        // No duplicate support - always use standard leaf pages
        mc->mc_flags |= C_INITIALIZED;
    }
    else
    {
        // make sure all cursor pages are writable
        rc2 = fds_cursor_touch(mc);
        if (rc2 != 0)
            return rc2;
    }

    unsigned offset = 0;

    insert_key = insert_data = rc;
    if (insert_key != 0)
    {
        // The key does not exist
        DPRINTF(("inserting key at index %i", mc->mc_ki[mc->mc_top]));
        // No duplicate support - simplified logic
    }
    else
    {
        // there's only a key anyway, so this is a no-op
        // No LEAF2 support - use standard nodes
        leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);

        // if overwriting slot 0 of leaf, need to
        // update branch key if there is a parent page
        if ((mc->mc_top != 0U) && (mc->mc_ki[mc->mc_top] == 0U))
        {
            unsigned short dtop = 1;
            mc->mc_top--;
            // slot 0 is always an empty key, find real slot
            while ((mc->mc_top != 0U) && (mc->mc_ki[mc->mc_top] == 0U))
            {
                mc->mc_top--;
                dtop++;
            }
            if (mc->mc_ki[mc->mc_top] != 0U)
                rc2 = fds_update_key(mc, key);
            else
                rc2 = FDS_SUCCESS;
            mc->mc_top += dtop;
            if (rc2 != 0)
                return rc2;
        }

        leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
        olddata.mv_size = NODEDSZ(leaf);
        olddata.mv_data = NODEDATA(leaf);

        // No duplicate support - simplified logic
    current:
        // FiksDataStore passes F_SUBDATA in 'flags' to write a DB record
        if (((leaf->mn_flags ^ flags) & F_SUBDATA) != 0U)
            return FDS_INCOMPATIBLE;
        // overflow page overwrites need special handling
        if (F_ISSET(leaf->mn_flags, F_BIGDATA))
        {
            FDS_page* omp;
            pgno_t pg;
            int level;
            int ovpages;
            int dpages = OVPAGES(data->mv_size, env->me_psize);

            memcpy(&pg, olddata.mv_data, sizeof(pg));
            rc2 = fds_page_get(mc, pg, &omp, &level);
            if (rc2 != 0)
                return rc2;
            ovpages = omp->mp_pages;

            // Is the ov page large enough?
            if (ovpages >= dpages)
            {
                if (((omp->mp_flags & P_DIRTY) == 0) && ((level != 0) || ((env->me_flags & FDS_WRITEMAP) != 0U)))
                {
                    rc = fds_page_unspill(mc->mc_txn, omp, &omp);
                    if (rc != 0)
                        return rc;
                    level = 0;  // dirty in this txn or clean
                }
                // Is it dirty?
                if ((omp->mp_flags & P_DIRTY) != 0)
                {
                    // yes, overwrite it. Note in this case we don't
                    // bother to try shrinking the page if the new data
                    // is smaller than the overflow threshold.
                    if (level > 1)
                    {
                        size_t sz = (size_t)env->me_psize * ovpages;
                        size_t off;
                        FDS_page* np = fds_page_malloc(mc->mc_txn, ovpages);
                        FDS_ID2 id2;
                        if (np == nullptr)
                            return ENOMEM;
                        id2.mid = pg;
                        id2.mptr = np;
                        // Note - this page is already counted in parent's dirty_room
                        rc2 = fds_mid2l_insert(mc->mc_txn->mt_u.dirty_list, &id2);
                        fds_cassert(mc, rc2 == 0);
                        // Currently we make the page look as with put() in the
                        // parent txn, in case the user peeks at FDS_RESERVEd
                        // or unused parts. Some users treats ovpages specially.
                        if ((flags & FDS_RESERVE) == 0U)
                        {
                            // Skip the part where FiksDataStore will put *data.
                            // Copy end of page, adjusting alignment so
                            // compiler may copy words instead of bytes.
                            off = (PAGEHDRSZ + data->mv_size) & -(int)sizeof(size_t);
                            memcpy((size_t*)((char*)np + off), (size_t*)((char*)omp + off), sz - off);
                            sz = PAGEHDRSZ;
                        }
                        memcpy(np, omp, sz);  // Copy beginning of page
                        omp = np;
                    }
                    SETDSZ(leaf, data->mv_size);
                    if (F_ISSET(flags, FDS_RESERVE))
                        data->mv_data = METADATA(omp);
                    else
                        memcpy(METADATA(omp), data->mv_data, data->mv_size);
                    return FDS_SUCCESS;
                }
            }
            rc2 = fds_ovpage_free(mc, omp);
            if (rc2 != FDS_SUCCESS)
                return rc2;
        }
        else if (data->mv_size == olddata.mv_size)
        {
            // same size, just replace it. Note that we could
            // also reuse this node if the new data is smaller,
            // but instead we opt to shrink the node in that case.
            if (F_ISSET(flags, FDS_RESERVE))
                data->mv_data = olddata.mv_data;
            else
                memcpy(olddata.mv_data, data->mv_data, data->mv_size);
            return FDS_SUCCESS;
        }
        fds_node_del(mc, 0);
    }

    rdata = data;

    nflags = flags & (static_cast<unsigned>(F_SUBDATA) | static_cast<unsigned>(FDS_RESERVE) | static_cast<unsigned>(FDS_APPEND));
    nsize = fds_leaf_size(env, key, rdata);
    if (SIZELEFT(mc->mc_pg[mc->mc_top]) < nsize)
    {
        rc = fds_page_split(mc, key, rdata, P_INVALID, nflags);
    }
    else
    {
        // There is room already in this leaf page.
        rc = fds_node_add(mc, mc->mc_ki[mc->mc_top], key, rdata, 0, nflags);
        if (rc == 0)
        {
            // Adjust other cursors pointing to mp
            FDS_cursor* m2;
            FDS_cursor* m3;
            FDS_dbi dbi = mc->mc_dbi;
            unsigned i = mc->mc_top;
            FDS_page* mp = mc->mc_pg[i];

            for (m2 = mc->mc_txn->mt_cursors[dbi]; m2 != nullptr; m2 = m2->mc_next)
            {
                // No xcursor support - simplified
                m3 = m2;
                if (m3 == mc || m3->mc_snum < mc->mc_snum || m3->mc_pg[i] != mp)
                    continue;
                if (m3->mc_ki[i] >= mc->mc_ki[i] && (insert_key != 0))
                {
                    m3->mc_ki[i]++;
                }
                // No XCURSOR_REFRESH needed
            }
        }
    }

    if (rc == FDS_SUCCESS)
    {
        // Increment count unless we just replaced an existing item.
        if (insert_data != 0)
            mc->mc_db->md_entries++;
        if (insert_key != 0)
        {
            // If we succeeded and the key didn't exist before,
            // make sure the cursor is marked valid.
            mc->mc_flags |= C_INITIALIZED;
        }
        // No FDS_MULTIPLE support
        return rc;
    }
    mc->mc_txn->mt_flags |= FDS_TXN_ERROR;
    return rc;
}

auto fds_cursor_put(FDS_cursor* cursor, FDS_val* key, FDS_val* data, unsigned int flags) -> int
{
    DKBUF;
    DDBUF;
    int rc = fds_cursor_put_impl(cursor, key, data, flags);
    DPRINTF(("%p, %" Z "u[%s], %" Z "u%s, %u",
               cursor,
               key ? key->mv_size : 0,
               DKEY(key),
               data ? data->mv_size : 0,
               data ? fds_dval(cursor->mc_txn, cursor->mc_dbi, data, dbuf) : "",
               flags));
    return rc;
}

auto fds_cursor_del_impl(FDS_cursor* mc, unsigned int flags) -> int
{
    FDS_node* leaf;
    FDS_page* mp;
    int rc;

    if ((mc->mc_txn->mt_flags & (FDS_TXN_RDONLY | FDS_TXN_BLOCKED)) != 0U)
        return ((mc->mc_txn->mt_flags & FDS_TXN_RDONLY) != 0U) ? EACCES : FDS_BAD_TXN;

    if ((mc->mc_flags & C_INITIALIZED) == 0U)
        return EINVAL;

    if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mc->mc_pg[mc->mc_top]))
        return FDS_NOTFOUND;

    if ((flags & FDS_NOSPILL) == 0U)
    {
        rc = fds_page_spill(mc, nullptr, nullptr);
        if (rc != 0)
            return rc;
    }

    rc = fds_cursor_touch(mc);
    if (rc != 0)
        return rc;

    mp = mc->mc_pg[mc->mc_top];
    if (!IS_LEAF(mp))
        return FDS_CORRUPTED;

    // No LEAF2 support - use standard nodes
    leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

    // No duplicate support - simplified delete logic
    // FiksDataStore passes F_SUBDATA in 'flags' to delete a DB record
    if (((leaf->mn_flags ^ flags) & F_SUBDATA) != 0U)
    {
        rc = FDS_INCOMPATIBLE;
        goto fail;
    }

    // add overflow pages to free list
    if (F_ISSET(leaf->mn_flags, F_BIGDATA))
    {
        FDS_page* omp;
        pgno_t pg;

        memcpy(&pg, NODEDATA(leaf), sizeof(pg));
        rc = fds_page_get(mc, pg, &omp, nullptr);
        if (rc == 0)
        {
            rc = fds_ovpage_free(mc, omp);
        }
        if (rc != 0)
            goto fail;
    }

del_key:
    return fds_cursor_del0(mc);

fail:
    if (rc != 0)
        mc->mc_txn->mt_flags |= FDS_TXN_ERROR;
    return rc;
}

auto fds_cursor_del(FDS_cursor* cursor, unsigned int flags) -> int
{
    DPRINTF(("%p, %u", cursor, flags));
    return fds_cursor_del_impl(cursor, flags);
}

// Initialize a cursor for a given transaction and database.
void fds_cursor_init(FDS_cursor* mc, FDS_txn* txn, FDS_dbi dbi)
{
    mc->mc_next = nullptr;
    mc->mc_backup = nullptr;
    mc->mc_dbi = dbi;
    mc->mc_txn = txn;
    mc->mc_db = &txn->mt_dbs[dbi];
    mc->mc_dbx = &txn->mt_dbxs[dbi];
    mc->mc_dbflag = &txn->mt_dbflags[dbi];
    mc->mc_snum = 0;
    mc->mc_top = 0;
    mc->mc_pg[0] = nullptr;
    mc->mc_ki[0] = 0;
    MC_SET_OVPG(mc, NULL);
    mc->mc_flags = txn->mt_flags & (C_ORIG_RDONLY | C_WRITEMAP);
    if ((*mc->mc_dbflag & DB_STALE) != 0)
    {
        fds_page_search(mc, nullptr, FDS_PS_ROOTONLY);
    }
}

auto fds_cursor_open(FDS_txn* txn, FDS_dbi dbi, FDS_cursor** cursor) -> int
{
    FDS_cursor* mc;
    size_t size = sizeof(FDS_cursor);

    if (cursor == nullptr || (TXN_DBI_EXIST(txn, dbi, DB_VALID) == 0))
        return EINVAL;

    if ((txn->mt_flags & FDS_TXN_BLOCKED) != 0U)
        return FDS_BAD_TXN;

    if (dbi == FREE_DBI && !F_ISSET(txn->mt_flags, FDS_TXN_RDONLY))
        return EINVAL;

    mc = (FDS_cursor*)malloc(size);
    if (mc != nullptr)
    {
        fds_cursor_init(mc, txn, dbi);
        if (txn->mt_cursors != nullptr)
        {
            mc->mc_next = txn->mt_cursors[dbi];
            txn->mt_cursors[dbi] = mc;
            mc->mc_flags |= C_UNTRACK;
        }
    }
    else
    {
        return ENOMEM;
    }

    DPRINTF(("%p, %u = %p", txn, dbi, mc));
    *cursor = mc;

    return FDS_SUCCESS;
}

auto fds_cursor_renew(FDS_txn* txn, FDS_cursor* cursor) -> int
{
    if (cursor == nullptr || (TXN_DBI_EXIST(txn, cursor->mc_dbi, DB_VALID) == 0))
        return EINVAL;

    if (((cursor->mc_flags & C_UNTRACK) != 0U) || (txn->mt_cursors != nullptr))
        return EINVAL;

    if ((txn->mt_flags & FDS_TXN_BLOCKED) != 0U)
        return FDS_BAD_TXN;

    fds_cursor_init(cursor, txn, cursor->mc_dbi);
    return FDS_SUCCESS;
}

// Return the count of duplicate data items for the current key
auto fds_cursor_count(FDS_cursor* mc, fds_size_t* countp) -> int
{
    if (mc == nullptr || countp == nullptr)
        return EINVAL;

    if ((mc->mc_txn->mt_flags & FDS_TXN_BLOCKED) != 0U)
        return FDS_BAD_TXN;

    if ((mc->mc_flags & C_INITIALIZED) == 0U)
        return EINVAL;

    if (mc->mc_snum == 0U)
        return FDS_NOTFOUND;

    if ((mc->mc_flags & C_EOF) != 0U)
    {
        if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mc->mc_pg[mc->mc_top]))
            return FDS_NOTFOUND;
        mc->mc_flags ^= C_EOF;
    }

    // No duplicates supported - always return 1
    *countp = 1;
    return FDS_SUCCESS;
}

void fds_cursor_close(FDS_cursor* cursor)
{
    DPRINTF(("%p", cursor));
    if ((cursor != nullptr) && (cursor->mc_backup == nullptr))
    {
        /* Remove from txn, if tracked.
         * A read-only txn (!C_UNTRACK) may have been freed already,
         * so do not peek inside it.  Only write txns track cursors.
         */
        if (((cursor->mc_flags & C_UNTRACK) != 0U) && (cursor->mc_txn->mt_cursors != nullptr))
        {
            FDS_cursor** prev = &cursor->mc_txn->mt_cursors[cursor->mc_dbi];
            while ((*prev != nullptr) && *prev != cursor)
                prev = &(*prev)->mc_next;
            if (*prev == cursor)
                *prev = cursor->mc_next;
        }
        free(cursor);
    }
}

auto fds_cursor_txn(FDS_cursor* cursor) -> FDS_txn*
{
    if (cursor == nullptr)
        return nullptr;
    return cursor->mc_txn;
}

auto fds_cursor_dbi(FDS_cursor* cursor) -> FDS_dbi
{
    return cursor->mc_dbi;
}

// Copy the contents of a cursor.
//
// csrc The cursor to copy from.
// cdst The cursor to copy to.
void fds_cursor_copy(const FDS_cursor* csrc, FDS_cursor* cdst)
{
    unsigned int i;

    cdst->mc_txn = csrc->mc_txn;
    cdst->mc_dbi = csrc->mc_dbi;
    cdst->mc_db = csrc->mc_db;
    cdst->mc_dbx = csrc->mc_dbx;
    cdst->mc_snum = csrc->mc_snum;
    cdst->mc_top = csrc->mc_top;
    cdst->mc_flags = csrc->mc_flags;
    MC_SET_OVPG(cdst, MC_OVPG(csrc));

    for (i = 0; i < csrc->mc_snum; i++)
    {
        cdst->mc_pg[i] = csrc->mc_pg[i];
        cdst->mc_ki[i] = csrc->mc_ki[i];
    }
}

// Complete a delete operation started by #fds_cursor_del().
auto fds_cursor_del0(FDS_cursor* mc) -> int
{
    int rc;
    FDS_page* mp;
    indx_t ki;
    unsigned int nkeys;
    FDS_cursor* m2;
    FDS_cursor* m3;
    FDS_dbi dbi = mc->mc_dbi;

    ki = mc->mc_ki[mc->mc_top];
    mp = mc->mc_pg[mc->mc_top];
    fds_node_del(mc, mc->mc_db->md_pad);
    mc->mc_db->md_entries--;
    {
        // Adjust other cursors pointing to mp
        for (m2 = mc->mc_txn->mt_cursors[dbi]; m2 != nullptr; m2 = m2->mc_next)
        {
            m3 = m2;
            if ((m2->mc_flags & m3->mc_flags & C_INITIALIZED) == 0U)
                continue;
            if (m3 == mc || m3->mc_snum < mc->mc_snum)
                continue;
            if (m3->mc_pg[mc->mc_top] == mp)
            {
                if (m3->mc_ki[mc->mc_top] == ki)
                {
                    m3->mc_flags |= C_DEL;
                    continue;
                }
                if (m3->mc_ki[mc->mc_top] > ki)
                {
                    m3->mc_ki[mc->mc_top]--;
                }
            }
        }
    }
    rc = fds_rebalance(mc);
    if (rc != 0)
        goto fail;

    /* DB is totally empty now, just bail out.
     * Other cursors adjustments were already done
     * by fds_rebalance and aren't needed here.
     */
    if (mc->mc_snum == 0U)
    {
        mc->mc_flags |= C_EOF;
        return rc;
    }

    mp = mc->mc_pg[mc->mc_top];
    nkeys = NUMKEYS(mp);

    // Adjust other cursors pointing to mp
    for (m2 = mc->mc_txn->mt_cursors[dbi]; (rc == 0) && (m2 != nullptr); m2 = m2->mc_next)
    {
        m3 = m2;
        if ((m2->mc_flags & m3->mc_flags & C_INITIALIZED) == 0U)
            continue;
        if (m3->mc_snum < mc->mc_snum)
            continue;
        if (m3->mc_pg[mc->mc_top] == mp)
        {
            if (m3->mc_ki[mc->mc_top] >= mc->mc_ki[mc->mc_top])
            {
                // if m3 points past last node in page, find next sibling
                if (m3->mc_ki[mc->mc_top] >= nkeys)
                {
                    rc = fds_cursor_sibling(m3, 1);
                    if (rc == FDS_NOTFOUND)
                    {
                        m3->mc_flags |= C_EOF;
                        rc = FDS_SUCCESS;
                        continue;
                    }
                    if (rc != 0)
                        goto fail;
                }
            }
        }
    }
    mc->mc_flags |= C_DEL;

fail:
    if (rc != 0)
        mc->mc_txn->mt_flags |= FDS_TXN_ERROR;
    return rc;
}

// Replace the key for a branch node with a new key.
// Set #FDS_TXN_ERROR on failure.
//
// mc Cursor pointing to the node to operate on.
// key The new key to use.
// 0 on success, non-zero on failure.
auto fds_update_key(FDS_cursor* mc, FDS_val* key) -> int
{
    FDS_page* mp;
    FDS_node* node;
    char* base;
    size_t len;
    int delta;
    int ksize;
    int oksize;
    indx_t ptr;
    indx_t i;
    indx_t numkeys;
    indx_t indx;
    DKBUF;

    indx = mc->mc_ki[mc->mc_top];
    mp = mc->mc_pg[mc->mc_top];
    node = NODEPTR(mp, indx);
    ptr = mp->mp_ptrs[indx];
#if FDS_DEBUG
    {
        FDS_val k2;
        char kbuf2[(DKBUF_MAXKEYSIZE * 2) + 1];
        k2.mv_data = NODEKEY(node);
        k2.mv_size = node->mn_ksize;
        DPRINTF(("update key %u (ofs %u) [%s] to [%s] on page %" Yu,
                 indx,
                 ptr,
                 fds_dkey(&k2, kbuf2),
                 DKEY(key),
                 mp->mp_pgno));
    }
#endif

    // Sizes must be 2-byte aligned.
    ksize = EVEN(key->mv_size);
    oksize = EVEN(node->mn_ksize);
    delta = ksize - oksize;

    // Shift node contents if EVEN(key length) changed.
    if (delta != 0)
    {
        if (delta > 0 && SIZELEFT(mp) < delta)
        {
            pgno_t pgno;
            // not enough space left, do a delete and split
            DPRINTF(("Not enough room, delta = %d, splitting...", delta));
            pgno = NODEPGNO(node);
            fds_node_del(mc, 0);
            return fds_page_split(mc, key, nullptr, pgno, 0);
        }

        numkeys = NUMKEYS(mp);
        for (i = 0; i < numkeys; i++)
        {
            if (mp->mp_ptrs[i] <= ptr)
                mp->mp_ptrs[i] -= delta;
        }

        base = (char*)mp + mp->mp_upper + PAGEBASE;
        len = ptr - mp->mp_upper + NODESIZE;
        memmove(base - delta, base, len);
        mp->mp_upper -= delta;

        node = NODEPTR(mp, indx);
    }

    // But even if no shift was needed, update ksize
    if (node->mn_ksize != key->mv_size)
        node->mn_ksize = key->mv_size;

    if (key->mv_size != 0U)
        memcpy(NODEKEY(node), key->mv_data, key->mv_size);

    return FDS_SUCCESS;
}

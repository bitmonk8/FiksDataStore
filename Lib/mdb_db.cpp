#include "mdb_db.h"

#include "mdb_btree.h"
#include "mdb_compare.h"
#include "mdb_cursor.h"
#include "mdb_debug.h"
#include "mdb_env.h"
#include "mdb_txn.h"

// Set the default comparison functions for a database.
// Called immediately after a database is opened to set the defaults.
// The user can then override them with mdb_set_compare().
static void mdb_default_cmp(MDB_txn* txn, MDB_dbi dbi)
{
    uint16_t f = txn->mt_dbs[dbi].md_flags;

    txn->mt_dbxs[dbi].md_cmp = ((f & MDB_REVERSEKEY) != 0) ? mdb_cmp_memnr : mdb_cmp_memn;

    txn->mt_dbxs[dbi].md_dcmp = nullptr;
}

auto mdb_dbi_open(MDB_txn* txn, const char* name, unsigned int flags, MDB_dbi* dbi) -> int
{
    if ((flags & ~VALID_FLAGS) != 0U)
        return EINVAL;
    if ((txn->mt_flags & MDB_TXN_BLOCKED) != 0U)
        return MDB_BAD_TXN;

    // main DB?
    if (name == nullptr)
    {
        *dbi = MAIN_DBI;
        if ((flags & PERSISTENT_FLAGS) != 0U)
        {
            uint16_t f2 = flags & PERSISTENT_FLAGS;
            // make sure flag changes get committed
            if ((txn->mt_dbs[MAIN_DBI].md_flags | f2) != txn->mt_dbs[MAIN_DBI].md_flags)
            {
                txn->mt_dbs[MAIN_DBI].md_flags |= f2;
                txn->mt_flags |= MDB_TXN_DIRTY;
            }
        }
        mdb_default_cmp(txn, MAIN_DBI);
        MDB_TRACE(("%p, (null), %u = %u", txn, flags, MAIN_DBI));
        return MDB_SUCCESS;
    }

    if (txn->mt_dbxs[MAIN_DBI].md_cmp == nullptr)
    {
        mdb_default_cmp(txn, MAIN_DBI);
    }

    // Is the DB already open?
    size_t len{strlen(name)};
    unsigned int unused{};
    for (MDB_dbi i{CORE_DBS}; i < txn->mt_numdbs; i++)
    {
        if (txn->mt_dbxs[i].md_name.mv_size == 0U)
        {
            // Remember this free slot
            if (unused == 0U)
                unused = i;
            continue;
        }
        if (len == txn->mt_dbxs[i].md_name.mv_size &&
            (strncmp(name, (const char*)txn->mt_dbxs[i].md_name.mv_data, len) == 0))
        {
            *dbi = i;
            return MDB_SUCCESS;
        }
    }

    // If no free slot and max hit, fail
    if ((unused == 0U) && txn->mt_numdbs >= txn->mt_env->me_maxdbs)
        return MDB_DBS_FULL;

    // Cannot mix named databases with some mainDB flags - removed MDB_INTEGERKEY check

    // Find the DB info
    int dbflag{DB_NEW | DB_VALID | DB_USRVALID};
    int exact{};
    MDB_val key{};
    key.mv_size = len;
    key.mv_data = (void*)name;
    MDB_val data{};
    MDB_cursor mc{};
    mdb_cursor_init(&mc, txn, MAIN_DBI, nullptr);
    int rc{mdb_cursor_set(&mc, &key, &data, MDB_SET, &exact)};
    if (rc == MDB_SUCCESS)
    {
        // make sure this is actually a DB
        MDB_node* node = NODEPTR(mc.mc_pg[mc.mc_top], mc.mc_ki[mc.mc_top]);
        if ((node->mn_flags & F_SUBDATA) != F_SUBDATA)
            return MDB_INCOMPATIBLE;
    }
    else
    {
        if (rc != MDB_NOTFOUND || ((flags & MDB_CREATE) == 0U))
            return rc;
        if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
            return EACCES;
    }

    // Done here so we cannot fail after creating a new DB
    char* namedup{mdb_strdup(name)};
    if (namedup == nullptr)
        return ENOMEM;

    if (rc != 0)
    {
        // MDB_NOTFOUND and MDB_CREATE: Create new DB
        MDB_db dummy{};
        data.mv_size = sizeof(MDB_db);
        data.mv_data = &dummy;
        memset(&dummy, 0, sizeof(dummy));
        dummy.md_root = P_INVALID;
        dummy.md_flags = flags & PERSISTENT_FLAGS;
        WITH_CURSOR_TRACKING(mc, rc = mdb_cursor_put_impl(&mc, &key, &data, F_SUBDATA));
        dbflag |= DB_DIRTY;
    }

    if (rc != 0)
    {
        free(namedup);
    }
    else
    {
        // Got info, register DBI in this txn
        unsigned int slot = (unused != 0U) ? unused : txn->mt_numdbs;
        txn->mt_dbxs[slot].md_name.mv_data = namedup;
        txn->mt_dbxs[slot].md_name.mv_size = len;
        txn->mt_dbflags[slot] = dbflag;
        // txn-> and env-> are the same in read txns, use
        // tmp variable to avoid undefined assignment
        unsigned int seq{++txn->mt_env->me_dbiseqs[slot]};
        txn->mt_dbiseqs[slot] = seq;

        memcpy(&txn->mt_dbs[slot], data.mv_data, sizeof(MDB_db));
        *dbi = slot;
        mdb_default_cmp(txn, slot);
        if (unused == 0U)
        {
            txn->mt_numdbs++;
        }
        MDB_TRACE(("%p, %s, %u = %u", txn, name, flags, slot));
    }

    return rc;
}

void mdb_dbi_close(MDB_env* env, MDB_dbi dbi)
{
    if (dbi < CORE_DBS || dbi >= env->me_maxdbs)
        return;
    char* ptr{(char*)env->me_dbxs[dbi].md_name.mv_data};
    // If there was no name, this was already closed
    if (ptr != nullptr)
    {
        MDB_TRACE(("%p, %u", env, dbi));
        env->me_dbxs[dbi].md_name.mv_data = nullptr;
        env->me_dbxs[dbi].md_name.mv_size = 0;
        env->me_dbflags[dbi] = 0;
        env->me_dbiseqs[dbi]++;
        free(ptr);
    }
}

auto mdb_dbi_flags(MDB_txn* txn, MDB_dbi dbi, unsigned int* flags) -> int
{
    // We could return the flags for the FREE_DBI too but what's the point?
    if (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0)
        return EINVAL;
    *flags = txn->mt_dbs[dbi].md_flags & PERSISTENT_FLAGS;
    return MDB_SUCCESS;
}

// Add all the DB's pages to the free list.
// mc: Cursor on the DB to free.
// subs: non-Zero to check for sub-DBs in this DB.
// Returns 0 on success, non-zero on failure.
auto mdb_drop0(MDB_cursor* mc, int subs) -> int
{
    int rc{mdb_page_search(mc, nullptr, MDB_PS_FIRST)};
    if (rc == MDB_SUCCESS)
    {
        MDB_txn* txn = mc->mc_txn;

        // Sub-DBs have no ovpages/DBs. Omit scanning leaves.
        // This also avoids any P_LEAF2 pages, which have no nodes.
        // Also if the DB doesn't have sub-DBs and has no overflow
        // pages, omit scanning leaves.
        if (((mc->mc_flags & C_SUB) != 0U) || ((subs == 0) && (mc->mc_db->md_overflow_pages == 0U)))
            mdb_cursor_pop(mc);

        MDB_cursor mx{};
        mdb_cursor_copy(mc, &mx);
        while (mc->mc_snum > 0)
        {
            MDB_page* mp = mc->mc_pg[mc->mc_top];
            unsigned n = NUMKEYS(mp);
            if (IS_LEAF(mp))
            {
                for (unsigned int i{}; i < n; i++)
                {
                    MDB_node* ni{NODEPTR(mp, i)};
                    if ((ni->mn_flags & F_BIGDATA) != 0)
                    {
                        MDB_page* omp;
                        pgno_t pg;
                        memcpy(&pg, NODEDATA(ni), sizeof(pg));
                        rc = mdb_page_get(mc, pg, &omp, nullptr);
                        if (rc != 0)
                            goto done;
                        mdb_cassert(mc, IS_OVERFLOW(omp));
                        rc = mdb_midl_append_range(&txn->mt_free_pgs, pg, omp->mp_pages);
                        if (rc != 0)
                            goto done;
                        mc->mc_db->md_overflow_pages -= omp->mp_pages;
                        if ((mc->mc_db->md_overflow_pages == 0U) && (subs == 0))
                            break;
                    }
                    else if ((subs != 0) && ((ni->mn_flags & F_SUBDATA) != 0))
                    {
                        // Sub-database handling - simplified without duplicate support
                        // This would need proper sub-database handling implementation
                        // For now, we'll skip this case
                    }
                }
                if ((subs == 0) && (mc->mc_db->md_overflow_pages == 0U))
                    goto pop;
            }
            else
            {
                rc = mdb_midl_need(&txn->mt_free_pgs, n);
                if (rc != 0)
                    goto done;
                for (unsigned int i{}; i < n; i++)
                {
                    pgno_t pg;
                    MDB_node* ni{NODEPTR(mp, i)};
                    pg = NODEPGNO(ni);
                    // free it
                    mdb_midl_xappend(txn->mt_free_pgs, pg);
                }
            }
            if (mc->mc_top == 0U)
                break;
            mc->mc_ki[mc->mc_top] = n;
            rc = mdb_cursor_sibling(mc, 1);
            if (rc != 0)
            {
                if (rc != MDB_NOTFOUND)
                    goto done;
                // no more siblings, go back to beginning
                // of previous level.
            pop:
                mdb_cursor_pop(mc);
                mc->mc_ki[0] = 0;
                for (unsigned int i{1}; i < mc->mc_snum; i++)
                {
                    mc->mc_ki[i] = 0;
                    mc->mc_pg[i] = mx.mc_pg[i];
                }
            }
        }
        // free it
        rc = mdb_midl_append(&txn->mt_free_pgs, mc->mc_db->md_root);
    done:
        if (rc != 0)
            txn->mt_flags |= MDB_TXN_ERROR;
    }
    else if (rc == MDB_NOTFOUND)
    {
        rc = MDB_SUCCESS;
    }
    mc->mc_flags &= ~C_INITIALIZED;
    return rc;
}

static auto mdb_del0(MDB_txn* txn, MDB_dbi dbi, MDB_val* key, MDB_val* data, unsigned flags) -> int
{
    DKBUF;

    DPRINTF(("====> delete db %u key [%s]", dbi, DKEY(key)));

    MDB_cursor mc{};
    mdb_cursor_init(&mc, txn, dbi, nullptr);

    MDB_cursor_op op{};
    MDB_val* xdata{nullptr};
    if (data != nullptr)
    {
        op = MDB_SET;     // Simplified without MDB_GET_BOTH
        xdata = nullptr;  // Ignore data parameter since no duplicates
    }
    else
    {
        op = MDB_SET;
        xdata = nullptr;
    }
    int exact{};
    int rc{mdb_cursor_set(&mc, key, xdata, op, &exact)};
    if (rc == 0)
    {
        // let mdb_page_split know about this cursor if needed:
        // delete will trigger a rebalance; if it needs to move
        // a node from one page to another, it will have to
        // update the parent's separator key(s). If the new sepkey
        // is larger than the current one, the parent page may
        // run out of space, triggering a split. We need this
        // cursor to be consistent until the end of the rebalance.
        mc.mc_next = txn->mt_cursors[dbi];
        txn->mt_cursors[dbi] = &mc;
        rc = mdb_cursor_del_impl(&mc, flags);
        txn->mt_cursors[dbi] = mc.mc_next;
    }
    return rc;
}

auto mdb_del(MDB_txn* txn, MDB_dbi dbi, MDB_val* key, MDB_val* data) -> int
{
    DKBUF;
    DDBUF;
    if (key == nullptr || (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0))
        return EINVAL;

    if ((txn->mt_flags & (MDB_TXN_RDONLY | MDB_TXN_BLOCKED)) != 0U)
        return ((txn->mt_flags & MDB_TXN_RDONLY) != 0U) ? EACCES : MDB_BAD_TXN;

    // Without duplicate support, always ignore data parameter
    data = nullptr;

#if MDB_DEBUG
    MDB_TRACE(("%p, %u, %" Z "u[%s], %" Z "u%s",
               txn,
               dbi,
               key ? key->mv_size : 0,
               DKEY(key),
               data ? data->mv_size : 0,
               data ? mdb_dval(txn, dbi, data, dbuf) : ""));
#endif
    return mdb_del0(txn, dbi, key, data, 0);
}

auto mdb_drop(MDB_txn* txn, MDB_dbi dbi, int del) -> int
{
    if ((unsigned)del > 1 || (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0))
        return EINVAL;

    if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
        return EACCES;

    if (TXN_DBI_CHANGED(txn, dbi))
        return MDB_BAD_DBI;

    MDB_cursor* mc{nullptr};
    int rc{mdb_cursor_open(txn, dbi, &mc)};
    if (rc != 0)
        return rc;

    MDB_TRACE(("%u, %d", dbi, del));
    rc = mdb_drop0(mc, 0);  // No duplicate support, so subs = 0
    // Invalidate the dropped DB's cursors
    for (MDB_cursor* m2{txn->mt_cursors[dbi]}; m2 != nullptr; m2 = m2->mc_next)
        m2->mc_flags &= ~(C_INITIALIZED | C_EOF);
    if (rc != 0)
        goto leave;

    // Can't delete the main DB
    if ((del != 0) && dbi >= CORE_DBS)
    {
        rc = mdb_del0(txn, MAIN_DBI, &mc->mc_dbx->md_name, nullptr, F_SUBDATA);
        if (rc == 0)
        {
            txn->mt_dbflags[dbi] = DB_STALE;
            mdb_dbi_close(txn->mt_env, dbi);
        }
        else
        {
            txn->mt_flags |= MDB_TXN_ERROR;
        }
    }
    else
    {
        // reset the DB record, mark it dirty
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

auto mdb_put(MDB_txn* txn, MDB_dbi dbi, MDB_val* key, MDB_val* data, unsigned int flags) -> int
{
    DKBUF;
    DDBUF;

    if (key == nullptr || data == nullptr || (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0))
        return EINVAL;

    if ((flags & ~(MDB_NOOVERWRITE | MDB_RESERVE | MDB_APPEND)) != 0U)
        return EINVAL;

    if ((txn->mt_flags & (MDB_TXN_RDONLY | MDB_TXN_BLOCKED)) != 0U)
        return ((txn->mt_flags & MDB_TXN_RDONLY) != 0U) ? EACCES : MDB_BAD_TXN;

#if MDB_DEBUG
    MDB_TRACE(("%p, %u, %" Z "u[%s], %" Z "u%s, %u",
               txn,
               dbi,
               key ? key->mv_size : 0,
               DKEY(key),
               data->mv_size,
               mdb_dval(txn, dbi, data, dbuf),
               flags));
#endif
    MDB_cursor mc{};
    mdb_cursor_init(&mc, txn, dbi, nullptr);
    mc.mc_next = txn->mt_cursors[dbi];
    txn->mt_cursors[dbi] = &mc;
    int rc{mdb_cursor_put_impl(&mc, key, data, flags)};
    txn->mt_cursors[dbi] = mc.mc_next;
    return rc;
}

auto mdb_set_compare(MDB_txn* txn, MDB_dbi dbi, MDB_cmp_func cmp) -> int
{
    if (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0)
        return EINVAL;

    txn->mt_dbxs[dbi].md_cmp = cmp;
    return MDB_SUCCESS;
}

auto mdb_get(MDB_txn* txn, MDB_dbi dbi, MDB_val* key, MDB_val* data) -> int
{
    DKBUF;

    DPRINTF(("===> get db %u key [%s]", dbi, DKEY(key)));

    if (key == nullptr || data == nullptr || (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0))
        return EINVAL;

    if ((txn->mt_flags & MDB_TXN_BLOCKED) != 0U)
        return MDB_BAD_TXN;

    MDB_cursor mc{};
    mdb_cursor_init(&mc, txn, dbi, nullptr);
    int exact{};
    int rc{mdb_cursor_set(&mc, key, data, MDB_SET, &exact)};
    return rc;
}

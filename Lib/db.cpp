#include "db.h"

#include "btree.h"
#include "compare.h"
#include "cursor.h"
#include "debug.h"
#include "env.h"
#include "txn.h"

// Set the default comparison functions for a database.
// Called immediately after a database is opened to set the defaults.
// The user can then override them with fds_set_compare().
static void fds_default_cmp(FDS_txn* txn, FDS_dbi dbi)
{
    uint16_t f = txn->mt_dbs[dbi].md_flags;

    txn->mt_dbxs[dbi].md_cmp = ((f & FDS_REVERSEKEY) != 0) ? fds_cmp_memnr : fds_cmp_memn;

    txn->mt_dbxs[dbi].md_dcmp = nullptr;
}

auto fds_dbi_open(FDS_txn* txn, const char* name, unsigned int flags, FDS_dbi* dbi) -> int
{
    if ((flags & ~VALID_FLAGS) != 0U)
        return EINVAL;
    if ((txn->mt_flags & FDS_TXN_BLOCKED) != 0U)
        return FDS_BAD_TXN;

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
                txn->mt_flags |= FDS_TXN_DIRTY;
            }
        }
        fds_default_cmp(txn, MAIN_DBI);
        DPRINTF(("%p, (null), %u = %u", txn, flags, MAIN_DBI));
        return FDS_SUCCESS;
    }

    if (txn->mt_dbxs[MAIN_DBI].md_cmp == nullptr)
    {
        fds_default_cmp(txn, MAIN_DBI);
    }

    // Is the DB already open?
    size_t len{strlen(name)};
    unsigned int unused{};
    for (FDS_dbi i{CORE_DBS}; i < txn->mt_numdbs; i++)
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
            return FDS_SUCCESS;
        }
    }

    // If no free slot and max hit, fail
    if ((unused == 0U) && txn->mt_numdbs >= txn->mt_env->me_maxdbs)
        return FDS_DBS_FULL;

    // Cannot mix named databases with some mainDB flags

    // Find the DB info
    int dbflag{DB_NEW | DB_VALID | DB_USRVALID};
    int exact{};
    FDS_val key{};
    key.mv_size = len;
    key.mv_data = (void*)name;
    FDS_val data{};
    FDS_cursor mc{};
    fds_cursor_init(&mc, txn, MAIN_DBI, nullptr);
    int rc{fds_cursor_set(&mc, &key, &data, FDS_SET, &exact)};
    if (rc == FDS_SUCCESS)
    {
        // make sure this is actually a DB
        FDS_node* node = NODEPTR(mc.mc_pg[mc.mc_top], mc.mc_ki[mc.mc_top]);
        if ((node->mn_flags & F_SUBDATA) != F_SUBDATA)
            return FDS_INCOMPATIBLE;
    }
    else
    {
        if (rc != FDS_NOTFOUND || ((flags & FDS_CREATE) == 0U))
            return rc;
        if (F_ISSET(txn->mt_flags, FDS_TXN_RDONLY))
            return EACCES;
    }

    // Done here so we cannot fail after creating a new DB
    char* namedup{fds_strdup(name)};
    if (namedup == nullptr)
        return ENOMEM;

    if (rc != 0)
    {
        // FDS_NOTFOUND and FDS_CREATE: Create new DB
        FDS_db dummy{};
        data.mv_size = sizeof(FDS_db);
        data.mv_data = &dummy;
        memset(&dummy, 0, sizeof(dummy));
        dummy.md_root = P_INVALID;
        dummy.md_flags = flags & PERSISTENT_FLAGS;
        WITH_CURSOR_TRACKING(mc, rc = fds_cursor_put_impl(&mc, &key, &data, F_SUBDATA));
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

        memcpy(&txn->mt_dbs[slot], data.mv_data, sizeof(FDS_db));
        *dbi = slot;
        fds_default_cmp(txn, slot);
        if (unused == 0U)
        {
            txn->mt_numdbs++;
        }
        DPRINTF(("%p, %s, %u = %u", txn, name, flags, slot));
    }

    return rc;
}

void fds_dbi_close(FDS_env* env, FDS_dbi dbi)
{
    if (dbi < CORE_DBS || dbi >= env->me_maxdbs)
        return;
    char* ptr{(char*)env->me_dbxs[dbi].md_name.mv_data};
    // If there was no name, this was already closed
    if (ptr != nullptr)
    {
        DPRINTF(("%p, %u", env, dbi));
        env->me_dbxs[dbi].md_name.mv_data = nullptr;
        env->me_dbxs[dbi].md_name.mv_size = 0;
        env->me_dbflags[dbi] = 0;
        env->me_dbiseqs[dbi]++;
        free(ptr);
    }
}

auto fds_dbi_flags(FDS_txn* txn, FDS_dbi dbi, unsigned int* flags) -> int
{
    // We could return the flags for the FREE_DBI too but what's the point?
    if (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0)
        return EINVAL;
    *flags = txn->mt_dbs[dbi].md_flags & PERSISTENT_FLAGS;
    return FDS_SUCCESS;
}

// Add all the DB's pages to the free list.
// mc: Cursor on the DB to free.
// subs: non-Zero to check for sub-DBs in this DB.
// Returns 0 on success, non-zero on failure.
auto fds_drop0(FDS_cursor* mc, int subs) -> int
{
    int rc{fds_page_search(mc, nullptr, FDS_PS_FIRST)};
    if (rc == FDS_SUCCESS)
    {
        FDS_txn* txn = mc->mc_txn;

        // Sub-DBs have no ovpages/DBs. Omit scanning leaves.
        // This also avoids any P_LEAF2 pages, which have no nodes.
        // Also if the DB doesn't have sub-DBs and has no overflow
        // pages, omit scanning leaves.
        if (((mc->mc_flags & C_SUB) != 0U) || ((subs == 0) && (mc->mc_db->md_overflow_pages == 0U)))
            fds_cursor_pop(mc);

        FDS_cursor mx{};
        fds_cursor_copy(mc, &mx);
        while (mc->mc_snum > 0)
        {
            FDS_page* mp = mc->mc_pg[mc->mc_top];
            unsigned n = NUMKEYS(mp);
            if (IS_LEAF(mp))
            {
                for (unsigned int i{}; i < n; i++)
                {
                    FDS_node* ni{NODEPTR(mp, i)};
                    if ((ni->mn_flags & F_BIGDATA) != 0)
                    {
                        FDS_page* omp;
                        pgno_t pg;
                        memcpy(&pg, NODEDATA(ni), sizeof(pg));
                        rc = fds_page_get(mc, pg, &omp, nullptr);
                        if (rc != 0)
                            goto done;
                        fds_cassert(mc, IS_OVERFLOW(omp));
                        rc = fds_midl_append_range(&txn->mt_free_pgs, pg, omp->mp_pages);
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
                rc = fds_midl_need(&txn->mt_free_pgs, n);
                if (rc != 0)
                    goto done;
                for (unsigned int i{}; i < n; i++)
                {
                    pgno_t pg;
                    FDS_node* ni{NODEPTR(mp, i)};
                    pg = NODEPGNO(ni);
                    // free it
                    fds_midl_xappend(txn->mt_free_pgs, pg);
                }
            }
            if (mc->mc_top == 0U)
                break;
            mc->mc_ki[mc->mc_top] = n;
            rc = fds_cursor_sibling(mc, 1);
            if (rc != 0)
            {
                if (rc != FDS_NOTFOUND)
                    goto done;
                // no more siblings, go back to beginning
                // of previous level.
            pop:
                fds_cursor_pop(mc);
                mc->mc_ki[0] = 0;
                for (unsigned int i{1}; i < mc->mc_snum; i++)
                {
                    mc->mc_ki[i] = 0;
                    mc->mc_pg[i] = mx.mc_pg[i];
                }
            }
        }
        // free it
        rc = fds_midl_append(&txn->mt_free_pgs, mc->mc_db->md_root);
    done:
        if (rc != 0)
            txn->mt_flags |= FDS_TXN_ERROR;
    }
    else if (rc == FDS_NOTFOUND)
    {
        rc = FDS_SUCCESS;
    }
    mc->mc_flags &= ~C_INITIALIZED;
    return rc;
}

static auto fds_del0(FDS_txn* txn, FDS_dbi dbi, FDS_val* key, FDS_val* data, unsigned flags) -> int
{
    DKBUF;

    DPRINTF(("====> delete db %u key [%s]", dbi, DKEY(key)));

    FDS_cursor mc{};
    fds_cursor_init(&mc, txn, dbi, nullptr);

    FDS_cursor_op op{};
    FDS_val* xdata{nullptr};
    op = FDS_SET;
    xdata = nullptr;
    int exact{};
    int rc{fds_cursor_set(&mc, key, xdata, op, &exact)};
    if (rc == 0)
    {
        // let fds_page_split know about this cursor if needed:
        // delete will trigger a rebalance; if it needs to move
        // a node from one page to another, it will have to
        // update the parent's separator key(s). If the new sepkey
        // is larger than the current one, the parent page may
        // run out of space, triggering a split. We need this
        // cursor to be consistent until the end of the rebalance.
        mc.mc_next = txn->mt_cursors[dbi];
        txn->mt_cursors[dbi] = &mc;
        rc = fds_cursor_del_impl(&mc, flags);
        txn->mt_cursors[dbi] = mc.mc_next;
    }
    return rc;
}

auto fds_del(FDS_txn* txn, FDS_dbi dbi, FDS_val* key, FDS_val* data) -> int
{
    DKBUF;
    DDBUF;
    if (key == nullptr || (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0))
        return EINVAL;

    if ((txn->mt_flags & (FDS_TXN_RDONLY | FDS_TXN_BLOCKED)) != 0U)
        return ((txn->mt_flags & FDS_TXN_RDONLY) != 0U) ? EACCES : FDS_BAD_TXN;

    // Without duplicate support, always ignore data parameter
    data = nullptr;

#if FDS_DEBUG
    DPRINTF(("%p, %u, %" Z "u[%s], %" Z "u%s",
               txn,
               dbi,
               key ? key->mv_size : 0,
               DKEY(key),
               data ? data->mv_size : 0,
               data ? fds_dval(txn, dbi, data, dbuf) : ""));
#endif
    return fds_del0(txn, dbi, key, data, 0);
}

auto fds_drop(FDS_txn* txn, FDS_dbi dbi, int del) -> int
{
    if ((unsigned)del > 1 || (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0))
        return EINVAL;

    if (F_ISSET(txn->mt_flags, FDS_TXN_RDONLY))
        return EACCES;

    if (TXN_DBI_CHANGED(txn, dbi))
        return FDS_BAD_DBI;

    FDS_cursor* mc{nullptr};
    int rc{fds_cursor_open(txn, dbi, &mc)};
    if (rc != 0)
        return rc;

    DPRINTF(("%u, %d", dbi, del));
    rc = fds_drop0(mc, 0);  // No duplicate support, so subs = 0
    // Invalidate the dropped DB's cursors
    for (FDS_cursor* m2{txn->mt_cursors[dbi]}; m2 != nullptr; m2 = m2->mc_next)
        m2->mc_flags &= ~(C_INITIALIZED | C_EOF);
    if (rc != 0)
        goto leave;

    // Can't delete the main DB
    if ((del != 0) && dbi >= CORE_DBS)
    {
        rc = fds_del0(txn, MAIN_DBI, &mc->mc_dbx->md_name, nullptr, F_SUBDATA);
        if (rc == 0)
        {
            txn->mt_dbflags[dbi] = DB_STALE;
            fds_dbi_close(txn->mt_env, dbi);
        }
        else
        {
            txn->mt_flags |= FDS_TXN_ERROR;
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

        txn->mt_flags |= FDS_TXN_DIRTY;
    }
leave:
    fds_cursor_close(mc);
    return rc;
}

auto fds_put(FDS_txn* txn, FDS_dbi dbi, FDS_val* key, FDS_val* data, unsigned int flags) -> int
{
    DKBUF;
    DDBUF;

    if (key == nullptr || data == nullptr || (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0))
        return EINVAL;

    if ((flags & ~(FDS_NOOVERWRITE | FDS_RESERVE | FDS_APPEND)) != 0U)
        return EINVAL;

    if ((txn->mt_flags & (FDS_TXN_RDONLY | FDS_TXN_BLOCKED)) != 0U)
        return ((txn->mt_flags & FDS_TXN_RDONLY) != 0U) ? EACCES : FDS_BAD_TXN;

#if FDS_DEBUG
    DPRINTF(("%p, %u, %" Z "u[%s], %" Z "u%s, %u",
               txn,
               dbi,
               key ? key->mv_size : 0,
               DKEY(key),
               data->mv_size,
               fds_dval(txn, dbi, data, dbuf),
               flags));
#endif
    FDS_cursor mc{};
    fds_cursor_init(&mc, txn, dbi, nullptr);
    mc.mc_next = txn->mt_cursors[dbi];
    txn->mt_cursors[dbi] = &mc;
    int rc{fds_cursor_put_impl(&mc, key, data, flags)};
    txn->mt_cursors[dbi] = mc.mc_next;
    return rc;
}

auto fds_set_compare(FDS_txn* txn, FDS_dbi dbi, FDS_cmp_func cmp) -> int
{
    if (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0)
        return EINVAL;

    txn->mt_dbxs[dbi].md_cmp = cmp;
    return FDS_SUCCESS;
}

auto fds_get(FDS_txn* txn, FDS_dbi dbi, FDS_val* key, FDS_val* data) -> int
{
    DKBUF;

    DPRINTF(("===> get db %u key [%s]", dbi, DKEY(key)));

    if (key == nullptr || data == nullptr || (TXN_DBI_EXIST(txn, dbi, DB_USRVALID) == 0))
        return EINVAL;

    if ((txn->mt_flags & FDS_TXN_BLOCKED) != 0U)
        return FDS_BAD_TXN;

    FDS_cursor mc{};
    fds_cursor_init(&mc, txn, dbi, nullptr);
    int exact{};
    int rc{fds_cursor_set(&mc, key, data, FDS_SET, &exact)};
    return rc;
}

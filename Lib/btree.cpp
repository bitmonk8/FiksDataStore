#include "btree.h"

#include "cursor.h"
#include "db.h"
#include "debug.h"
#include "env.h"
#include "page_io.h"
#include "txn.h"

#include <string.h>

#include <utility>

static auto mdb_page_loose(MDB_cursor* mc, MDB_page* mp) -> int
{
    auto* const txn = mc->mc_txn;
    const auto pgno = mp->mp_pgno;

    bool is_loose_page = false;

    if (((mp->mp_flags & P_DIRTY) != 0) && mc->mc_dbi != FREE_DBI)
    {
        if (txn->mt_parent != nullptr)
        {
            auto* const dl = txn->mt_u.dirty_list;
            // If txn has a parent, make sure the page is in our
            // dirty list.
            if (dl[0].mid != 0U)
            {
                // Rule: Declare 'x' close to first use. Use 'const' and 'auto'.
                const auto x = mdb_mid2l_search(dl, pgno);
                if (x <= dl[0].mid && dl[x].mid == pgno)
                {
                    if (mp != dl[x].mptr)
                    {  // bad cursor?
                        mc->mc_flags &= ~(C_INITIALIZED | C_EOF);
                        txn->mt_flags |= MDB_TXN_ERROR;
                        return MDB_PROBLEM;
                    }
                    // ok, it's ours
                    is_loose_page = true;
                }
            }
        }
        else
        {
            // no parent txn, so it's just ours
            is_loose_page = true;
        }
    }

    if (is_loose_page)
    {
        DPRINTF(("loosen db %d page %" Yu, DDBI(mc), mp->mp_pgno));
        NEXT_LOOSE_PAGE(mp) = txn->mt_loose_pgs;
        txn->mt_loose_pgs = mp;
        txn->mt_loose_count++;
        mp->mp_flags |= P_LOOSE;
    }
    else
    {
        // Rule: Declare 'rc' in the narrowest possible scope. Use 'const' and 'auto'.
        const auto rc = mdb_midl_append(&txn->mt_free_pgs, pgno);
        if (rc != 0)
            return rc;
    }

    return MDB_SUCCESS;
}

void mdb_page_copy(MDB_page* dst, MDB_page* src, unsigned int psize)
{
    enum
    {
        Align = sizeof(pgno_t)
    };
    indx_t upper = src->mp_upper;
    indx_t lower = src->mp_lower;
    indx_t unused = upper - lower;

    // If page isn't full, just copy the used portion. Adjust
    // alignment so memcpy may copy words instead of bytes.
    unused &= -Align;
    if (unused != 0U)
    {
        upper = (upper + PAGEBASE) & -Align;
        memcpy(dst, src, (lower + PAGEBASE + (Align - 1)) & -Align);
        memcpy(reinterpret_cast<pgno_t*>(reinterpret_cast<char*>(dst) + upper),
               reinterpret_cast<pgno_t*>(reinterpret_cast<char*>(src) + upper),
               psize - upper);
    }
    else
    {
        memcpy(dst, src, psize - unused);
    }
}

auto mdb_page_touch(MDB_cursor* mc) -> int
{
    auto* const mp = mc->mc_pg[mc->mc_top];
    auto* const txn = mc->mc_txn;
    MDB_page* new_page;

    if (!F_ISSET(MP_FLAGS(mp), P_DIRTY))
    {
        if ((txn->mt_flags & MDB_TXN_SPILLS) != 0U)
        {
            MDB_page* unspilled_page = nullptr;
            if (const auto rc = mdb_page_unspill(txn, mp, &unspilled_page); rc != 0)
            {
                txn->mt_flags |= MDB_TXN_ERROR;
                return rc;
            }
            if (unspilled_page != nullptr)
            {
                new_page = unspilled_page;
                goto done;
            }
        }
        if (const auto rc = mdb_midl_need(&txn->mt_free_pgs, 1); rc != 0)
        {
            txn->mt_flags |= MDB_TXN_ERROR;
            return rc;
        }
        if (const auto rc = mdb_page_alloc(mc, 1, &new_page); rc != 0)
        {
            txn->mt_flags |= MDB_TXN_ERROR;
            return rc;
        }

        const auto new_pgno = new_page->mp_pgno;
        DPRINTF(("touched db %d page %" Yu " -> %" Yu, DDBI(mc), mp->mp_pgno, new_pgno));
        mdb_cassert(mc, mp->mp_pgno != new_pgno);
        mdb_midl_xappend(txn->mt_free_pgs, mp->mp_pgno);
        // Update the parent page, if any, to point to the new page
        if (mc->mc_top != 0U)
        {
            auto* const parent = mc->mc_pg[mc->mc_top - 1];
            auto* node = NODEPTR(parent, mc->mc_ki[mc->mc_top - 1]);
            SETPGNO(node, new_pgno);
        }
        else
        {
            mc->mc_db->md_root = new_pgno;
        }
        mdb_page_copy(new_page, mp, txn->mt_env->me_psize);
        new_page->mp_pgno = new_pgno;
        new_page->mp_flags |= P_DIRTY;
    }
    else if ((txn->mt_parent != nullptr) && !IS_SUBP(mp))
    {
        const auto current_pgno = mp->mp_pgno;
        auto* const dl = txn->mt_u.dirty_list;
        // If txn has a parent, make sure the page is in our
        // dirty list.
        if (dl[0].mid != 0U)
        {
            const auto x = mdb_mid2l_search(dl, current_pgno);
            if (x <= dl[0].mid && dl[x].mid == current_pgno)
            {
                if (mp != dl[x].mptr)
                {  // bad cursor?
                    mc->mc_flags &= ~(C_INITIALIZED | C_EOF);
                    txn->mt_flags |= MDB_TXN_ERROR;
                    return MDB_PROBLEM;
                }
                return 0;
            }
        }
        mdb_cassert(mc, dl[0].mid < MDB_IDL_UM_MAX);
        // No - copy it
        new_page = mdb_page_malloc(txn, 1);
        if (new_page == nullptr)
            return ENOMEM;
        MDB_ID2 mid;
        mid.mid = current_pgno;
        mid.mptr = new_page;
        const auto rc = mdb_mid2l_insert(dl, &mid);
        mdb_cassert(mc, rc == 0);
        mdb_page_copy(new_page, mp, txn->mt_env->me_psize);
        new_page->mp_pgno = current_pgno;
        new_page->mp_flags |= P_DIRTY;
    }
    else
    {
        return 0;
    }

done:
    // Adjust cursors pointing to mp
    mc->mc_pg[mc->mc_top] = new_page;
    if ((mc->mc_flags & C_SUB) != 0U)
    {
        for (auto* m2 = txn->mt_cursors[mc->mc_dbi]; m2 != nullptr; m2 = m2->mc_next)
        {
            auto* m3 = &m2->mc_xcursor->mx_cursor;
            if (m3->mc_snum < mc->mc_snum)
                continue;
            if (m3->mc_pg[mc->mc_top] == mp)
                m3->mc_pg[mc->mc_top] = new_page;
        }
    }
    else
    {
        for (auto* m2 = txn->mt_cursors[mc->mc_dbi]; m2 != nullptr; m2 = m2->mc_next)
        {
            if (m2->mc_snum < mc->mc_snum)
                continue;
            if (m2 == mc)
                continue;
            if (m2->mc_pg[mc->mc_top] == mp)
            {
                m2->mc_pg[mc->mc_top] = new_page;
                if (IS_LEAF(new_page))
                    XCURSOR_REFRESH(m2, mc->mc_top, new_page);
            }
        }
    }
    return 0;
}

auto mdb_page_search_root(MDB_cursor* mc, MDB_val* key, int modify) -> int
{
    MDB_page* mp = mc->mc_pg[mc->mc_top];
    DKBUF;

    while (IS_BRANCH(mp))
    {
        DPRINTF(("branch page %" Yu " has %u keys", mp->mp_pgno, NUMKEYS(mp)));
        // Don't assert on branch pages in the FreeDB. We can get here
        // while in the process of rebalancing a FreeDB branch page; we must
        // let that proceed. ITS#8336
        mdb_cassert(mc, !mc->mc_dbi || NUMKEYS(mp) > 1);
        DPRINTF(("found index 0 to page %" Yu, NODEPGNO(NODEPTR(mp, 0))));

        indx_t i;
        bool descend = true;

        if ((modify & (MDB_PS_FIRST | MDB_PS_LAST)) != 0)
        {
            i = 0;
            if ((modify & MDB_PS_LAST) != 0)
            {
                i = NUMKEYS(mp) - 1;
                // if already init'd, see if we're already in right place
                if (((mc->mc_flags & C_INITIALIZED) != 0U) && (mc->mc_ki[mc->mc_top] == i))
                {
                    mc->mc_top = mc->mc_snum++;
                    mp = mc->mc_pg[mc->mc_top];
                    descend = false;
                }
            }
        }
        else
        {
            int exact;
            auto* search_node = mdb_node_search(mc, key, &exact);
            if (search_node == nullptr)
            {
                i = NUMKEYS(mp) - 1;
            }
            else
            {
                i = mc->mc_ki[mc->mc_top];
                if (exact == 0)
                {
                    mdb_cassert(mc, i > 0);
                    i--;
                }
            }
            DPRINTF(("following index %u for key [%s]", i, DKEY(key)));
        }

        if (descend)
        {
            mdb_cassert(mc, i < NUMKEYS(mp));
            const auto* node = NODEPTR(mp, i);

            MDB_page* child_page;
            if (const auto rc = mdb_page_get(mc, NODEPGNO(node), &child_page, nullptr); rc != 0)
            {
                return rc;
            }
            mp = child_page;

            mc->mc_ki[mc->mc_top] = i;
            if (const auto rc = mdb_cursor_push(mc, mp); rc != 0)
            {
                return rc;
            }
        }

        if ((modify & MDB_PS_MODIFY) != 0)
        {
            if (const auto rc = mdb_page_touch(mc); rc != 0)
            {
                return rc;
            }
            mp = mc->mc_pg[mc->mc_top];
        }
    }

    if (!IS_LEAF(mp))
    {
        DPRINTF(("internal error, index points to a %02X page!?", mp->mp_flags));
        mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
        return MDB_CORRUPTED;
    }

    DPRINTF(("found leaf page %" Yu " for key [%s]", mp->mp_pgno, key ? DKEY(key) : "null"));
    mc->mc_flags |= C_INITIALIZED;
    mc->mc_flags &= ~C_EOF;

    return MDB_SUCCESS;
}

auto mdb_page_search_lowest(MDB_cursor* mc) -> int
{
    const auto initial_page = mc->mc_pg[mc->mc_top];
    const auto node = NODEPTR(initial_page, 0);

    MDB_page* new_page = nullptr;
    if (const auto rc = mdb_page_get(mc, NODEPGNO(node), &new_page, nullptr); rc != 0)
        return rc;

    mc->mc_ki[mc->mc_top] = 0;

    if (const auto rc = mdb_cursor_push(mc, new_page); rc != 0)
        return rc;

    return mdb_page_search_root(mc, nullptr, MDB_PS_FIRST);
}

auto mdb_page_search(MDB_cursor* mc, MDB_val* key, int flags) -> int
{
    // Make sure the txn is still viable, then find the root from
    // the txn's db table and set it as the root of the cursor's stack.
    if ((mc->mc_txn->mt_flags & MDB_TXN_BLOCKED) != 0U)
    {
        DPUTS("transaction may not be used now");
        return MDB_BAD_TXN;
    }

    int rc;

    // Make sure we're using an up-to-date root
    if ((*mc->mc_dbflag & DB_STALE) != 0)
    {
        if (TXN_DBI_CHANGED(mc->mc_txn, mc->mc_dbi))
            return MDB_BAD_DBI;

        MDB_cursor mc2;
        mdb_cursor_init(&mc2, mc->mc_txn, MAIN_DBI, nullptr);
        rc = mdb_page_search(&mc2, &mc->mc_dbx->md_name, 0);
        if (rc != 0)
            return rc;

        int exact = 0;
        const auto leaf = mdb_node_search(&mc2, &mc->mc_dbx->md_name, &exact);
        if (exact == 0)
            return MDB_BAD_DBI;

        if ((leaf->mn_flags & (F_DUPDATA | F_SUBDATA)) != F_SUBDATA)
            return MDB_INCOMPATIBLE;  // not a named DB

        MDB_val data;
        rc = mdb_node_read(&mc2, leaf, &data);
        if (rc != 0)
            return rc;

        uint16_t db_flags;
        memcpy(&db_flags, (char*)data.mv_data + offsetof(MDB_db, md_flags), sizeof(uint16_t));
        // The txn may not know this DBI, or another process may
        // have dropped and recreated the DB with other flags.
        if ((mc->mc_db->md_flags & PERSISTENT_FLAGS) != db_flags)
            return MDB_INCOMPATIBLE;

        memcpy(mc->mc_db, data.mv_data, sizeof(MDB_db));

        *mc->mc_dbflag &= ~DB_STALE;
    }

    const auto root = mc->mc_db->md_root;

    if (root == P_INVALID)
    {
        // Tree is empty.
        DPUTS("tree is empty");
        return MDB_NOTFOUND;
    }

    mdb_cassert(mc, root > 1);
    if ((mc->mc_pg[0] == nullptr) || mc->mc_pg[0]->mp_pgno != root)
    {
        rc = mdb_page_get(mc, root, &mc->mc_pg[0], nullptr);
        if (rc != 0)
            return rc;
    }

    mc->mc_snum = 1;
    mc->mc_top = 0;

    DPRINTF(("db %d root page %" Yu " has flags 0x%X", DDBI(mc), root, mc->mc_pg[0]->mp_flags));

    if ((flags & MDB_PS_MODIFY) != 0)
    {
        rc = mdb_page_touch(mc);
        if (rc != 0)
            return rc;
    }

    if ((flags & MDB_PS_ROOTONLY) != 0)
        return MDB_SUCCESS;

    return mdb_page_search_root(mc, key, flags);
}

auto mdb_page_new(MDB_cursor* mc, uint32_t flags, int num, MDB_page** mp) -> int
{
    MDB_page* np;
    int rc;

    rc = mdb_page_alloc(mc, num, &np);
    if (rc != 0)
        return rc;
    DPRINTF(("allocated new mpage %" Yu ", page size %u", np->mp_pgno, mc->mc_txn->mt_env->me_psize));
    np->mp_flags = flags | P_DIRTY;
    np->mp_lower = (PAGEHDRSZ - PAGEBASE);
    np->mp_upper = mc->mc_txn->mt_env->me_psize - PAGEBASE;

    if (IS_BRANCH(np))
        mc->mc_db->md_branch_pages++;
    else if (IS_LEAF(np))
        mc->mc_db->md_leaf_pages++;
    else if (IS_OVERFLOW(np))
    {
        mc->mc_db->md_overflow_pages += num;
        np->mp_pages = num;
    }
    *mp = np;

    return 0;
}

auto mdb_leaf_size(MDB_env* env, MDB_val* key, MDB_val* data) -> size_t
{
    size_t sz;

    sz = LEAFSIZE(key, data);
    if (sz > env->me_nodemax)
    {
        // put on overflow page
        sz -= data->mv_size - sizeof(pgno_t);
    }

    return EVEN(sz + sizeof(indx_t));
}

auto mdb_branch_size(MDB_env* env, MDB_val* key) -> size_t
{
    size_t sz;

    sz = INDXSIZE(key);
    if (sz > env->me_nodemax)
    {
        // put on overflow page
        // not implemented
        // sz -= key->size - sizeof(pgno_t);
    }

    return sz + sizeof(indx_t);
}

auto mdb_node_add(MDB_cursor* mc, indx_t indx, MDB_val* key, MDB_val* data, pgno_t pgno, unsigned int flags) -> int
{
    unsigned int i;
    size_t node_size = NODESIZE;
    ssize_t room;
    indx_t ofs;
    MDB_node* node;
    MDB_page* mp = mc->mc_pg[mc->mc_top];
    MDB_page* ofp = nullptr;  // overflow page
    void* ndata;
    DKBUF;

    mdb_cassert(mc, MP_UPPER(mp) >= MP_LOWER(mp));

#if MDB_DEBUG
    DPRINTF(("add to %s %spage %" Yu " index %i, data size %" Z "u key size %" Z "u [%s]",
             IS_LEAF(mp) ? "leaf" : "branch",
             IS_SUBP(mp) ? "sub-" : "",
             mdb_dbg_pgno(mp),
             indx,
             data ? data->mv_size : 0,
             key ? key->mv_size : 0,
             key ? DKEY(key) : "null"));
#endif

    room = (ssize_t)SIZELEFT(mp) - (ssize_t)sizeof(indx_t);
    if (key != nullptr)
        node_size += key->mv_size;
    if (IS_LEAF(mp))
    {
        mdb_cassert(mc, key && data);
        if (F_ISSET(flags, F_BIGDATA))
        {
            // Data already on overflow page.
            node_size += sizeof(pgno_t);
        }
        else if (node_size + data->mv_size > mc->mc_txn->mt_env->me_nodemax)
        {
            int ovpages = OVPAGES(data->mv_size, mc->mc_txn->mt_env->me_psize);
            int rc;
            // Put data on overflow page.
            DPRINTF(("data size is %" Z "u, node would be %" Z "u, put data on overflow page",
                     data->mv_size,
                     node_size + data->mv_size));
            node_size = EVEN(node_size + sizeof(pgno_t));
            if (node_size > room)
                goto full;
            rc = mdb_page_new(mc, P_OVERFLOW, ovpages, &ofp);
            if (rc != 0)
                return rc;
            DPRINTF(("allocated overflow page %" Yu, ofp->mp_pgno));
            flags |= F_BIGDATA;
            goto update;
        }
        else
        {
            node_size += data->mv_size;
        }
    }
    node_size = EVEN(node_size);
    if (node_size > room)
        goto full;

update:
    // Move higher pointers up one slot.
    for (i = NUMKEYS(mp); i > indx; i--)
        MP_PTRS(mp)[i] = MP_PTRS(mp)[i - 1];

    // Adjust free space offsets.
    ofs = MP_UPPER(mp) - node_size;
    mdb_cassert(mc, ofs >= MP_LOWER(mp) + sizeof(indx_t));
    MP_PTRS(mp)[indx] = ofs;
    MP_UPPER(mp) = ofs;
    MP_LOWER(mp) += sizeof(indx_t);

    // Write the node data.
    node = NODEPTR(mp, indx);
    node->mn_ksize = (key == nullptr) ? 0 : key->mv_size;
    node->mn_flags = flags;
    if (IS_LEAF(mp))
        SETDSZ(node, data->mv_size);
    else
        SETPGNO(node, pgno);

    if (key != nullptr)
        memcpy(NODEKEY(node), key->mv_data, key->mv_size);

    if (IS_LEAF(mp))
    {
        ndata = NODEDATA(node);
        if (ofp == nullptr)
        {
            if (F_ISSET(flags, F_BIGDATA))
                memcpy(ndata, data->mv_data, sizeof(pgno_t));
            else if (F_ISSET(flags, MDB_RESERVE))
                data->mv_data = ndata;
            else
                memcpy(ndata, data->mv_data, data->mv_size);
        }
        else
        {
            memcpy(ndata, &ofp->mp_pgno, sizeof(pgno_t));
            ndata = METADATA(ofp);
            if (F_ISSET(flags, MDB_RESERVE))
                data->mv_data = ndata;
            else
                memcpy(ndata, data->mv_data, data->mv_size);
        }
    }

    return MDB_SUCCESS;

full:
#if MDB_DEBUG
    DPRINTF(("not enough room in page %" Yu ", got %u ptrs", mdb_dbg_pgno(mp), NUMKEYS(mp)));
#endif
    DPRINTF(("upper-lower = %u - %u = %" Z "d", MP_UPPER(mp), MP_LOWER(mp), room));
    DPRINTF(("node size = %" Z "u", node_size));
    mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
    return MDB_PAGE_FULL;
}

void mdb_node_del(MDB_cursor* mc, int ksize)
{
    MDB_page* mp = mc->mc_pg[mc->mc_top];
    indx_t indx = mc->mc_ki[mc->mc_top];
    unsigned int sz{};
    indx_t numkeys{};
    indx_t ptr{};
    MDB_node* node{nullptr};
    char* base{nullptr};
    indx_t i{};
    indx_t j{};

#if MDB_DEBUG
    DPRINTF(("delete node %u on %s page %" Yu, indx, IS_LEAF(mp) ? "leaf" : "branch", mdb_dbg_pgno(mp)));
#endif
    numkeys = NUMKEYS(mp);
    mdb_cassert(mc, indx < numkeys);

    node = NODEPTR(mp, indx);
    sz = NODESIZE + node->mn_ksize;
    if (IS_LEAF(mp))
    {
        if (F_ISSET(node->mn_flags, F_BIGDATA))
            sz += sizeof(pgno_t);
        else
            sz += NODEDSZ(node);
    }
    sz = EVEN(sz);

    ptr = MP_PTRS(mp)[indx];
    for (i = j = 0; i < numkeys; i++)
    {
        if (i != indx)
        {
            MP_PTRS(mp)[j] = MP_PTRS(mp)[i];
            if (MP_PTRS(mp)[i] < ptr)
                MP_PTRS(mp)[j] += sz;
            j++;
        }
    }

    base = (char*)mp + MP_UPPER(mp) + PAGEBASE;
    memmove(base + sz, base, ptr - MP_UPPER(mp));

    MP_LOWER(mp) -= sizeof(indx_t);
    MP_UPPER(mp) += sz;
}

void mdb_node_shrink(MDB_page* mp, indx_t indx)
{
    MDB_node* node{nullptr};
    MDB_page* sp{nullptr};
    MDB_page* xp{nullptr};
    char* base{nullptr};
    indx_t delta{};
    indx_t len{};
    indx_t ptr{};
    unsigned int nsize{};
    int i{};

    node = NODEPTR(mp, indx);
    sp = reinterpret_cast<MDB_page*>(reinterpret_cast<char*>(node->mn_data) + node->mn_ksize);
    delta = SIZELEFT(sp);
    nsize = NODEDSZ(node) - delta;

    /* Prepare to shift upward, set len = length(subpage part to shift) */
    xp = reinterpret_cast<MDB_page*>(reinterpret_cast<char*>(sp) + delta); /* destination subpage */
    for (i = NUMKEYS(sp); --i >= 0;)
        MP_PTRS(xp)[i] = MP_PTRS(sp)[i] - delta;
    len = PAGEHDRSZ;
    MP_UPPER(sp) = MP_LOWER(sp);
    COPY_PGNO(MP_PGNO(sp), mp->mp_pgno);
    SETDSZ(node, nsize);

    /* Shift <lower nodes...initial part of subpage> upward */
    base = reinterpret_cast<char*>(mp) + mp->mp_upper + PAGEBASE;
    memmove(base + delta, base, reinterpret_cast<char*>(sp) + len - base);

    ptr = mp->mp_ptrs[indx];
    for (i = NUMKEYS(mp); --i >= 0;)
    {
        if (mp->mp_ptrs[i] <= ptr)
            mp->mp_ptrs[i] += delta;
    }
    mp->mp_upper += delta;
}

auto mdb_node_move(MDB_cursor* csrc, MDB_cursor* cdst, int fromleft) -> int
{
    MDB_node* srcnode{nullptr};
    MDB_val key{};
    MDB_val data{};
    pgno_t srcpg{};
    MDB_cursor mn{};
    int rc{};
    unsigned short flags{};

    DKBUF;

    // Mark src and dst as dirty.
    rc = mdb_page_touch(csrc);
    if (rc != 0)
        return rc;
    rc = mdb_page_touch(cdst);
    if (rc != 0)
        return rc;

    {
        srcnode = NODEPTR(csrc->mc_pg[csrc->mc_top], csrc->mc_ki[csrc->mc_top]);
        mdb_cassert(csrc, !((size_t)srcnode & 1));
        srcpg = NODEPGNO(srcnode);
        flags = srcnode->mn_flags;
        if (csrc->mc_ki[csrc->mc_top] == 0 && IS_BRANCH(csrc->mc_pg[csrc->mc_top]))
        {
            unsigned int snum = csrc->mc_snum;
            MDB_node* s2;
            // must find the lowest key below src
            rc = mdb_page_search_lowest(csrc);
            if (rc != 0)
                return rc;
            s2 = NODEPTR(csrc->mc_pg[csrc->mc_top], 0);
            key.mv_size = NODEKSZ(s2);
            key.mv_data = NODEKEY(s2);
            csrc->mc_snum = snum--;
            csrc->mc_top = snum;
            csrc->mc_ki[snum] = 0;
            rc = mdb_update_key(csrc, &key);
            if (rc != 0)
                return rc;
        }
        else
        {
            key.mv_size = NODEKSZ(srcnode);
            key.mv_data = NODEKEY(srcnode);
        }
        data.mv_size = NODEDSZ(srcnode);
        data.mv_data = NODEDATA(srcnode);
    }
    mn.mc_xcursor = nullptr;
    if (IS_BRANCH(cdst->mc_pg[cdst->mc_top]) && cdst->mc_ki[cdst->mc_top] == 0)
    {
        unsigned int snum = cdst->mc_snum;
        MDB_node* s2;
        MDB_val bkey;
        // must find the lowest key below dst
        mdb_cursor_copy(cdst, &mn);
        rc = mdb_page_search_lowest(&mn);
        if (rc != 0)
            return rc;
        s2 = NODEPTR(mn.mc_pg[mn.mc_top], 0);
        bkey.mv_size = NODEKSZ(s2);
        bkey.mv_data = NODEKEY(s2);
        mn.mc_snum = snum--;
        mn.mc_top = snum;
        mn.mc_ki[snum] = 0;
        rc = mdb_update_key(&mn, &bkey);
        if (rc != 0)
            return rc;
    }

    DPRINTF(("moving %s node %u [%s] on page %" Yu " to node %u on page %" Yu,
             IS_LEAF(csrc->mc_pg[csrc->mc_top]) ? "leaf" : "branch",
             csrc->mc_ki[csrc->mc_top],
             DKEY(&key),
             csrc->mc_pg[csrc->mc_top]->mp_pgno,
             cdst->mc_ki[cdst->mc_top],
             cdst->mc_pg[cdst->mc_top]->mp_pgno));

    /* Add the node to the destination page.
     */
    rc = mdb_node_add(cdst, cdst->mc_ki[cdst->mc_top], &key, &data, srcpg, flags);
    if (rc != MDB_SUCCESS)
        return rc;

    /* Delete the node from the source page.
     */
    mdb_node_del(csrc, key.mv_size);

    {
        /* Adjust other cursors pointing to mp */
        MDB_cursor* m2;
        MDB_cursor* m3;
        MDB_dbi dbi = csrc->mc_dbi;
        MDB_page* mpd;
        MDB_page* mps;

        mps = csrc->mc_pg[csrc->mc_top];
        /* If we're adding on the left, bump others up */
        if (fromleft != 0)
        {
            mpd = cdst->mc_pg[csrc->mc_top];
            for (m2 = csrc->mc_txn->mt_cursors[dbi]; m2 != nullptr; m2 = m2->mc_next)
            {
                if ((csrc->mc_flags & C_SUB) != 0U)
                    m3 = &m2->mc_xcursor->mx_cursor;
                else
                    m3 = m2;
                if (((m3->mc_flags & C_INITIALIZED) == 0U) || m3->mc_top < csrc->mc_top)
                    continue;
                if (m3 != cdst && m3->mc_pg[csrc->mc_top] == mpd &&
                    m3->mc_ki[csrc->mc_top] >= cdst->mc_ki[csrc->mc_top])
                {
                    m3->mc_ki[csrc->mc_top]++;
                }
                if (m3 != csrc && m3->mc_pg[csrc->mc_top] == mps &&
                    m3->mc_ki[csrc->mc_top] == csrc->mc_ki[csrc->mc_top])
                {
                    m3->mc_pg[csrc->mc_top] = cdst->mc_pg[cdst->mc_top];
                    m3->mc_ki[csrc->mc_top] = cdst->mc_ki[cdst->mc_top];
                    m3->mc_ki[csrc->mc_top - 1]++;
                }
                if (IS_LEAF(mps))
                    XCURSOR_REFRESH(m3, csrc->mc_top, m3->mc_pg[csrc->mc_top]);
            }
        }
        else
        /* Adding on the right, bump others down */
        {
            for (m2 = csrc->mc_txn->mt_cursors[dbi]; m2 != nullptr; m2 = m2->mc_next)
            {
                if ((csrc->mc_flags & C_SUB) != 0U)
                    m3 = &m2->mc_xcursor->mx_cursor;
                else
                    m3 = m2;
                if (m3 == csrc)
                    continue;
                if (((m3->mc_flags & C_INITIALIZED) == 0U) || m3->mc_top < csrc->mc_top)
                    continue;
                if (m3->mc_pg[csrc->mc_top] == mps)
                {
                    if (m3->mc_ki[csrc->mc_top] == 0U)
                    {
                        m3->mc_pg[csrc->mc_top] = cdst->mc_pg[cdst->mc_top];
                        m3->mc_ki[csrc->mc_top] = cdst->mc_ki[cdst->mc_top];
                        m3->mc_ki[csrc->mc_top - 1]--;
                    }
                    else
                    {
                        m3->mc_ki[csrc->mc_top]--;
                    }
                    if (IS_LEAF(mps))
                        XCURSOR_REFRESH(m3, csrc->mc_top, m3->mc_pg[csrc->mc_top]);
                }
            }
        }
    }

    /* Update the parent separators.
     */
    if (csrc->mc_ki[csrc->mc_top] == 0)
    {
        if (csrc->mc_ki[csrc->mc_top - 1] != 0)
        {
            srcnode = NODEPTR(csrc->mc_pg[csrc->mc_top], 0);
            key.mv_size = NODEKSZ(srcnode);
            key.mv_data = NODEKEY(srcnode);
            DPRINTF(
                ("update separator for source page %" Yu " to [%s]", csrc->mc_pg[csrc->mc_top]->mp_pgno, DKEY(&key)));
            mdb_cursor_copy(csrc, &mn);
            mn.mc_snum--;
            mn.mc_top--;
            /* We want mdb_rebalance to find mn when doing fixups */
            // mdb_update_key is static in mdb_cursor.cpp
            WITH_CURSOR_TRACKING(mn, rc = mdb_update_key(&mn, &key));
            if (rc != 0)
                return rc;
        }
        if (IS_BRANCH(csrc->mc_pg[csrc->mc_top]))
        {
            MDB_val nullkey;
            indx_t ix = csrc->mc_ki[csrc->mc_top];
            nullkey.mv_size = 0;
            csrc->mc_ki[csrc->mc_top] = 0;
            rc = mdb_update_key(csrc, &nullkey);
            csrc->mc_ki[csrc->mc_top] = ix;
            mdb_cassert(csrc, rc == MDB_SUCCESS);
        }
    }

    if (cdst->mc_ki[cdst->mc_top] == 0)
    {
        if (cdst->mc_ki[cdst->mc_top - 1] != 0)
        {
            srcnode = NODEPTR(cdst->mc_pg[cdst->mc_top], 0);
            key.mv_size = NODEKSZ(srcnode);
            key.mv_data = NODEKEY(srcnode);
            DPRINTF(("update separator for destination page %" Yu " to [%s]",
                     cdst->mc_pg[cdst->mc_top]->mp_pgno,
                     DKEY(&key)));
            mdb_cursor_copy(cdst, &mn);
            mn.mc_snum--;
            mn.mc_top--;
            // We want mdb_rebalance to find mn when doing fixups
            WITH_CURSOR_TRACKING(mn, rc = mdb_update_key(&mn, &key));
            if (rc != 0)
                return rc;
        }
        if (IS_BRANCH(cdst->mc_pg[cdst->mc_top]))
        {
            MDB_val nullkey;
            indx_t ix = cdst->mc_ki[cdst->mc_top];
            nullkey.mv_size = 0;
            cdst->mc_ki[cdst->mc_top] = 0;
            // mdb_update_key is static in mdb_cursor.cpp
            rc = mdb_update_key(cdst, &nullkey);
            cdst->mc_ki[cdst->mc_top] = ix;
            mdb_cassert(cdst, rc == MDB_SUCCESS);
        }
    }

    return MDB_SUCCESS;
}

auto mdb_page_merge(MDB_cursor* csrc, MDB_cursor* cdst) -> int
{
    MDB_page* psrc{nullptr};
    MDB_page* pdst{nullptr};
    MDB_node* srcnode{nullptr};
    MDB_val key{};
    MDB_val data{};
    unsigned nkeys{};
    int rc{};
    indx_t i{};
    indx_t j{};

    psrc = csrc->mc_pg[csrc->mc_top];
    pdst = cdst->mc_pg[cdst->mc_top];

    DPRINTF(("merging page %" Yu " into %" Yu, psrc->mp_pgno, pdst->mp_pgno));

    mdb_cassert(csrc, csrc->mc_snum > 1);  // can't merge root page
    mdb_cassert(csrc, cdst->mc_snum > 1);

    // Mark dst as dirty.
    rc = mdb_page_touch(cdst);
    if (rc != 0)
        return rc;

    // get dst page again now that we've touched it.
    pdst = cdst->mc_pg[cdst->mc_top];

    // Move all nodes from src to dst.
    j = nkeys = NUMKEYS(pdst);
    for (unsigned int i = 0; i < NUMKEYS(psrc); i++, j++)
    {
        srcnode = NODEPTR(psrc, i);
        if (i == 0 && IS_BRANCH(psrc))
        {
            MDB_cursor mn;
            MDB_node* s2;
            mdb_cursor_copy(csrc, &mn);
            mn.mc_xcursor = nullptr;
            // must find the lowest key below src
            rc = mdb_page_search_lowest(&mn);
            if (rc != 0)
                return rc;
            s2 = NODEPTR(mn.mc_pg[mn.mc_top], 0);
            key.mv_size = NODEKSZ(s2);
            key.mv_data = NODEKEY(s2);
        }
        else
        {
            key.mv_size = srcnode->mn_ksize;
            key.mv_data = NODEKEY(srcnode);
        }

        data.mv_size = NODEDSZ(srcnode);
        data.mv_data = NODEDATA(srcnode);
        rc = mdb_node_add(cdst, j, &key, &data, NODEPGNO(srcnode), srcnode->mn_flags);
        if (rc != MDB_SUCCESS)
            return rc;
    }

    DPRINTF(("dst page %" Yu " now has %u keys (%.1f%% filled)",
             pdst->mp_pgno,
             NUMKEYS(pdst),
             (float)PAGEFILL(cdst->mc_txn->mt_env, pdst) / 10));

    // Unlink the src page from parent and add to free list.
    csrc->mc_top--;
    mdb_node_del(csrc, 0);
    if (csrc->mc_ki[csrc->mc_top] == 0)
    {
        key.mv_size = 0;
        // mdb_update_key is static in mdb_cursor.cpp
        rc = mdb_update_key(csrc, &key);
        if (rc != 0)
        {
            csrc->mc_top++;
            return rc;
        }
    }
    csrc->mc_top++;

    psrc = csrc->mc_pg[csrc->mc_top];
    /* If not operating on FreeDB, allow this page to be reused
     * in this txn. Otherwise just add to free list.
     */
    // mdb_page_loose is static in this file
    rc = mdb_page_loose(csrc, psrc);
    if (rc != 0)
        return rc;
    if (IS_LEAF(psrc))
        csrc->mc_db->md_leaf_pages--;
    else
        csrc->mc_db->md_branch_pages--;
    {
        /* Adjust other cursors pointing to mp */
        MDB_cursor* m2;
        MDB_cursor* m3;
        MDB_dbi dbi = csrc->mc_dbi;
        unsigned int top = csrc->mc_top;

        for (m2 = csrc->mc_txn->mt_cursors[dbi]; m2 != nullptr; m2 = m2->mc_next)
        {
            if ((csrc->mc_flags & C_SUB) != 0U)
                m3 = &m2->mc_xcursor->mx_cursor;
            else
                m3 = m2;
            if (m3 == csrc)
                continue;
            if (m3->mc_snum < csrc->mc_snum)
                continue;
            if (m3->mc_pg[top] == psrc)
            {
                m3->mc_pg[top] = pdst;
                m3->mc_ki[top] += nkeys;
                m3->mc_ki[top - 1] = cdst->mc_ki[top - 1];
            }
            else if (m3->mc_pg[top - 1] == csrc->mc_pg[top - 1] && m3->mc_ki[top - 1] > csrc->mc_ki[top - 1])
            {
                m3->mc_ki[top - 1]--;
            }
            if (IS_LEAF(psrc))
                XCURSOR_REFRESH(m3, top, m3->mc_pg[top]);
        }
    }
    {
        unsigned int snum = cdst->mc_snum;
        uint16_t depth = cdst->mc_db->md_depth;
        mdb_cursor_pop(cdst);
        rc = mdb_rebalance(cdst);
        // Did the tree height change?
        if (depth != cdst->mc_db->md_depth)
            snum += cdst->mc_db->md_depth - depth;
        cdst->mc_snum = snum;
        cdst->mc_top = snum - 1;
    }
    return rc;
}

auto mdb_rebalance(MDB_cursor* mc) -> int
{
    MDB_node* node{nullptr};
    int rc{};
    int fromleft{};
    unsigned int ptop{};
    unsigned int minkeys{};
    unsigned int thresh{};
    MDB_cursor mn{};
    indx_t oldki{};

    if (IS_BRANCH(mc->mc_pg[mc->mc_top]))
    {
        minkeys = 2;
        thresh = 1;
    }
    else
    {
        minkeys = 1;
        thresh = FILL_THRESHOLD;
    }
#if MDB_DEBUG
    DPRINTF(("rebalancing %s page %" Yu " (has %u keys, %.1f%% full)",
             IS_LEAF(mc->mc_pg[mc->mc_top]) ? "leaf" : "branch",
             mdb_dbg_pgno(mc->mc_pg[mc->mc_top]),
             NUMKEYS(mc->mc_pg[mc->mc_top]),
             (float)PAGEFILL(mc->mc_txn->mt_env, mc->mc_pg[mc->mc_top]) / 10));
#endif

    if (PAGEFILL(mc->mc_txn->mt_env, mc->mc_pg[mc->mc_top]) >= thresh && NUMKEYS(mc->mc_pg[mc->mc_top]) >= minkeys)
    {
#if MDB_DEBUG
        DPRINTF(("no need to rebalance page %" Yu ", above fill threshold", mdb_dbg_pgno(mc->mc_pg[mc->mc_top])));
#endif
        return MDB_SUCCESS;
    }

    if (mc->mc_snum < 2)
    {
        MDB_page* mp = mc->mc_pg[0];
        if (IS_SUBP(mp))
        {
            DPUTS("Can't rebalance a subpage, ignoring");
            return MDB_SUCCESS;
        }
        if (NUMKEYS(mp) == 0)
        {
            DPUTS("tree is completely empty");
            mc->mc_db->md_root = P_INVALID;
            mc->mc_db->md_depth = 0;
            mc->mc_db->md_leaf_pages = 0;
            // mdb_midl_append is from midl.h
            rc = mdb_midl_append(&mc->mc_txn->mt_free_pgs, mp->mp_pgno);
            if (rc != 0)
                return rc;
            /* Adjust cursors pointing to mp */
            mc->mc_snum = 0;
            mc->mc_top = 0;
            mc->mc_flags &= ~C_INITIALIZED;
            {
                MDB_cursor* m2;
                MDB_cursor* m3;
                MDB_dbi dbi = mc->mc_dbi;

                for (m2 = mc->mc_txn->mt_cursors[dbi]; m2 != nullptr; m2 = m2->mc_next)
                {
                    if ((mc->mc_flags & C_SUB) != 0U)
                        m3 = &m2->mc_xcursor->mx_cursor;
                    else
                        m3 = m2;
                    if (((m3->mc_flags & C_INITIALIZED) == 0U) || (m3->mc_snum < mc->mc_snum))
                        continue;
                    if (m3->mc_pg[0] == mp)
                    {
                        for (int i = 0; i < mc->mc_db->md_depth; i++)
                        {
                            m3->mc_pg[i] = m3->mc_pg[i + 1];
                            m3->mc_ki[i] = m3->mc_ki[i + 1];
                        }
                        m3->mc_snum--;
                        m3->mc_top--;
                    }
                }
            }
        }
        else if (IS_BRANCH(mp) && NUMKEYS(mp) == 1)
        {
            int i;
            DPUTS("collapsing root page!");
            // mdb_midl_append is from midl.h
            rc = mdb_midl_append(&mc->mc_txn->mt_free_pgs, mp->mp_pgno);
            if (rc != 0)
                return rc;
            mc->mc_db->md_root = NODEPGNO(NODEPTR(mp, 0));
            // mdb_page_get is in mdb_page_io.h
            rc = mdb_page_get(mc, mc->mc_db->md_root, &mc->mc_pg[0], nullptr);
            if (rc != 0)
                return rc;
            mc->mc_db->md_depth--;
            mc->mc_db->md_branch_pages--;
            mc->mc_ki[0] = mc->mc_ki[1];
            for (i = 1; i < mc->mc_db->md_depth; i++)
            {
                mc->mc_pg[i] = mc->mc_pg[i + 1];
                mc->mc_ki[i] = mc->mc_ki[i + 1];
            }
            {
                /* Adjust other cursors pointing to mp */
                MDB_cursor* m2;
                MDB_cursor* m3;
                MDB_dbi dbi = mc->mc_dbi;

                for (m2 = mc->mc_txn->mt_cursors[dbi]; m2 != nullptr; m2 = m2->mc_next)
                {
                    if ((mc->mc_flags & C_SUB) != 0U)
                        m3 = &m2->mc_xcursor->mx_cursor;
                    else
                        m3 = m2;
                    if (m3 == mc)
                        continue;
                    if ((m3->mc_flags & C_INITIALIZED) == 0U)
                        continue;
                    if (m3->mc_pg[0] == mp)
                    {
                        for (i = 0; i < mc->mc_db->md_depth; i++)
                        {
                            m3->mc_pg[i] = m3->mc_pg[i + 1];
                            m3->mc_ki[i] = m3->mc_ki[i + 1];
                        }
                        m3->mc_snum--;
                        m3->mc_top--;
                    }
                }
            }
        }
        else
            DPUTS("root page doesn't need rebalancing");
        return MDB_SUCCESS;
    }

    /* The parent (branch page) must have at least 2 pointers,
     * otherwise the tree is invalid.
     */
    ptop = mc->mc_top - 1;
    mdb_cassert(mc, NUMKEYS(mc->mc_pg[ptop]) > 1);

    /* Leaf page fill factor is below the threshold.
     * Try to move keys from left or right neighbor, or
     * merge with a neighbor page.
     */
    /* Find neighbors.
     */
    mdb_cursor_copy(mc, &mn);
    mn.mc_xcursor = nullptr;

    oldki = mc->mc_ki[mc->mc_top];
    if (mc->mc_ki[ptop] == 0)
    {
        /* We're the leftmost leaf in our parent.
         */
        DPUTS("reading right neighbor");
        mn.mc_ki[ptop]++;
        node = NODEPTR(mc->mc_pg[ptop], mn.mc_ki[ptop]);
        // mdb_page_get is in mdb_page_io.h
        rc = mdb_page_get(mc, NODEPGNO(node), &mn.mc_pg[mn.mc_top], nullptr);
        if (rc != 0)
            return rc;
        mn.mc_ki[mn.mc_top] = 0;
        mc->mc_ki[mc->mc_top] = NUMKEYS(mc->mc_pg[mc->mc_top]);
        fromleft = 0;
    }
    else
    {
        /* There is at least one neighbor to the left.
         */
        DPUTS("reading left neighbor");
        mn.mc_ki[ptop]--;
        node = NODEPTR(mc->mc_pg[ptop], mn.mc_ki[ptop]);
        // mdb_page_get is in mdb_page_io.h
        rc = mdb_page_get(mc, NODEPGNO(node), &mn.mc_pg[mn.mc_top], nullptr);
        if (rc != 0)
            return rc;
        mn.mc_ki[mn.mc_top] = NUMKEYS(mn.mc_pg[mn.mc_top]) - 1;
        mc->mc_ki[mc->mc_top] = 0;
        fromleft = 1;
    }

    DPRINTF(("found neighbor page %" Yu " (%u keys, %.1f%% full)",
             mn.mc_pg[mn.mc_top]->mp_pgno,
             NUMKEYS(mn.mc_pg[mn.mc_top]),
             (float)PAGEFILL(mc->mc_txn->mt_env, mn.mc_pg[mn.mc_top]) / 10));

    /* If the neighbor page is above threshold and has enough keys,
     * move one key from it. Otherwise we should try to merge them.
     * (A branch page must never have less than 2 keys.)
     */
    if (PAGEFILL(mc->mc_txn->mt_env, mn.mc_pg[mn.mc_top]) >= thresh && NUMKEYS(mn.mc_pg[mn.mc_top]) > minkeys)
    {
        rc = mdb_node_move(&mn, mc, fromleft);
        if (fromleft != 0)
        {
            /* if we inserted on left, bump position up */
            oldki++;
        }
    }
    else
    {
        if (fromleft == 0)
        {
            rc = mdb_page_merge(&mn, mc);
        }
        else
        {
            oldki += NUMKEYS(mn.mc_pg[mn.mc_top]);
            mn.mc_ki[mn.mc_top] += mc->mc_ki[mn.mc_top] + 1;
            // We want mdb_rebalance to find mn when doing fixups
            WITH_CURSOR_TRACKING(mn, rc = mdb_page_merge(mc, &mn));
            mdb_cursor_copy(&mn, mc);
        }
        mc->mc_flags &= ~C_EOF;
    }
    mc->mc_ki[mc->mc_top] = oldki;
    return rc;
}

// Split a page and insert a new node.
// Set #MDB_TXN_ERROR on failure.
// @param[in,out] mc Cursor pointing to the page and desired insertion index.
// The cursor will be updated to point to the actual page and index where
// the node got inserted after the split.
// newkey The key for the newly inserted node.
// newdata The data for the newly inserted node.
// newpgno The page number, if the new node is a branch node.
// nflags The #NODE_ADD_FLAGS for the new node.
// 0 on success, non-zero on failure.
auto mdb_page_split(MDB_cursor* mc, MDB_val* newkey, MDB_val* newdata, pgno_t newpgno, unsigned int nflags) -> int
{
    DKBUF;
    MDB_page* mp = mc->mc_pg[mc->mc_top];
    const indx_t newindx = mc->mc_ki[mc->mc_top];
    int nkeys = NUMKEYS(mp);

    DPRINTF(("-----> splitting %s page %" Yu " and adding [%s] at index %i/%i",
             IS_LEAF(mp) ? "leaf" : "branch",
             mp->mp_pgno,
             DKEY(newkey),
             mc->mc_ki[mc->mc_top],
             nkeys));

    // Create a right sibling.
    MDB_page* rp{nullptr};
    const int pageNewResult = mdb_page_new(mc, mp->mp_flags, 1, &rp);
    if (pageNewResult != 0)
        return pageNewResult;

    rp->mp_pad = mp->mp_pad;
    DPRINTF(("new right sibling: page %" Yu, rp->mp_pgno));

    int new_root{0};
    int ptop{};

    // Usually when splitting the root page, the cursor
    // height is 1. But when called from mdb_update_key,
    // the cursor height may be greater because it walks
    // up the stack while finding the branch slot to update.
    if (mc->mc_top < 1)
    {
        MDB_page* pp{nullptr};
        const int pageNewResult = mdb_page_new(mc, P_BRANCH, 1, &pp);
        if (pageNewResult != 0)
        {
            mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
            return pageNewResult;
        }
        // shift current top to make room for new parent
        for (int i = mc->mc_snum; i > 0; i--)
        {
            mc->mc_pg[i] = mc->mc_pg[i - 1];
            mc->mc_ki[i] = mc->mc_ki[i - 1];
        }
        mc->mc_pg[0] = pp;
        mc->mc_ki[0] = 0;
        mc->mc_db->md_root = pp->mp_pgno;
        DPRINTF(("root split! new root = %" Yu, pp->mp_pgno));
        new_root = mc->mc_db->md_depth++;

        // Add left (implicit) pointer.
        const int nodeAddResult = mdb_node_add(mc, 0, nullptr, nullptr, mp->mp_pgno, 0);
        if (nodeAddResult != MDB_SUCCESS)
        {
            // undo the pre-push
            mc->mc_pg[0] = mc->mc_pg[1];
            mc->mc_ki[0] = mc->mc_ki[1];
            mc->mc_db->md_root = mp->mp_pgno;
            mc->mc_db->md_depth--;
            mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
            return nodeAddResult;
        }
        mc->mc_snum++;
        mc->mc_top++;
        ptop = 0;
    }
    else
    {
        ptop = mc->mc_top - 1;
        DPRINTF(("parent branch page is %" Yu, mc->mc_pg[ptop]->mp_pgno));
    }

    MDB_cursor mn{};
    mdb_cursor_copy(mc, &mn);
    mn.mc_xcursor = nullptr;
    mn.mc_pg[mn.mc_top] = rp;
    mn.mc_ki[ptop] = mc->mc_ki[ptop] + 1;

    MDB_page* copy{nullptr};
    int split_indx{};
    MDB_val sepkey{};
    MDB_node* node{nullptr};
    MDB_env* env = mc->mc_txn->mt_env;

    if ((nflags & MDB_APPEND) != 0U)
    {
        mn.mc_ki[mn.mc_top] = 0;
        sepkey = *newkey;
        split_indx = newindx;
        nkeys = 0;
    }
    else
    {
        split_indx = (nkeys + 1) / 2;

        {
            // Maximum free space in an empty page
            const int pmax = env->me_psize - PAGEHDRSZ;

            // Threshold number of keys considered "small"
            const int keythresh = env->me_psize >> 7;

            const int nsize = EVEN(IS_LEAF(mp) ? mdb_leaf_size(env, newkey, newdata) : mdb_branch_size(env, newkey));

            // grab a page to hold a temporary copy
            copy = mdb_page_malloc(mc->mc_txn, 1);
            if (copy == nullptr)
            {
                if (copy != nullptr)  // tmp page
                    mdb_page_free(env, copy);
                mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
                return ENOMEM;
            }

            copy->mp_pgno = mp->mp_pgno;
            copy->mp_flags = mp->mp_flags;
            copy->mp_lower = (PAGEHDRSZ - PAGEBASE);
            copy->mp_upper = env->me_psize - PAGEBASE;

            // prepare to insert
            for (int i = 0, j = 0; i < nkeys; i++)
            {
                if (i == newindx)
                    copy->mp_ptrs[j++] = 0;
                copy->mp_ptrs[j++] = mp->mp_ptrs[i];
            }

            // When items are relatively large the split point needs
            // to be checked, because being off-by-one will make the
            // difference between success or failure in mdb_node_add.

            // It's also relevant if a page happens to be laid out
            // such that one half of its nodes are all "small" and
            // the other half of its nodes are "large." If the new
            // item is also "large" and falls on the half with
            // "large" nodes, it also may not fit.

            // As a final tweak, if the new item goes on the last
            // spot on the page (and thus, onto the new page), bias
            // the split so the new page is emptier than the old page.
            // This yields better packing during sequential inserts.
            if (nkeys < keythresh || nsize > pmax / 16 || newindx >= nkeys)
            {
                // Find split point
                int i;
                int j;
                int k;
                if (newindx <= split_indx || newindx >= nkeys)
                {
                    i = 0;
                    j = 1;
                    k = (newindx >= nkeys) ? nkeys : split_indx + 1 + IS_LEAF(mp);
                }
                else
                {
                    i = nkeys;
                    j = -1;
                    k = split_indx - 1;
                }

                int psize = 0;
                for (; i != k; i += j)
                {
                    if (i == newindx)
                    {
                        psize += nsize;
                        node = nullptr;
                    }
                    else
                    {
                        node = reinterpret_cast<MDB_node*>(reinterpret_cast<char*>(mp) + copy->mp_ptrs[i] + PAGEBASE);
                        psize += NODESIZE + NODEKSZ(node) + sizeof(indx_t);
                        if (IS_LEAF(mp))
                        {
                            if (F_ISSET(node->mn_flags, F_BIGDATA))
                                psize += sizeof(pgno_t);
                            else
                                psize += NODEDSZ(node);
                        }
                        psize = EVEN(psize);
                    }
                    if (psize > pmax || i == k - j)
                    {
                        split_indx = i + static_cast<int>(j < 0);
                        break;
                    }
                }
            }
            if (split_indx == newindx)
            {
                sepkey.mv_size = newkey->mv_size;
                sepkey.mv_data = newkey->mv_data;
            }
            else
            {
                node = reinterpret_cast<MDB_node*>(reinterpret_cast<char*>(mp) + copy->mp_ptrs[split_indx] + PAGEBASE);
                sepkey.mv_size = node->mn_ksize;
                sepkey.mv_data = NODEKEY(node);
            }
        }
    }

    DPRINTF(("separator is %d [%s]", split_indx, DKEY(&sepkey)));

    int did_split{0};
    pgno_t pgno{0};

    // Copy separator key to the parent.
    if (SIZELEFT(mn.mc_pg[ptop]) < mdb_branch_size(env, &sepkey))
    {
        int snum = mc->mc_snum;
        mn.mc_snum--;
        mn.mc_top--;
        did_split = 1;
        // We want other splits to find mn when doing fixups
        int pageSplitResult = MDB_SUCCESS;
        WITH_CURSOR_TRACKING(mn, pageSplitResult = mdb_page_split(&mn, &sepkey, nullptr, rp->mp_pgno, 0));
        if (pageSplitResult != 0)
        {
            if (copy != nullptr)  // tmp page
                mdb_page_free(env, copy);
            mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
            return pageSplitResult;
        }

        // root split?
        if (mc->mc_snum > snum)
            ptop++;

        // Right page might now have changed parent.
        // Check if left page also changed parent.
        if (mn.mc_pg[ptop] != mc->mc_pg[ptop] && mc->mc_ki[ptop] >= NUMKEYS(mc->mc_pg[ptop]))
        {
            for (int i = 0; i < ptop; i++)
            {
                mc->mc_pg[i] = mn.mc_pg[i];
                mc->mc_ki[i] = mn.mc_ki[i];
            }
            mc->mc_pg[ptop] = mn.mc_pg[ptop];
            if (mn.mc_ki[ptop] != 0U)
            {
                mc->mc_ki[ptop] = mn.mc_ki[ptop] - 1;
            }
            else
            {
                // find right page's left sibling
                mc->mc_ki[ptop] = mn.mc_ki[ptop];
                int cursorSiblingResult = mdb_cursor_sibling(mc, 0);
                if (cursorSiblingResult != MDB_SUCCESS)
                {
                    if (cursorSiblingResult == MDB_NOTFOUND)  // improper mdb_cursor_sibling() result
                        cursorSiblingResult = MDB_PROBLEM;
                    if (copy != nullptr)  // tmp page
                        mdb_page_free(env, copy);
                    mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
                    return cursorSiblingResult;
                }
            }
        }
    }
    else
    {
        mn.mc_top--;
        int nodeAddResult = mdb_node_add(&mn, mn.mc_ki[ptop], &sepkey, nullptr, rp->mp_pgno, 0);
        mn.mc_top++;
        if (nodeAddResult != MDB_SUCCESS)
        {
            if (nodeAddResult == MDB_NOTFOUND)  // improper mdb_cursor_sibling() result
                nodeAddResult = MDB_PROBLEM;
            if (copy != nullptr)  // tmp page
                mdb_page_free(env, copy);
            mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
            return nodeAddResult;
        }
    }

    MDB_val rkey{};
    MDB_val xdata{};
    MDB_val* rdata = &xdata;

    if ((nflags & MDB_APPEND) != 0U)
    {
        mc->mc_pg[mc->mc_top] = rp;
        mc->mc_ki[mc->mc_top] = 0;
        const int nodeAddResult = mdb_node_add(mc, 0, newkey, newdata, newpgno, nflags);
        if (nodeAddResult != 0)
        {
            if (copy != nullptr)  // tmp page
                mdb_page_free(env, copy);
            mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
            return nodeAddResult;
        }
        for (int i = 0; i < mc->mc_top; i++)
            mc->mc_ki[i] = mn.mc_ki[i];
    }
    else
    {
        // Move nodes
        mc->mc_pg[mc->mc_top] = rp;
        int i = split_indx;
        int j = 0;
        do
        {
            unsigned int flags{};
            if (i == newindx)
            {
                rkey.mv_data = newkey->mv_data;
                rkey.mv_size = newkey->mv_size;
                if (IS_LEAF(mp))
                {
                    rdata = newdata;
                }
                else
                    pgno = newpgno;
                flags = nflags;
                // Update index for the new key.
                mc->mc_ki[mc->mc_top] = j;
            }
            else
            {
                node = reinterpret_cast<MDB_node*>(reinterpret_cast<char*>(mp) + copy->mp_ptrs[i] + PAGEBASE);
                rkey.mv_data = NODEKEY(node);
                rkey.mv_size = node->mn_ksize;
                if (IS_LEAF(mp))
                {
                    xdata.mv_data = NODEDATA(node);
                    xdata.mv_size = NODEDSZ(node);
                    rdata = &xdata;
                }
                else
                    pgno = NODEPGNO(node);
                flags = node->mn_flags;
            }

            if (!IS_LEAF(mp) && j == 0)
            {
                // First branch index doesn't need key data.
                rkey.mv_size = 0;
            }

            const int nodeAddResult = mdb_node_add(mc, j, &rkey, rdata, pgno, flags);
            if (nodeAddResult != 0)
            {
                if (copy != nullptr)  // tmp page
                    mdb_page_free(env, copy);
                mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
                return nodeAddResult;
            }
            if (i == nkeys)
            {
                i = 0;
                j = 0;
                mc->mc_pg[mc->mc_top] = copy;
            }
            else
            {
                i++;
                j++;
            }
        } while (i != split_indx);

        nkeys = NUMKEYS(copy);
        for (int i = 0; i < nkeys; i++)
            mp->mp_ptrs[i] = copy->mp_ptrs[i];
        mp->mp_lower = copy->mp_lower;
        mp->mp_upper = copy->mp_upper;
        memcpy(NODEPTR(mp, nkeys - 1), NODEPTR(copy, nkeys - 1), env->me_psize - copy->mp_upper - PAGEBASE);

        // reset back to original page
        if (newindx < split_indx)
        {
            mc->mc_pg[mc->mc_top] = mp;
        }
        else
        {
            mc->mc_pg[mc->mc_top] = rp;
            mc->mc_ki[ptop]++;
            // Make sure mc_ki is still valid.
            if (mn.mc_pg[ptop] != mc->mc_pg[ptop] && mc->mc_ki[ptop] >= NUMKEYS(mc->mc_pg[ptop]))
            {
                for (i = 0; i <= ptop; i++)
                {
                    mc->mc_pg[i] = mn.mc_pg[i];
                    mc->mc_ki[i] = mn.mc_ki[i];
                }
            }
        }
        if ((nflags & MDB_RESERVE) != 0U)
        {
            node = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
            if ((node->mn_flags & F_BIGDATA) == 0)
                newdata->mv_data = NODEDATA(node);
        }
    }

    if (newindx >= split_indx)
    {
        mc->mc_pg[mc->mc_top] = rp;
        mc->mc_ki[ptop]++;
        // Make sure mc_ki is still valid.
        if (mn.mc_pg[ptop] != mc->mc_pg[ptop] && mc->mc_ki[ptop] >= NUMKEYS(mc->mc_pg[ptop]))
        {
            for (int i = 0; i <= ptop; i++)
            {
                mc->mc_pg[i] = mn.mc_pg[i];
                mc->mc_ki[i] = mn.mc_ki[i];
            }
        }
    }

    {
        // Adjust other cursors pointing to mp
        MDB_cursor* m2;
        MDB_cursor* m3;
        MDB_dbi dbi = mc->mc_dbi;
        nkeys = NUMKEYS(mp);

        for (m2 = mc->mc_txn->mt_cursors[dbi]; m2 != nullptr; m2 = m2->mc_next)
        {
            if ((mc->mc_flags & C_SUB) != 0U)
                m3 = &m2->mc_xcursor->mx_cursor;
            else
                m3 = m2;
            if (m3 == mc)
                continue;
            if ((m2->mc_flags & m3->mc_flags & C_INITIALIZED) == 0U)
                continue;
            if (new_root != 0)
            {
                int k;
                // sub cursors may be on different DB
                if (m3->mc_pg[0] != mp)
                    continue;
                // root split
                for (k = new_root; k >= 0; k--)
                {
                    m3->mc_ki[k + 1] = m3->mc_ki[k];
                    m3->mc_pg[k + 1] = m3->mc_pg[k];
                }
                if (m3->mc_ki[0] >= nkeys)
                {
                    m3->mc_ki[0] = 1;
                }
                else
                {
                    m3->mc_ki[0] = 0;
                }
                m3->mc_pg[0] = mc->mc_pg[0];
                m3->mc_snum++;
                m3->mc_top++;
            }
            if (m3->mc_top >= mc->mc_top && m3->mc_pg[mc->mc_top] == mp)
            {
                if (m3->mc_ki[mc->mc_top] >= newindx && ((nflags & MDB_SPLIT_REPLACE) == 0U))
                    m3->mc_ki[mc->mc_top]++;
                if (m3->mc_ki[mc->mc_top] >= nkeys)
                {
                    m3->mc_pg[mc->mc_top] = rp;
                    m3->mc_ki[mc->mc_top] -= nkeys;
                    for (int i = 0; i < mc->mc_top; i++)
                    {
                        m3->mc_ki[i] = mn.mc_ki[i];
                        m3->mc_pg[i] = mn.mc_pg[i];
                    }
                }
            }
            else if ((did_split == 0) && (m3->mc_top >= ptop) && m3->mc_pg[ptop] == mc->mc_pg[ptop] &&
                     m3->mc_ki[ptop] >= mc->mc_ki[ptop])
            {
                m3->mc_ki[ptop]++;
            }
            if (IS_LEAF(mp))
                XCURSOR_REFRESH(m3, mc->mc_top, m3->mc_pg[mc->mc_top]);
        }
    }

    DPRINTF(("mp left: %d, rp left: %d", SIZELEFT(mp), SIZELEFT(rp)));
    if (copy != nullptr)  // tmp page
        mdb_page_free(env, copy);
    return MDB_SUCCESS;
}
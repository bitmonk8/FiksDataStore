#include "mdb_txn.h"

#include "mdb_btree.h"
#include "mdb_cursor.h"
#include "mdb_db.h"
#include "mdb_debug.h"
#include "mdb_env.h"
#include "mdb_lock.h"

#include <utility>

// Nested transaction
struct MDB_ntxn
{
    MDB_txn mnt_txn;          //< the transaction
    MDB_pgstate mnt_pgstate;  //< parent transaction's saved freestate
};

// Common code for #mdb_txn_begin() and #mdb_txn_renew().
// [in] txn the transaction handle to initialize
//  0 on success, non-zero on failure.
auto mdb_txn_renew0(MDB_txn* txn) -> int
{
    MDB_env* env = txn->mt_env;
    MDB_txninfo* ti = env->me_txns;
    MDB_meta* meta{nullptr};
    unsigned int i{};
    unsigned int nr{};
    unsigned int flags = txn->mt_flags;
    int rc{};
    int new_notls{0};

    flags &= MDB_TXN_RDONLY;
    if (flags != 0)
    {
        if (ti == nullptr)
        {
            meta = mdb_env_pick_meta(env);
            txn->mt_txnid = meta->mm_txnid;
            txn->mt_u.reader = nullptr;
        }
        else
        {
            auto* r = (MDB_reader*)(((env->me_flags & MDB_NOTLS) != 0U) ? txn->mt_u.reader
                                                                        : pthread_getspecific(env->me_txkey));
            if (r != nullptr)
            {
                if ((r->mr_pid != env->me_pid) || (r->mr_txnid != -1))
                {
                    return MDB_BAD_RSLOT;
                }
            }
            else
            {
                MDB_PID_T pid = env->me_pid;
                MDB_THR_T tid = pthread_self();
                mdb_mutexref_t rmutex = env->me_rmutex;

                if (env->me_live_reader == 0)
                {
                    rc = mdb_reader_pid(env, Pidset, pid);
                    if (rc != 0)
                        return rc;
                    env->me_live_reader = 1;
                }

                LOCK_MUTEX(rc, env, rmutex);
                if (rc != 0)
                    return rc;
                nr = ti->mti_numreaders;
                for (i = 0; i < nr; i++)
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
                if (new_notls == 0)
                {
                    rc = pthread_setspecific(env->me_txkey, r);
                    if (rc != 0)
                    {
                        r->mr_pid = 0;
                        return rc;
                    }
                }
            }
            do /* LY: Retry on a race, ITS#7970. */
                r->mr_txnid = ti->mti_txnid;
            while (r->mr_txnid != ti->mti_txnid);
            if ((r->mr_txnid == 0U) && ((env->me_flags & MDB_RDONLY) != 0U))
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
        if (ti != nullptr)
        {
            LOCK_MUTEX(rc, env, env->me_wmutex);
            if (rc != 0)
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
        txn->mt_child = nullptr;
        txn->mt_loose_pgs = nullptr;
        txn->mt_loose_count = 0;
        txn->mt_dirty_room = MDB_IDL_UM_MAX;
        txn->mt_u.dirty_list = env->me_dirty_list;
        txn->mt_u.dirty_list[0].mid = 0;
        txn->mt_free_pgs = env->me_free_pgs;
        txn->mt_free_pgs[0] = 0;
        txn->mt_spill_pgs = nullptr;
        env->me_txn = txn;
        memcpy(txn->mt_dbiseqs, env->me_dbiseqs, env->me_maxdbs * sizeof(unsigned int));
    }

    // Copy the DB info and flags
    memcpy(txn->mt_dbs, meta->mm_dbs, CORE_DBS * sizeof(MDB_db));

    // Moved to here to avoid a data race in read TXNs
    txn->mt_next_pgno = meta->mm_last_pg + 1;
    txn->mt_flags = flags;

    // Setup db info
    txn->mt_numdbs = env->me_numdbs;
    for (i = CORE_DBS; i < txn->mt_numdbs; i++)
    {
        uint16_t x = env->me_dbflags[i];
        txn->mt_dbs[i].md_flags = x & PERSISTENT_FLAGS;
        txn->mt_dbflags[i] = ((x & MDB_VALID) != 0) ? DB_VALID | DB_USRVALID | DB_STALE : 0;
    }
    txn->mt_dbflags[MAIN_DBI] = DB_VALID | DB_USRVALID;
    txn->mt_dbflags[FREE_DBI] = DB_VALID;

    if ((env->me_flags & MDB_FATAL_ERROR) != 0U)
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

auto mdb_txn_renew(MDB_txn* txn) -> int
{
    int rc;

    if ((txn == nullptr) || !F_ISSET(txn->mt_flags, MDB_TXN_RDONLY | MDB_TXN_FINISHED))
        return EINVAL;

    rc = mdb_txn_renew0(txn);
    if (rc == MDB_SUCCESS)
    {
        DPRINTF(("renew txn %" Yu "%c %p on mdbenv %p, root page %" Yu,
                 txn->mt_txnid,
                 (txn->mt_flags & MDB_TXN_RDONLY) ? 'r' : 'w',
                 (void*)txn,
                 (void*)txn->mt_env,
                 txn->mt_dbs[MAIN_DBI].md_root));
    }
    return rc;
}

// Back up parent txn's cursors, then grab the originals for tracking
static auto mdb_cursor_shadow(MDB_txn* src, MDB_txn* dst) -> int
{
    int i{};

    for (i = src->mt_numdbs; --i >= 0;)
    {
        MDB_cursor* current_cursor = src->mt_cursors[i];
        if (current_cursor != nullptr)
        {
            const size_t base_cursor_size = sizeof(MDB_cursor);
            // No xcursor support - simplified cursor size
            const size_t total_cursor_size = base_cursor_size;

            for (; current_cursor != nullptr;)
            {
                auto* const backup_cursor = (MDB_cursor*)malloc(total_cursor_size);
                if (backup_cursor == nullptr)
                    return ENOMEM;

                *backup_cursor = *current_cursor;
                current_cursor->mc_backup = backup_cursor;
                current_cursor->mc_db = &dst->mt_dbs[i];
                // Kill pointers into src to reduce abuse: The
                // user may not use mc until dst ends. But we need a valid
                // txn pointer here for cursor fixups to keep working.
                current_cursor->mc_txn = dst;
                current_cursor->mc_dbflag = &dst->mt_dbflags[i];

                // No xcursor support - removed

                current_cursor->mc_next = dst->mt_cursors[i];
                dst->mt_cursors[i] = current_cursor;

                // Move to next cursor in chain
                current_cursor = backup_cursor->mc_next;
            }
        }
    }
    return MDB_SUCCESS;
}

auto mdb_txn_begin(MDB_env* env, MDB_txn* parent, unsigned int flags, MDB_txn** ret) -> int
{
    MDB_txn* txn{nullptr};
    MDB_ntxn* ntxn{nullptr};
    int rc{};
    int size{};
    int tsize{};

    flags &= MDB_TXN_BEGIN_FLAGS;
    flags |= env->me_flags & MDB_WRITEMAP;

    if ((env->me_flags & MDB_RDONLY & ~flags) != 0U) /* write txn in RDONLY env */
        return EACCES;

    if (parent != nullptr)
    {
        // Nested transactions: Max 1 child, write txns only, no writemap
        flags |= parent->mt_flags;
        if ((flags & (MDB_RDONLY | MDB_WRITEMAP | MDB_TXN_BLOCKED)) != 0U)
        {
            return ((parent->mt_flags & MDB_TXN_RDONLY) != 0U) ? EINVAL : MDB_BAD_TXN;
        }
        // Child txns save MDB_pgstate and use own copy of cursors
        size = env->me_maxdbs * (sizeof(MDB_db) + sizeof(MDB_cursor*) + 1);
        size += tsize = sizeof(MDB_ntxn);
    }
    else if ((flags & MDB_RDONLY) != 0U)
    {
        size = env->me_maxdbs * (sizeof(MDB_db) + 1);
        size += tsize = sizeof(MDB_txn);
    }
    else
    {
        // Reuse preallocated write txn. However, do not touch it until
        // mdb_txn_renew0() succeeds, since it currently may be active.
        txn = env->me_txn0;
        goto renew;
    }
    txn = (MDB_txn*)calloc(1, size);
    if (txn == nullptr)
    {
        DPRINTF(("calloc: %s", strerror(errno)));
        return ENOMEM;
    }
    txn->mt_dbxs = env->me_dbxs;  // static
    txn->mt_dbs = (MDB_db*)((char*)txn + tsize);
    txn->mt_dbflags = (unsigned char*)txn + size - env->me_maxdbs;
    txn->mt_flags = flags;
    txn->mt_env = env;

    if (parent != nullptr)
    {
        unsigned int i;
        txn->mt_cursors = (MDB_cursor**)(txn->mt_dbs + env->me_maxdbs);
        txn->mt_dbiseqs = parent->mt_dbiseqs;
        txn->mt_u.dirty_list = (MDB_ID2L)malloc(sizeof(MDB_ID2) * MDB_IDL_UM_SIZE);
        txn->mt_free_pgs = mdb_midl_alloc(MDB_IDL_UM_MAX);
        if ((txn->mt_u.dirty_list == nullptr) || (txn->mt_free_pgs == nullptr))
        {
            free(txn->mt_u.dirty_list);
            free(txn);
            return ENOMEM;
        }
        txn->mt_txnid = parent->mt_txnid;
        txn->mt_dirty_room = parent->mt_dirty_room;
        txn->mt_u.dirty_list[0].mid = 0;
        txn->mt_spill_pgs = nullptr;
        txn->mt_next_pgno = parent->mt_next_pgno;
        parent->mt_flags |= MDB_TXN_HAS_CHILD;
        parent->mt_child = txn;
        txn->mt_parent = parent;
        txn->mt_numdbs = parent->mt_numdbs;
        memcpy(txn->mt_dbs, parent->mt_dbs, txn->mt_numdbs * sizeof(MDB_db));
        // Copy parent's mt_dbflags, but clear DB_NEW
        for (i = 0; i < txn->mt_numdbs; i++)
            txn->mt_dbflags[i] = parent->mt_dbflags[i] & ~DB_NEW;
        rc = 0;
        ntxn = (MDB_ntxn*)txn;
        ntxn->mnt_pgstate = env->me_pgstate;  // save parent me_pghead & co
        if (env->me_pghead != nullptr)
        {
            size = MDB_IDL_SIZEOF(env->me_pghead);
            env->me_pghead = mdb_midl_alloc(env->me_pghead[0]);
            if (env->me_pghead != nullptr)
                memcpy(env->me_pghead, ntxn->mnt_pgstate.mf_pghead, size);
            else
                rc = ENOMEM;
        }
        if (rc == 0)
            rc = mdb_cursor_shadow(parent, txn);
        if (rc != 0)
            mdb_txn_end(txn, MDB_END_FAIL_BEGINCHILD);
    }
    else
    { /* MDB_RDONLY */
        txn->mt_dbiseqs = env->me_dbiseqs;
    renew:
        rc = mdb_txn_renew0(txn);
    }
    if (rc != 0)
    {
        if (txn != env->me_txn0)
        {
            free(txn);
        }
    }
    else
    {
        txn->mt_flags |= flags; /* could not change txn=me_txn0 earlier */
        *ret = txn;
        DPRINTF(("begin txn %" Yu "%c %p on mdbenv %p, root page %" Yu,
                 txn->mt_txnid,
                 (flags & MDB_RDONLY) ? 'r' : 'w',
                 (void*)txn,
                 (void*)env,
                 txn->mt_dbs[MAIN_DBI].md_root));
    }
    MDB_TRACE(("%p, %p, %u = %p", env, parent, flags, txn));

    return rc;
}

auto mdb_txn_env(MDB_txn* txn) -> MDB_env*
{
    if (txn == nullptr)
        return nullptr;
    return txn->mt_env;
}

auto mdb_txn_id(MDB_txn* txn) -> mdb_size_t
{
    if (txn == nullptr)
        return 0;
    return txn->mt_txnid;
}

// Export or close DBI handles opened in this txn.
static void mdb_dbis_update(MDB_txn* txn, int keep)
{
    int i;
    MDB_dbi n = txn->mt_numdbs;
    MDB_env* env = txn->mt_env;
    unsigned char* tdbflags = txn->mt_dbflags;

    for (i = n; --i >= CORE_DBS;)
    {
        if ((tdbflags[i] & DB_NEW) != 0)
        {
            if (keep != 0)
            {
                env->me_dbflags[i] = txn->mt_dbs[i].md_flags | MDB_VALID;
            }
            else
            {
                char* ptr = (char*)env->me_dbxs[i].md_name.mv_data;
                if (ptr != nullptr)
                {
                    env->me_dbxs[i].md_name.mv_data = nullptr;
                    env->me_dbxs[i].md_name.mv_size = 0;
                    env->me_dbflags[i] = 0;
                    env->me_dbiseqs[i]++;
                    free(ptr);
                }
            }
        }
    }
    if ((keep != 0) && env->me_numdbs < n)
        env->me_numdbs = n;
}

// Close this write txn's cursors, give parent txn's cursors back to parent.
// [in] txn the transaction handle.
// [in] merge true to keep changes to parent cursors, false to revert.
//  0 on success, non-zero on failure.
static void mdb_cursors_close(MDB_txn* txn, unsigned merge)
{
    MDB_cursor** cursors = txn->mt_cursors;
    MDB_cursor* mc;
    MDB_cursor* next;
    MDB_cursor* bk;
    int i;

    for (i = txn->mt_numdbs; --i >= 0;)
    {
        for (mc = cursors[i]; mc != nullptr; mc = next)
        {
            next = mc->mc_next;
            bk = mc->mc_backup;
            if (bk != nullptr)
            {
                if (merge != 0U)
                {
                    // Commit changes to parent txn
                    mc->mc_next = bk->mc_next;
                    mc->mc_backup = bk->mc_backup;
                    mc->mc_txn = bk->mc_txn;
                    mc->mc_db = bk->mc_db;
                    mc->mc_dbflag = bk->mc_dbflag;
                    // No xcursor support - removed
                }
                else
                {
                    // Abort nested txn
                    *mc = *bk;
                    // No xcursor support - removed
                }
                mc = bk;
            }
            // Only malloced cursors are permanently tracked.
            free(mc);
        }
        cursors[i] = nullptr;
    }
}

// Return all dirty pages to dpage list
static void mdb_dlist_free(MDB_txn* txn)
{
    MDB_env* env = txn->mt_env;
    MDB_ID2L dl = txn->mt_u.dirty_list;
    unsigned i{};
    unsigned n = dl[0].mid;

    for (i = 1; i <= n; i++)
    {
        mdb_dpage_free(env, (MDB_page*)dl[i].mptr);
    }
    dl[0].mid = 0;
}

#define MDB_END_NAMES {"committed", "empty-commit", "abort", "reset", "reset-tmp", "fail-begin", "fail-beginchild"}

// End a transaction, except successful commit of a nested transaction.
// May be called twice for readonly txns: First reset it, then abort.
// [in] txn the transaction handle to end
// [in] mode why and how to end the transaction
void mdb_txn_end(MDB_txn* txn, unsigned mode)
{
    MDB_env* env = txn->mt_env;
#if MDB_DEBUG
    static const char* const names[] = MDB_END_NAMES;
#endif

    // Export or close DBI handles opened in this txn
    mdb_dbis_update(txn, mode & MDB_END_UPDATE);

    DPRINTF(("%s txn %" Yu "%c %p on mdbenv %p, root page %" Yu,
             names[mode & MDB_END_OPMASK],
             txn->mt_txnid,
             (txn->mt_flags & MDB_TXN_RDONLY) ? 'r' : 'w',
             (void*)txn,
             (void*)env,
             txn->mt_dbs[MAIN_DBI].md_root));

    if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
    {
        if (txn->mt_u.reader != nullptr)
        {
            txn->mt_u.reader->mr_txnid = (txnid_t)-1;
            if ((env->me_flags & MDB_NOTLS) == 0U)
            {
                txn->mt_u.reader = nullptr; /* txn does not own reader */
            }
            else if ((mode & MDB_END_SLOT) != 0U)
            {
                txn->mt_u.reader->mr_pid = 0;
                txn->mt_u.reader = nullptr;
            } /* else txn owns the slot until it does MDB_END_SLOT */
        }
        txn->mt_numdbs = 0; /* prevent further DBI activity */
        txn->mt_flags |= MDB_TXN_FINISHED;
    }
    else if (!F_ISSET(txn->mt_flags, MDB_TXN_FINISHED))
    {
        pgno_t* pghead = env->me_pghead;

        if ((mode & MDB_END_UPDATE) == 0U)  // !(already closed cursors)
            mdb_cursors_close(txn, 0);
        if ((env->me_flags & MDB_WRITEMAP) == 0U)
        {
            mdb_dlist_free(txn);
        }

        txn->mt_numdbs = 0;
        txn->mt_flags = MDB_TXN_FINISHED;

        if (txn->mt_parent == nullptr)
        {
            mdb_midl_shrink(&txn->mt_free_pgs);
            env->me_free_pgs = txn->mt_free_pgs;
            // me_pgstate:
            env->me_pghead = nullptr;
            env->me_pglast = 0;

            env->me_txn = nullptr;
            mode = 0;  // txn == env->me_txn0, do not free() it

            /* The writer mutex was locked in mdb_txn_begin. */
            if (env->me_txns != nullptr)
                UNLOCK_MUTEX(env->me_wmutex);
        }
        else
        {
            txn->mt_parent->mt_child = nullptr;
            txn->mt_parent->mt_flags &= ~MDB_TXN_HAS_CHILD;
            env->me_pgstate = ((MDB_ntxn*)txn)->mnt_pgstate;
            mdb_midl_free(txn->mt_free_pgs);
            free(txn->mt_u.dirty_list);
        }
        mdb_midl_free(txn->mt_spill_pgs);

        mdb_midl_free(pghead);
    }
    if ((mode & MDB_END_FREE) != 0U)
        free(txn);
}

void mdb_txn_reset(MDB_txn* txn)
{
    if (txn == nullptr)
        return;

    // This call is only valid for read-only txns
    if ((txn->mt_flags & MDB_TXN_RDONLY) == 0U)
        return;

    mdb_txn_end(txn, MDB_END_RESET);
}

void mdb_txn_abort_impl(MDB_txn* txn)
{
    if (txn == nullptr)
        return;

    if (txn->mt_child != nullptr)
        mdb_txn_abort_impl(txn->mt_child);

    mdb_txn_end(txn,
                static_cast<unsigned>(MDB_END_ABORT) | static_cast<unsigned>(MDB_END_SLOT) |
                    static_cast<unsigned>(MDB_END_FREE));
}

void mdb_txn_abort(MDB_txn* txn)
{
    MDB_TRACE(("%p", txn));
    mdb_txn_abort_impl(txn);
}

// Save the freelist as of this transaction to the freeDB.
// This changes the freelist. Keep trying until it stabilizes.
auto mdb_freelist_save(MDB_txn* txn) -> int
{
    // env->me_pghead[] can grow and shrink during this call.
    // env->me_pglast and txn->mt_free_pgs[] can only grow.
    // Page numbers cannot disappear from txn->mt_free_pgs[].
    MDB_cursor mc{};
    MDB_env* env = txn->mt_env;
    int rc{};
    int maxfree_1pg = env->me_maxfree_1pg;
    int more{1};
    txnid_t pglast{0};
    txnid_t head_id{0};
    pgno_t freecnt{0};
    pgno_t* free_pgs{nullptr};
    pgno_t* mop{nullptr};
    ssize_t head_room{0};
    ssize_t total_room{0};
    ssize_t mop_len{};
    ssize_t clean_limit{};

    mdb_cursor_init(&mc, txn, FREE_DBI, nullptr);

    if (env->me_pghead != nullptr)
    {
        // Make sure first page of freeDB is touched and on freelist
        rc = mdb_page_search(&mc, nullptr, MDB_PS_FIRST | MDB_PS_MODIFY);
        if ((rc != 0) && rc != MDB_NOTFOUND)
            return rc;
    }

    if ((env->me_pghead == nullptr) && (txn->mt_loose_pgs != nullptr))
    {
        // Put loose page numbers in mt_free_pgs, since
        // we may be unable to return them to me_pghead.
        MDB_page* mp = txn->mt_loose_pgs;
        MDB_ID2* dl = txn->mt_u.dirty_list;
        unsigned x;
        rc = mdb_midl_need(&txn->mt_free_pgs, txn->mt_loose_count);
        if (rc != 0)
            return rc;
        for (; mp != nullptr; mp = NEXT_LOOSE_PAGE(mp))
        {
            mdb_midl_xappend(txn->mt_free_pgs, mp->mp_pgno);
            // must also remove from dirty list
            if ((txn->mt_flags & MDB_TXN_WRITEMAP) != 0U)
            {
                for (x = 1; x <= dl[0].mid; x++)
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
            dl[x].mptr = nullptr;
        }
        {
            // squash freed slots out of the dirty list
            unsigned y;
            for (y = 1; (dl[y].mptr != nullptr) && y <= dl[0].mid; y++)
                ;
            if (y <= dl[0].mid)
            {
                for (x = y, y++;;)
                {
                    while ((dl[y].mptr == nullptr) && y <= dl[0].mid)
                        y++;
                    if (y > dl[0].mid)
                        break;
                    dl[x++] = dl[y++];
                }
                dl[0].mid = x - 1;
            }
            else
            {
                // all slots freed
                dl[0].mid = 0;
            }
        }
        txn->mt_loose_pgs = nullptr;
        txn->mt_loose_count = 0;
    }

    // MDB_RESERVE cancels meminit in ovpage malloc (when no WRITEMAP)
    clean_limit = ((env->me_flags & (MDB_NOMEMINIT | MDB_WRITEMAP)) != 0U) ? SSIZE_MAX : maxfree_1pg;

    for (;;)
    {
        // Come back here after each Put() in case freelist changed
        MDB_val key{};
        MDB_val data{};
        pgno_t* pgs{nullptr};
        ssize_t j{};

        // If using records from freeDB which we have not yet
        // deleted, delete them and any we reserved for me_pghead.
        while (pglast < env->me_pglast)
        {
            rc = mdb_cursor_first(&mc, &key, nullptr);
            if (rc != 0)
                return rc;
            pglast = head_id = *(txnid_t*)key.mv_data;
            total_room = head_room = 0;
            mdb_tassert(txn, pglast <= env->me_pglast);
            rc = mdb_cursor_del_impl(&mc, 0);
            if (rc != 0)
                return rc;
        }

        // Save the IDL of pages freed by this txn, to a single record
        if (freecnt < txn->mt_free_pgs[0])
        {
            if (freecnt == 0U)
            {
                // Make sure last page of freeDB is touched and on freelist
                rc = mdb_page_search(&mc, nullptr, MDB_PS_LAST | MDB_PS_MODIFY);
                if ((rc != 0) && rc != MDB_NOTFOUND)
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
                rc = mdb_cursor_put_impl(&mc, &key, &data, MDB_RESERVE);
                if (rc != 0)
                    return rc;
                // Retry if mt_free_pgs[] grew during the Put()
                free_pgs = txn->mt_free_pgs;
            } while (freecnt < free_pgs[0]);
            mdb_midl_sort(free_pgs);
            memcpy(data.mv_data, free_pgs, data.mv_size);
#if (MDB_DEBUG) > 1
            {
                unsigned int i = free_pgs[0];
                DPRINTF(("IDL write txn %" Yu " root %" Yu " num %u", txn->mt_txnid, txn->mt_dbs[FREE_DBI].md_root, i));
                for (; i; i--)
                    DPRINTF(("IDL %" Yu, free_pgs[i]));
            }
#endif
            continue;
        }

        mop = env->me_pghead;
        mop_len = ((mop != nullptr) ? mop[0] : 0) + txn->mt_loose_count;

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
            head_room /= head_id;  // amortize page sizes
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
        rc = mdb_cursor_put_impl(&mc, &key, &data, MDB_RESERVE);
        if (rc != 0)
            return rc;
        // IDL is initially empty, zero out at least the length
        pgs = (pgno_t*)data.mv_data;
        j = head_room > clean_limit ? head_room : 0;
        do
        {
            pgs[j] = 0;
        } while (--j >= 0);
        total_room += head_room;
    }

    // Return loose page numbers to me_pghead, though usually none are
    // left at this point.  The pages themselves remain in dirty_list.
    if (txn->mt_loose_pgs != nullptr)
    {
        MDB_page* mp = txn->mt_loose_pgs;
        unsigned count = txn->mt_loose_count;
        MDB_IDL loose;
        // Room for loose pages + temp IDL with same
        rc = mdb_midl_need(&env->me_pghead, (2 * count) + 1);
        if (rc != 0)
            return rc;
        mop = env->me_pghead;
        loose = mop + MDB_IDL_ALLOCLEN(mop) - count;
        for (count = 0; mp != nullptr; mp = NEXT_LOOSE_PAGE(mp))
            loose[++count] = mp->mp_pgno;
        loose[0] = count;
        mdb_midl_sort(loose);
        mdb_midl_xmerge(mop, loose);
        txn->mt_loose_pgs = nullptr;
        txn->mt_loose_count = 0;
        mop_len = mop[0];
    }

    // Fill in the reserved me_pghead records
    rc = MDB_SUCCESS;
    if (mop_len != 0)
    {
        MDB_val key{};
        MDB_val data{};

        mop += mop_len;
        rc = mdb_cursor_first(&mc, &key, &data);
        for (; rc == 0; rc = mdb_cursor_next(&mc, &key, &data, MDB_NEXT))
        {
            txnid_t id = *(txnid_t*)key.mv_data;
            ssize_t len = (ssize_t)(data.mv_size / sizeof(MDB_ID)) - 1;
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
            rc = mdb_cursor_put_impl(&mc, &key, &data, MDB_CURRENT);
            mop[0] = save;
            mop_len -= len;
            if ((rc != 0) || (mop_len == 0))
                break;
        }
    }
    return rc;
}

// TODO: Fix Single-Purpose Variable violations in mdb_txn_commit_impl()
// This function has extensive variable re-assignments that violate single-purpose principle
// Variables rc, i, x, y, len, ps_len are re-purposed throughout the function
// Requires careful refactoring to maintain complex transaction commit logic
auto mdb_txn_commit_impl(MDB_txn* txn) -> int
{
    int rc{};
    unsigned int i{};
    unsigned int end_mode{};
    MDB_env* env{nullptr};

    if (txn == nullptr)
        return EINVAL;

    // mdb_txn_end() mode for a commit which writes nothing
    end_mode = static_cast<unsigned>(MDB_END_EMPTY_COMMIT) | static_cast<unsigned>(MDB_END_UPDATE) |
               static_cast<unsigned>(MDB_END_SLOT) | static_cast<unsigned>(MDB_END_FREE);

    if (txn->mt_child != nullptr)
    {
        rc = mdb_txn_commit_impl(txn->mt_child);
        if (rc != 0)
            goto fail;
    }

    env = txn->mt_env;

    if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
    {
        goto done;
    }

    if ((txn->mt_flags & (MDB_TXN_FINISHED | MDB_TXN_ERROR)) != 0U)
    {
        DPUTS("txn has failed/finished, can't commit");
        if (txn->mt_parent != nullptr)
            txn->mt_parent->mt_flags |= MDB_TXN_ERROR;
        rc = MDB_BAD_TXN;
        goto fail;
    }

    if (txn->mt_parent != nullptr)
    {
        MDB_txn* parent = txn->mt_parent;
        MDB_page** lp{nullptr};
        MDB_ID2L dst{nullptr};
        MDB_ID2L src{nullptr};
        MDB_IDL pspill{nullptr};
        unsigned x{};
        unsigned y{};
        unsigned len{};
        unsigned ps_len{};

        // Append our free list to parent's
        rc = mdb_midl_append_list(&parent->mt_free_pgs, txn->mt_free_pgs);
        if (rc != 0)
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
        for (i = CORE_DBS; i < txn->mt_numdbs; i++)
        {
            // preserve parent's DB_NEW status
            x = parent->mt_dbflags[i] & DB_NEW;
            parent->mt_dbflags[i] = txn->mt_dbflags[i] | x;
        }

        dst = parent->mt_u.dirty_list;
        src = txn->mt_u.dirty_list;
        // Remove anything in our dirty list from parent's spill list
        pspill = parent->mt_spill_pgs;
        if (pspill != nullptr)
        {
            ps_len = pspill[0];
            if (ps_len != 0U)
            {
                x = y = ps_len;
                pspill[0] = (pgno_t)-1;
                // Mark our dirty pages as deleted in parent spill list
                for (i = 0, len = src[0].mid; ++i <= len;)
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
                for (x = y; ++x <= ps_len;)
                    if ((pspill[x] & 1) == 0U)
                        pspill[++y] = pspill[x];
                pspill[0] = y;
            }
        }

        // Remove anything in our spill list from parent's dirty list
        if ((txn->mt_spill_pgs != nullptr) && (txn->mt_spill_pgs[0] != 0U))
        {
            for (i = 1; i <= txn->mt_spill_pgs[0]; i++)
            {
                MDB_ID pn = txn->mt_spill_pgs[i];
                if ((pn & 1) != 0U)
                    continue;  // deleted spillpg
                pn >>= 1;
                y = mdb_mid2l_search(dst, pn);
                if (y <= dst[0].mid && dst[y].mid == pn)
                {
                    free(dst[y].mptr);
                    while (y < dst[0].mid)
                    {
                        dst[y] = dst[y + 1];
                        y++;
                    }
                    dst[0].mid--;
                }
            }
        }

        // Find len = length of merging our dirty list with parent's
        x = dst[0].mid;
        dst[0].mid = 0;  // simplify loops
        if (parent->mt_parent != nullptr)
        {
            len = x + src[0].mid;
            y = mdb_mid2l_search(src, dst[x].mid + 1) - 1;
            for (i = x; (y != 0U) && (i != 0U); y--)
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
        {  // Simplify the above for single-ancestor case
            len = MDB_IDL_UM_MAX - txn->mt_dirty_room;
        }
        // Merge our dirty list with parent's
        y = src[0].mid;
        for (i = len; y != 0U; dst[i--] = src[y--])
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
        if (txn->mt_spill_pgs != nullptr)
        {
            if (parent->mt_spill_pgs != nullptr)
            {
                // TODO: Prevent failure here, so parent does not fail
                rc = mdb_midl_append_list(&parent->mt_spill_pgs, txn->mt_spill_pgs);
                if (rc != 0)
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
        for (lp = &parent->mt_loose_pgs; *lp != nullptr; lp = &NEXT_LOOSE_PAGE(*lp))
            ;
        *lp = txn->mt_loose_pgs;
        parent->mt_loose_count += txn->mt_loose_count;

        parent->mt_child = nullptr;
        mdb_midl_free(((MDB_ntxn*)txn)->mnt_pgstate.mf_pghead);
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

    if ((txn->mt_u.dirty_list[0].mid == 0U) && ((txn->mt_flags & (MDB_TXN_DIRTY | MDB_TXN_SPILLS)) == 0U))
        goto done;

    DPRINTF(("committing txn %" Yu " %p on mdbenv %p, root page %" Yu,
             txn->mt_txnid,
             (void*)txn,
             (void*)env,
             txn->mt_dbs[MAIN_DBI].md_root));

    // Update DB root pointers
    if (txn->mt_numdbs > CORE_DBS)
    {
        MDB_cursor mc;
        MDB_dbi i;
        MDB_val data;
        data.mv_size = sizeof(MDB_db);

        mdb_cursor_init(&mc, txn, MAIN_DBI, nullptr);
        for (i = CORE_DBS; i < txn->mt_numdbs; i++)
        {
            if ((txn->mt_dbflags[i] & DB_DIRTY) != 0)
            {
                if (TXN_DBI_CHANGED(txn, i))
                {
                    rc = MDB_BAD_DBI;
                    goto fail;
                }
                data.mv_data = &txn->mt_dbs[i];
                rc = mdb_cursor_put_impl(&mc, &txn->mt_dbxs[i].md_name, &data, F_SUBDATA);
                if (rc != 0)
                    goto fail;
            }
        }
    }

    rc = mdb_freelist_save(txn);
    if (rc != 0)
        goto fail;

    mdb_midl_free(env->me_pghead);
    env->me_pghead = nullptr;
    mdb_midl_shrink(&txn->mt_free_pgs);

#if (MDB_DEBUG) > 2
    mdb_audit(txn);
#endif

    rc = mdb_page_flush(txn, 0);
    if (rc != 0)
        goto fail;
    if (!F_ISSET(txn->mt_flags, MDB_TXN_NOSYNC))
    {
        rc = mdb_env_sync0(env, 0, txn->mt_next_pgno);
        if (rc != 0)
            goto fail;
    }
    rc = mdb_env_write_meta(txn);
    if (rc != 0)
        goto fail;
    end_mode = static_cast<unsigned>(MDB_END_COMMITTED) | static_cast<unsigned>(MDB_END_UPDATE);
    if ((env->me_flags & MDB_PREVSNAPSHOT) != 0U)
    {
        if ((env->me_flags & MDB_NOLOCK) == 0U)
        {
            int excl;
            rc = mdb_env_share_locks(env, &excl);
            if (rc != 0)
                goto fail;
        }
        env->me_flags ^= MDB_PREVSNAPSHOT;
    }

done:
    mdb_txn_end(txn, end_mode);
    return MDB_SUCCESS;

fail:
    mdb_txn_abort_impl(txn);
    return rc;
}

auto mdb_txn_commit(MDB_txn* txn) -> int
{
    MDB_TRACE(("%p", txn));
    return mdb_txn_commit_impl(txn);
}

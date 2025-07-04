#include "txn.h"

#include "btree.h"
#include "cursor.h"
#include "db.h"
#include "debug.h"
#include "env.h"
#include "lock.h"

#include <utility>

// Nested transaction
struct FDS_ntxn
{
    FDS_txn mnt_txn;          //< the transaction
    FDS_pgstate mnt_pgstate;  //< parent transaction's saved freestate
};

// Common code for #fds_txn_begin() and #fds_txn_renew().
// [in] txn the transaction handle to initialize
//  0 on success, non-zero on failure.
auto fds_txn_renew0(FDS_txn* txn) -> int
{
    FDS_env* env = txn->mt_env;
    FDS_txninfo* ti = env->me_txns;
    FDS_meta* meta{nullptr};
    unsigned int i{};
    unsigned int nr{};
    unsigned int flags = txn->mt_flags;
    int rc{};
    int new_notls{0};

    flags &= FDS_TXN_RDONLY;
    if (flags != 0)
    {
        if (ti == nullptr)
        {
            meta = fds_env_pick_meta(env);
            txn->mt_txnid = meta->mm_txnid;
            txn->mt_u.reader = nullptr;
        }
        else
        {
            auto* r = (FDS_reader*)(((env->me_flags & FDS_NOTLS) != 0U) ? txn->mt_u.reader
                                                                        : pthread_getspecific(env->me_txkey));
            if (r != nullptr)
            {
                if ((r->mrx.mrb_pid != env->me_pid) || (r->mrx.mrb_txnid != -1))
                {
                    return FDS_BAD_RSLOT;
                }
            }
            else
            {
                FDS_PID_T pid = env->me_pid;
                FDS_THR_T tid = pthread_self();
                fds_mutexref_t rmutex = env->me_rmutex;

                if (env->me_live_reader == 0)
                {
                    rc = fds_reader_pid(env, Pidset, pid);
                    if (rc != 0)
                        return rc;
                    env->me_live_reader = 1;
                }

                LOCK_MUTEX(rc, env, rmutex);
                if (rc != 0)
                    return rc;
                nr = ti->mtb.mtb_numreaders;
                for (i = 0; i < nr; i++)
                    if (ti->mti_readers[i].mrx.mrb_pid == 0)
                        break;
                if (i == env->me_maxreaders)
                {
                    UNLOCK_MUTEX(rmutex);
                    return FDS_READERS_FULL;
                }
                r = &ti->mti_readers[i];
                // Claim the reader slot, carefully since other code
                // uses the reader table un-mutexed: First reset the
                // slot, next publish it in mtb.mtb_numreaders.  After
                // that, it is safe for fds_env_close() to touch it.
                // When it will be closed, we can finally claim it.
                r->mrx.mrb_pid = 0;
                r->mrx.mrb_txnid = (txnid_t)-1;
                r->mrx.mrb_tid = tid;
                if (i == nr)
                    ti->mtb.mtb_numreaders = ++nr;
                env->me_close_readers = nr;
                r->mrx.mrb_pid = pid;
                UNLOCK_MUTEX(rmutex);

                new_notls = (env->me_flags & FDS_NOTLS);
                if (new_notls == 0)
                {
                    rc = pthread_setspecific(env->me_txkey, r);
                    if (rc != 0)
                    {
                        r->mrx.mrb_pid = 0;
                        return rc;
                    }
                }
            }
            do /* LY: Retry on a race, ITS#7970. */
                r->mrx.mrb_txnid = ti->mtb.mtb_txnid;
            while (r->mrx.mrb_txnid != ti->mtb.mtb_txnid);
            if ((r->mrx.mrb_txnid == 0U) && ((env->me_flags & FDS_RDONLY) != 0U))
            {
                meta = fds_env_pick_meta(env);
                r->mrx.mrb_txnid = meta->mm_txnid;
            }
            else
            {
                meta = env->me_metas[r->mrx.mrb_txnid & 1];
            }
            txn->mt_txnid = r->mrx.mrb_txnid;
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
            txn->mt_txnid = ti->mtb.mtb_txnid;
            meta = env->me_metas[txn->mt_txnid & 1];
        }
        else
        {
            meta = fds_env_pick_meta(env);
            txn->mt_txnid = meta->mm_txnid;
        }
        txn->mt_txnid++;
#if FDS_DEBUG
        if (txn->mt_txnid == fds_debug_start)
            fds_debug = FDS_DBG_INFO;
#endif
        txn->mt_child = nullptr;
        txn->mt_loose_pgs = nullptr;
        txn->mt_loose_count = 0;
        txn->mt_dirty_room = FDS_IDL_UM_MAX;
        txn->mt_u.dirty_list = env->me_dirty_list;
        txn->mt_u.dirty_list[0].mid = 0;
        txn->mt_free_pgs = env->me_free_pgs;
        txn->mt_free_pgs[0] = 0;
        txn->mt_spill_pgs = nullptr;
        env->me_txn = txn;
        memcpy(txn->mt_dbiseqs, env->me_dbiseqs, env->me_maxdbs * sizeof(unsigned int));
    }

    // Copy the DB info and flags
    memcpy(txn->mt_dbs, meta->mm_dbs, CORE_DBS * sizeof(FDS_db));

    // Moved to here to avoid a data race in read TXNs
    txn->mt_next_pgno = meta->mm_last_pg + 1;
    txn->mt_flags = flags;

    // Setup db info
    txn->mt_numdbs = env->me_numdbs;
    for (i = CORE_DBS; i < txn->mt_numdbs; i++)
    {
        uint16_t x = env->me_dbflags[i];
        txn->mt_dbs[i].md_flags = x & PERSISTENT_FLAGS;
        txn->mt_dbflags[i] = ((x & FDS_VALID) != 0) ? DB_VALID | DB_USRVALID | DB_STALE : 0;
    }
    txn->mt_dbflags[MAIN_DBI] = DB_VALID | DB_USRVALID;
    txn->mt_dbflags[FREE_DBI] = DB_VALID;

    if ((env->me_flags & FDS_FATAL_ERROR) != 0U)
    {
        DPUTS("environment had fatal error, must shutdown!");
        rc = FDS_PANIC;
    }
    else if (env->me_maxpg < txn->mt_next_pgno)
    {
        rc = FDS_MAP_RESIZED;
    }
    else
    {
        return FDS_SUCCESS;
    }
    fds_txn_end(txn, new_notls /*0 or FDS_END_SLOT*/ | FDS_END_FAIL_BEGIN);
    return rc;
}

auto fds_txn_renew(FDS_txn* txn) -> int
{
    int rc;

    if ((txn == nullptr) || !F_ISSET(txn->mt_flags, FDS_TXN_RDONLY | FDS_TXN_FINISHED))
        return EINVAL;

    rc = fds_txn_renew0(txn);
    if (rc == FDS_SUCCESS)
    {
        DPRINTF(("renew txn %" Yu "%c %p on mdbenv %p, root page %" Yu,
                 txn->mt_txnid,
                 (txn->mt_flags & FDS_TXN_RDONLY) ? 'r' : 'w',
                 (void*)txn,
                 (void*)txn->mt_env,
                 txn->mt_dbs[MAIN_DBI].md_root));
    }
    return rc;
}

// Back up parent txn's cursors, then grab the originals for tracking
static auto fds_cursor_shadow(FDS_txn* src, FDS_txn* dst) -> int
{
    int i{};

    for (i = src->mt_numdbs; --i >= 0;)
    {
        FDS_cursor* current_cursor = src->mt_cursors[i];
        if (current_cursor != nullptr)
        {
            const size_t base_cursor_size = sizeof(FDS_cursor);
            // No xcursor support - simplified cursor size
            const size_t total_cursor_size = base_cursor_size;

            for (; current_cursor != nullptr;)
            {
                auto* const backup_cursor = (FDS_cursor*)malloc(total_cursor_size);
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
    return FDS_SUCCESS;
}

auto fds_txn_begin(FDS_env* env, FDS_txn* parent, unsigned int flags, FDS_txn** txn) -> int
{
    FDS_txn* new_txn{nullptr};
    FDS_ntxn* ntxn{nullptr};
    int rc{};
    int size{};
    int tsize{};

    flags &= FDS_TXN_BEGIN_FLAGS;

    if ((env->me_flags & FDS_RDONLY & ~flags) != 0U) /* write txn in RDONLY env */
        return EACCES;

    if (parent != nullptr)
    {
        // Nested transactions: Max 1 child, write txns only, no writemap
        flags |= parent->mt_flags;
        if ((flags & (FDS_RDONLY | FDS_TXN_BLOCKED)) != 0U)
        {
            return ((parent->mt_flags & FDS_TXN_RDONLY) != 0U) ? EINVAL : FDS_BAD_TXN;
        }
        // Child txns save FDS_pgstate and use own copy of cursors
        size = env->me_maxdbs * (sizeof(FDS_db) + sizeof(FDS_cursor*) + 1);
        size += tsize = sizeof(FDS_ntxn);
    }
    else if ((flags & FDS_RDONLY) != 0U)
    {
        size = env->me_maxdbs * (sizeof(FDS_db) + 1);
        size += tsize = sizeof(FDS_txn);
    }
    else
    {
        // Reuse preallocated write txn. However, do not touch it until
        // fds_txn_renew0() succeeds, since it currently may be active.
        new_txn = env->me_txn0;
        goto renew;
    }
    new_txn = (FDS_txn*)calloc(1, size);
    if (new_txn == nullptr)
        return ENOMEM;

    new_txn->mt_dbxs = env->me_dbxs;  // static
    new_txn->mt_dbs = (FDS_db*)((char*)new_txn + tsize);
    new_txn->mt_dbflags = (unsigned char*)new_txn + size - env->me_maxdbs;
    new_txn->mt_flags = flags;
    new_txn->mt_env = env;

    if (parent != nullptr)
    {
        unsigned int i;
        new_txn->mt_cursors = (FDS_cursor**)(new_txn->mt_dbs + env->me_maxdbs);
        new_txn->mt_dbiseqs = parent->mt_dbiseqs;
        new_txn->mt_u.dirty_list = (FDS_ID2L)malloc(sizeof(FDS_ID2) * FDS_IDL_UM_SIZE);
        new_txn->mt_free_pgs = fds_midl_alloc(FDS_IDL_UM_MAX);
        if ((new_txn->mt_u.dirty_list == nullptr) || (new_txn->mt_free_pgs == nullptr))
        {
            free(new_txn->mt_u.dirty_list);
            free(new_txn);
            return ENOMEM;
        }
        new_txn->mt_txnid = parent->mt_txnid;
        new_txn->mt_dirty_room = parent->mt_dirty_room;
        new_txn->mt_u.dirty_list[0].mid = 0;
        new_txn->mt_spill_pgs = nullptr;
        new_txn->mt_next_pgno = parent->mt_next_pgno;
        parent->mt_flags |= FDS_TXN_HAS_CHILD;
        parent->mt_child = new_txn;
        new_txn->mt_parent = parent;
        new_txn->mt_numdbs = parent->mt_numdbs;
        memcpy(new_txn->mt_dbs, parent->mt_dbs, new_txn->mt_numdbs * sizeof(FDS_db));
        // Copy parent's mt_dbflags, but clear DB_NEW
        for (i = 0; i < new_txn->mt_numdbs; i++)
            new_txn->mt_dbflags[i] = parent->mt_dbflags[i] & ~DB_NEW;
        rc = 0;
        ntxn = (FDS_ntxn*)new_txn;
        ntxn->mnt_pgstate = env->me_pgstate;  // save parent me_pgstate.mf_pghead & co
        if (env->me_pgstate.mf_pghead != nullptr)
        {
            size = FDS_IDL_SIZEOF(env->me_pgstate.mf_pghead);
            env->me_pgstate.mf_pghead = fds_midl_alloc(env->me_pgstate.mf_pghead[0]);
            if (env->me_pgstate.mf_pghead != nullptr)
                memcpy(env->me_pgstate.mf_pghead, ntxn->mnt_pgstate.mf_pghead, size);
            else
                rc = ENOMEM;
        }
        if (rc == 0)
            rc = fds_cursor_shadow(parent, new_txn);
        if (rc != 0)
            fds_txn_end(new_txn, FDS_END_FAIL_BEGINCHILD);
    }
    else
    { /* FDS_RDONLY */
        new_txn->mt_dbiseqs = env->me_dbiseqs;
    renew:
        rc = fds_txn_renew0(new_txn);
    }
    if (rc != 0)
    {
        if (new_txn != env->me_txn0)
        {
            free(new_txn);
        }
    }
    else
    {
        new_txn->mt_flags |= flags; /* could not change new_txn=me_txn0 earlier */
        *txn = new_txn;
        DPRINTF(("begin txn %" Yu "%c %p on mdbenv %p, root page %" Yu,
                 new_txn->mt_txnid,
                 (flags & FDS_RDONLY) ? 'r' : 'w',
                 (void*)new_txn,
                 (void*)env,
                 new_txn->mt_dbs[MAIN_DBI].md_root));
    }
    DPRINTF(("%p, %p, %u = %p", env, parent, flags, txn));

    return rc;
}

auto fds_txn_env(FDS_txn* txn) -> FDS_env*
{
    if (txn == nullptr)
        return nullptr;
    return txn->mt_env;
}

auto fds_txn_id(FDS_txn* txn) -> size_t
{
    if (txn == nullptr)
        return 0;
    return txn->mt_txnid;
}

// Export or close DBI handles opened in this txn.
static void fds_dbis_update(FDS_txn* txn, int keep)
{
    int i;
    FDS_dbi n = txn->mt_numdbs;
    FDS_env* env = txn->mt_env;
    unsigned char* tdbflags = txn->mt_dbflags;

    for (i = n; --i >= CORE_DBS;)
    {
        if ((tdbflags[i] & DB_NEW) != 0)
        {
            if (keep != 0)
            {
                env->me_dbflags[i] = txn->mt_dbs[i].md_flags | FDS_VALID;
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
static void fds_cursors_close(FDS_txn* txn, unsigned merge)
{
    FDS_cursor** cursors = txn->mt_cursors;
    FDS_cursor* mc;
    FDS_cursor* next;
    FDS_cursor* bk;
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
static void fds_dlist_free(FDS_txn* txn)
{
    FDS_env* env = txn->mt_env;
    FDS_ID2L dl = txn->mt_u.dirty_list;
    unsigned i{};
    unsigned n = dl[0].mid;

    for (i = 1; i <= n; i++)
    {
        fds_dpage_free(env, (FDS_page*)dl[i].mptr);
    }
    dl[0].mid = 0;
}

#define FDS_END_NAMES {"committed", "empty-commit", "abort", "reset", "reset-tmp", "fail-begin", "fail-beginchild"}

// End a transaction, except successful commit of a nested transaction.
// May be called twice for readonly txns: First reset it, then abort.
// [in] txn the transaction handle to end
// [in] mode why and how to end the transaction
void fds_txn_end(FDS_txn* txn, unsigned mode)
{
    FDS_env* env = txn->mt_env;
#if FDS_DEBUG
    static const char* const names[] = FDS_END_NAMES;
#endif

    // Export or close DBI handles opened in this txn
    fds_dbis_update(txn, mode & FDS_END_UPDATE);

    DPRINTF(("%s txn %" Yu "%c %p on mdbenv %p, root page %" Yu,
             names[mode & FDS_END_OPMASK],
             txn->mt_txnid,
             (txn->mt_flags & FDS_TXN_RDONLY) ? 'r' : 'w',
             (void*)txn,
             (void*)env,
             txn->mt_dbs[MAIN_DBI].md_root));

    if (F_ISSET(txn->mt_flags, FDS_TXN_RDONLY))
    {
        if (txn->mt_u.reader != nullptr)
        {
            txn->mt_u.reader->mrx.mrb_txnid = (txnid_t)-1;
            if ((env->me_flags & FDS_NOTLS) == 0U)
            {
                txn->mt_u.reader = nullptr; /* txn does not own reader */
            }
            else if ((mode & FDS_END_SLOT) != 0U)
            {
                txn->mt_u.reader->mrx.mrb_pid = 0;
                txn->mt_u.reader = nullptr;
            } /* else txn owns the slot until it does FDS_END_SLOT */
        }
        txn->mt_numdbs = 0; /* prevent further DBI activity */
        txn->mt_flags |= FDS_TXN_FINISHED;
    }
    else if (!F_ISSET(txn->mt_flags, FDS_TXN_FINISHED))
    {
        pgno_t* pghead = env->me_pgstate.mf_pghead;

        if ((mode & FDS_END_UPDATE) == 0U)  // !(already closed cursors)
            fds_cursors_close(txn, 0);

        fds_dlist_free(txn);

        txn->mt_numdbs = 0;
        txn->mt_flags = FDS_TXN_FINISHED;

        if (txn->mt_parent == nullptr)
        {
            fds_midl_shrink(&txn->mt_free_pgs);
            env->me_free_pgs = txn->mt_free_pgs;
            // me_pgstate:
            env->me_pgstate.mf_pghead = nullptr;
            env->me_pgstate.mf_pglast = 0;

            env->me_txn = nullptr;
            mode = 0;  // txn == env->me_txn0, do not free() it

            /* The writer mutex was locked in fds_txn_begin. */
            if (env->me_txns != nullptr)
                UNLOCK_MUTEX(env->me_wmutex);
        }
        else
        {
            txn->mt_parent->mt_child = nullptr;
            txn->mt_parent->mt_flags &= ~FDS_TXN_HAS_CHILD;
            env->me_pgstate = ((FDS_ntxn*)txn)->mnt_pgstate;
            fds_midl_free(txn->mt_free_pgs);
            free(txn->mt_u.dirty_list);
        }
        fds_midl_free(txn->mt_spill_pgs);

        fds_midl_free(pghead);
    }
    if ((mode & FDS_END_FREE) != 0U)
        free(txn);
}

void fds_txn_reset(FDS_txn* txn)
{
    if (txn == nullptr)
        return;

    // This call is only valid for read-only txns
    if ((txn->mt_flags & FDS_TXN_RDONLY) == 0U)
        return;

    fds_txn_end(txn, FDS_END_RESET);
}

void fds_txn_abort_impl(FDS_txn* txn)
{
    if (txn == nullptr)
        return;

    if (txn->mt_child != nullptr)
        fds_txn_abort_impl(txn->mt_child);

    fds_txn_end(txn,
                static_cast<unsigned>(FDS_END_ABORT) | static_cast<unsigned>(FDS_END_SLOT) |
                    static_cast<unsigned>(FDS_END_FREE));
}

void fds_txn_abort(FDS_txn* txn)
{
    DPRINTF(("%p", txn));
    fds_txn_abort_impl(txn);
}

// Save the freelist as of this transaction to the freeDB.
// This changes the freelist. Keep trying until it stabilizes.
auto fds_freelist_save(FDS_txn* txn) -> int
{
    // env->me_pgstate.mf_pghead[] can grow and shrink during this call.
    // env->me_pgstate.mf_pglast and txn->mt_free_pgs[] can only grow.
    // Page numbers cannot disappear from txn->mt_free_pgs[].
    FDS_cursor mc{};
    FDS_env* env = txn->mt_env;
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

    fds_cursor_init(&mc, txn, FREE_DBI);

    if (env->me_pgstate.mf_pghead != nullptr)
    {
        // Make sure first page of freeDB is touched and on freelist
        rc = fds_page_search(&mc, nullptr, FDS_PS_FIRST | FDS_PS_MODIFY);
        if ((rc != 0) && rc != FDS_NOTFOUND)
            return rc;
    }

    if ((env->me_pgstate.mf_pghead == nullptr) && (txn->mt_loose_pgs != nullptr))
    {
        // Put loose page numbers in mt_free_pgs, since
        // we may be unable to return them to me_pgstate.mf_pghead.
        FDS_page* mp = txn->mt_loose_pgs;
        FDS_ID2* dl = txn->mt_u.dirty_list;
        unsigned x;
        rc = fds_midl_need(&txn->mt_free_pgs, txn->mt_loose_count);
        if (rc != 0)
            return rc;
        for (; mp != nullptr; mp = NEXT_LOOSE_PAGE(mp))
        {
            fds_midl_xappend(txn->mt_free_pgs, mp->mp_pgno);
            // must also remove from dirty list
            x = fds_mid2l_search(dl, mp->mp_pgno);
            fds_tassert(txn, dl[x].mid == mp->mp_pgno);
            fds_dpage_free(env, mp);
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

    // FDS_RESERVE cancels meminit in ovpage malloc (when no WRITEMAP)
    clean_limit = maxfree_1pg;

    for (;;)
    {
        // Come back here after each Put() in case freelist changed
        FDS_val key{};
        FDS_val data{};
        pgno_t* pgs{nullptr};
        ssize_t j{};

        // If using records from freeDB which we have not yet
        // deleted, delete them and any we reserved for me_pgstate.mf_pghead.
        while (pglast < env->me_pgstate.mf_pglast)
        {
            rc = fds_cursor_first(&mc, &key, nullptr);
            if (rc != 0)
                return rc;
            pglast = head_id = *(txnid_t*)key.mv_data;
            total_room = head_room = 0;
            fds_tassert(txn, pglast <= env->me_pgstate.mf_pglast);
            rc = fds_cursor_del_impl(&mc, 0);
            if (rc != 0)
                return rc;
        }

        // Save the IDL of pages freed by this txn, to a single record
        if (freecnt < txn->mt_free_pgs[0])
        {
            if (freecnt == 0U)
            {
                // Make sure last page of freeDB is touched and on freelist
                rc = fds_page_search(&mc, nullptr, FDS_PS_LAST | FDS_PS_MODIFY);
                if ((rc != 0) && rc != FDS_NOTFOUND)
                    return rc;
            }
            free_pgs = txn->mt_free_pgs;
            // Write to last page of freeDB
            key.mv_size = sizeof(txn->mt_txnid);
            key.mv_data = &txn->mt_txnid;
            do
            {
                freecnt = free_pgs[0];
                data.mv_size = FDS_IDL_SIZEOF(free_pgs);
                rc = fds_cursor_put_impl(&mc, &key, &data, FDS_RESERVE);
                if (rc != 0)
                    return rc;
                // Retry if mt_free_pgs[] grew during the Put()
                free_pgs = txn->mt_free_pgs;
            } while (freecnt < free_pgs[0]);
            fds_midl_sort(free_pgs);
            memcpy(data.mv_data, free_pgs, data.mv_size);
#if (FDS_DEBUG) > 1
            {
                unsigned int i = free_pgs[0];
                DPRINTF(("IDL write txn %" Yu " root %" Yu " num %u", txn->mt_txnid, txn->mt_dbs[FREE_DBI].md_root, i));
                for (; i; i--)
                    DPRINTF(("IDL %" Yu, free_pgs[i]));
            }
#endif
            continue;
        }

        mop = env->me_pgstate.mf_pghead;
        mop_len = ((mop != nullptr) ? mop[0] : 0) + txn->mt_loose_count;

        // Reserve records for me_pgstate.mf_pghead[]. Split it if multi-page,
        // to avoid searching freeDB for a page range. Use keys in
        // range [1,me_pgstate.mf_pglast]: Smaller than txnid of oldest reader.
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
            // Overflow multi-page for part of me_pgstate.mf_pghead
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
        rc = fds_cursor_put_impl(&mc, &key, &data, FDS_RESERVE);
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

    // Return loose page numbers to me_pgstate.mf_pghead, though usually none are
    // left at this point.  The pages themselves remain in dirty_list.
    if (txn->mt_loose_pgs != nullptr)
    {
        FDS_page* mp = txn->mt_loose_pgs;
        unsigned count = txn->mt_loose_count;
        FDS_IDL loose;
        // Room for loose pages + temp IDL with same
        rc = fds_midl_need(&env->me_pgstate.mf_pghead, (2 * count) + 1);
        if (rc != 0)
            return rc;
        mop = env->me_pgstate.mf_pghead;
        loose = mop + FDS_IDL_ALLOCLEN(mop) - count;
        for (count = 0; mp != nullptr; mp = NEXT_LOOSE_PAGE(mp))
            loose[++count] = mp->mp_pgno;
        loose[0] = count;
        fds_midl_sort(loose);
        fds_midl_xmerge(mop, loose);
        txn->mt_loose_pgs = nullptr;
        txn->mt_loose_count = 0;
        mop_len = mop[0];
    }

    // Fill in the reserved me_pgstate.mf_pghead records
    rc = FDS_SUCCESS;
    if (mop_len != 0)
    {
        FDS_val key{};
        FDS_val data{};

        mop += mop_len;
        rc = fds_cursor_first(&mc, &key, &data);
        for (; rc == 0; rc = fds_cursor_next(&mc, &key, &data, FDS_NEXT))
        {
            txnid_t id = *(txnid_t*)key.mv_data;
            ssize_t len = (ssize_t)(data.mv_size / sizeof(FDS_ID)) - 1;
            FDS_ID save;

            fds_tassert(txn, len >= 0 && id <= env->me_pgstate.mf_pglast);
            key.mv_data = &id;
            if (len > mop_len)
            {
                len = mop_len;
                data.mv_size = (len + 1) * sizeof(FDS_ID);
            }
            data.mv_data = mop -= len;
            save = mop[0];
            mop[0] = len;
            rc = fds_cursor_put_impl(&mc, &key, &data, FDS_CURRENT);
            mop[0] = save;
            mop_len -= len;
            if ((rc != 0) || (mop_len == 0))
                break;
        }
    }
    return rc;
}

// TODO: Fix Single-Purpose Variable violations in fds_txn_commit_impl()
// This function has extensive variable re-assignments that violate single-purpose principle
// Variables rc, i, x, y, len, ps_len are re-purposed throughout the function
// Requires careful refactoring to maintain complex transaction commit logic
auto fds_txn_commit_impl(FDS_txn* txn) -> int
{
    int rc{};
    unsigned int i{};
    unsigned int end_mode{};
    FDS_env* env{nullptr};

    if (txn == nullptr)
        return EINVAL;

    // fds_txn_end() mode for a commit which writes nothing
    end_mode = static_cast<unsigned>(FDS_END_EMPTY_COMMIT) | static_cast<unsigned>(FDS_END_UPDATE) |
               static_cast<unsigned>(FDS_END_SLOT) | static_cast<unsigned>(FDS_END_FREE);

    if (txn->mt_child != nullptr)
    {
        rc = fds_txn_commit_impl(txn->mt_child);
        if (rc != 0)
            goto fail;
    }

    env = txn->mt_env;

    if (F_ISSET(txn->mt_flags, FDS_TXN_RDONLY))
    {
        goto done;
    }

    if ((txn->mt_flags & (FDS_TXN_FINISHED | FDS_TXN_ERROR)) != 0U)
    {
        DPUTS("txn has failed/finished, can't commit");
        if (txn->mt_parent != nullptr)
            txn->mt_parent->mt_flags |= FDS_TXN_ERROR;
        rc = FDS_BAD_TXN;
        goto fail;
    }

    if (txn->mt_parent != nullptr)
    {
        FDS_txn* parent = txn->mt_parent;
        FDS_page** lp{nullptr};
        FDS_ID2L dst{nullptr};
        FDS_ID2L src{nullptr};
        FDS_IDL pspill{nullptr};
        unsigned x{};
        unsigned y{};
        unsigned len{};
        unsigned ps_len{};

        // Append our free list to parent's
        rc = fds_midl_append_list(&parent->mt_free_pgs, txn->mt_free_pgs);
        if (rc != 0)
            goto fail;
        fds_midl_free(txn->mt_free_pgs);
        // Failures after this must either undo the changes
        // to the parent or set FDS_TXN_ERROR in the parent.

        parent->mt_next_pgno = txn->mt_next_pgno;
        parent->mt_flags = txn->mt_flags;

        // Merge our cursors into parent's and close them
        fds_cursors_close(txn, 1);

        // Update parent's DB table.
        memcpy(parent->mt_dbs, txn->mt_dbs, txn->mt_numdbs * sizeof(FDS_db));
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
                    FDS_ID pn = src[i].mid << 1;
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
                FDS_ID pn = txn->mt_spill_pgs[i];
                if ((pn & 1) != 0U)
                    continue;  // deleted spillpg
                pn >>= 1;
                y = fds_mid2l_search(dst, pn);
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
            y = fds_mid2l_search(src, dst[x].mid + 1) - 1;
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
            len = FDS_IDL_UM_MAX - txn->mt_dirty_room;
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
        fds_tassert(txn, i == x);
        dst[0].mid = len;
        free(txn->mt_u.dirty_list);
        parent->mt_dirty_room = txn->mt_dirty_room;
        if (txn->mt_spill_pgs != nullptr)
        {
            if (parent->mt_spill_pgs != nullptr)
            {
                // TODO: Prevent failure here, so parent does not fail
                rc = fds_midl_append_list(&parent->mt_spill_pgs, txn->mt_spill_pgs);
                if (rc != 0)
                    parent->mt_flags |= FDS_TXN_ERROR;
                fds_midl_free(txn->mt_spill_pgs);
                fds_midl_sort(parent->mt_spill_pgs);
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
        fds_midl_free(((FDS_ntxn*)txn)->mnt_pgstate.mf_pghead);
        free(txn);
        return rc;
    }

    if (txn != env->me_txn)
    {
        DPUTS("attempt to commit unknown transaction");
        rc = EINVAL;
        goto fail;
    }

    fds_cursors_close(txn, 0);

    if ((txn->mt_u.dirty_list[0].mid == 0U) && ((txn->mt_flags & (FDS_TXN_DIRTY | FDS_TXN_SPILLS)) == 0U))
        goto done;

    DPRINTF(("committing txn %" Yu " %p on mdbenv %p, root page %" Yu,
             txn->mt_txnid,
             (void*)txn,
             (void*)env,
             txn->mt_dbs[MAIN_DBI].md_root));

    // Update DB root pointers
    if (txn->mt_numdbs > CORE_DBS)
    {
        FDS_cursor mc;
        FDS_dbi i;
        FDS_val data;
        data.mv_size = sizeof(FDS_db);

        fds_cursor_init(&mc, txn, MAIN_DBI);
        for (i = CORE_DBS; i < txn->mt_numdbs; i++)
        {
            if ((txn->mt_dbflags[i] & DB_DIRTY) != 0)
            {
                if (TXN_DBI_CHANGED(txn, i))
                {
                    rc = FDS_BAD_DBI;
                    goto fail;
                }
                data.mv_data = &txn->mt_dbs[i];
                rc = fds_cursor_put_impl(&mc, &txn->mt_dbxs[i].md_name, &data, F_SUBDATA);
                if (rc != 0)
                    goto fail;
            }
        }
    }

    rc = fds_freelist_save(txn);
    if (rc != 0)
        goto fail;

    fds_midl_free(env->me_pgstate.mf_pghead);
    env->me_pgstate.mf_pghead = nullptr;
    fds_midl_shrink(&txn->mt_free_pgs);

#if (FDS_DEBUG) > 2
    fds_audit(txn);
#endif

    rc = fds_page_flush(txn, 0);
    if (rc != 0)
        goto fail;
    rc = fds_env_sync0(env, 0, txn->mt_next_pgno);
    if (rc != 0)
        goto fail;
    rc = fds_env_write_meta(txn);
    if (rc != 0)
        goto fail;
    end_mode = static_cast<unsigned>(FDS_END_COMMITTED) | static_cast<unsigned>(FDS_END_UPDATE);
done:
    fds_txn_end(txn, end_mode);
    return FDS_SUCCESS;

fail:
    fds_txn_abort_impl(txn);
    return rc;
}

auto fds_txn_commit(FDS_txn* txn) -> int
{
    DPRINTF(("%p", txn));
    return fds_txn_commit_impl(txn);
}

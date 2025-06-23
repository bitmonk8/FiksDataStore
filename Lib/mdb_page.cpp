#include "mdb_page.h"

#include "mdb_cursor.h"
#include "mdb_db.h"
#include "mdb_debug.h"
#include "mdb_env.h"
#include "mdb_txn.h"

// Page Management Operations
//
// Allocate memory for a page.
// Re-use old malloc'd pages first for singletons, otherwise just malloc.
// Set #MDB_TXN_ERROR on failure.
//
MDB_page* mdb_page_malloc(MDB_txn* txn, unsigned num)
{
    MDB_env* env = txn->mt_env;
    MDB_page* ret = env->me_dpages;
    size_t psize = env->me_psize, sz = psize, off;
    /* For ! #MDB_NOMEMINIT, psize counts how much to init.
     * For a single page alloc, we init everything after the page header.
     * For multi-page, we init the final page; if the caller needed that
     * many pages they will be filling in at least up to the last page.
     */
    if (num == 1)
    {
        if (ret)
        {
            VGMEMP_ALLOC(env, ret, sz);
            VGMEMP_DEFINED(ret, sizeof(ret->mp_next));
            env->me_dpages = ret->mp_next;
            return ret;
        }
        psize -= off = PAGEHDRSZ;
    }
    else
    {
        sz *= num;
        off = sz - psize;
    }
    ret = (MDB_page*)malloc(sz);
    if (ret != NULL)
    {
        VGMEMP_ALLOC(env, ret, sz);
        if (!(env->me_flags & MDB_NOMEMINIT))
        {
            memset((char*)ret + off, 0, psize);
            ret->mp_pad = 0;
        }
    }
    else
    {
        txn->mt_flags |= MDB_TXN_ERROR;
    }
    return ret;
}

// Free a single page.
// Saves single pages to a list, for future reuse.
// (This is not used for multi-page overflow pages.)
//
void mdb_page_free(MDB_env* env, MDB_page* mp)
{
    mp->mp_next = env->me_dpages;
    VGMEMP_FREE(env, mp);
    env->me_dpages = mp;
}

// Free a dirty page
void mdb_dpage_free(MDB_env* env, MDB_page* dp)
{
    if (!IS_OVERFLOW(dp) || dp->mp_pages == 1)
    {
        mdb_page_free(env, dp);
    }
    else
    {
        /* large pages just get freed directly */
        VGMEMP_FREE(env, dp);
        free(dp);
    }
}

// Loosen or free a single page.
// Saves single pages to a list for future reuse
// in this same txn. It has been pulled from the freeDB
// and already resides on the dirty list, but has been
// deleted. Use these pages first before pulling again
// from the freeDB.
//
// If the page wasn't dirtied in this txn, just add it
// to this txn's free list.
//
int mdb_page_loose(MDB_cursor* mc, MDB_page* mp)
{
    int loose = 0;
    pgno_t pgno = mp->mp_pgno;
    MDB_txn* txn = mc->mc_txn;

    if ((mp->mp_flags & P_DIRTY) && mc->mc_dbi != FREE_DBI)
    {
        if (txn->mt_parent)
        {
            MDB_ID2* dl = txn->mt_u.dirty_list;
            /* If txn has a parent, make sure the page is in our
             * dirty list.
             */
            if (dl[0].mid)
            {
                unsigned x = mdb_mid2l_search(dl, pgno);
                if (x <= dl[0].mid && dl[x].mid == pgno)
                {
                    if (mp != dl[x].mptr)
                    { /* bad cursor? */
                        mc->mc_flags &= ~(C_INITIALIZED | C_EOF);
                        txn->mt_flags |= MDB_TXN_ERROR;
                        return MDB_PROBLEM;
                    }
                    /* ok, it's ours */
                    loose = 1;
                }
            }
        }
        else
        {
            /* no parent txn, so it's just ours */
            loose = 1;
        }
    }
    if (loose)
    {
        DPRINTF(("loosen db %d page %" Yu, DDBI(mc), mp->mp_pgno));
        NEXT_LOOSE_PAGE(mp) = txn->mt_loose_pgs;
        txn->mt_loose_pgs = mp;
        txn->mt_loose_count++;
        mp->mp_flags |= P_LOOSE;
    }
    else
    {
        int rc = mdb_midl_append(&txn->mt_free_pgs, pgno);
        if (rc)
            return rc;
    }

    return MDB_SUCCESS;
}

// Set or clear P_KEEP in dirty, non-overflow, non-sub pages watched by txn.
// mc A cursor handle for the current operation.
// pflags Flags of the pages to update:
// P_DIRTY to set P_KEEP, P_DIRTY|P_KEEP to clear it.
// all No shortcuts. Needed except after a full #mdb_page_flush().
// 0 on success, non-zero on failure.
//
int mdb_pages_xkeep(MDB_cursor* mc, unsigned pflags, int all)
{
    enum
    {
        Mask = P_SUBP | P_DIRTY | P_LOOSE | P_KEEP
    };
    MDB_txn* txn = mc->mc_txn;
    MDB_cursor *m3, *m0 = mc;
    MDB_xcursor* mx;
    MDB_page *dp, *mp;
    MDB_node* leaf;
    unsigned i, j;
    int rc = MDB_SUCCESS, level;

    /* Mark pages seen by cursors: First m0, then tracked cursors */
    for (i = txn->mt_numdbs;;)
    {
        if (mc->mc_flags & C_INITIALIZED)
        {
            for (m3 = mc;; m3 = &mx->mx_cursor)
            {
                mp = NULL;
                for (j = 0; j < m3->mc_snum; j++)
                {
                    mp = m3->mc_pg[j];
                    if ((mp->mp_flags & Mask) == pflags)
                        mp->mp_flags ^= P_KEEP;
                }
                mx = m3->mc_xcursor;
                /* Proceed to mx if it is at a sub-database */
                if (!(mx && (mx->mx_cursor.mc_flags & C_INITIALIZED)))
                    break;
                if (!(mp && (mp->mp_flags & P_LEAF)))
                    break;
                leaf = NODEPTR(mp, m3->mc_ki[j - 1]);
                if (!(leaf->mn_flags & F_SUBDATA))
                    break;
            }
        }
        mc = mc->mc_next;
        for (; !mc || mc == m0; mc = txn->mt_cursors[--i])
            if (i == 0)
                goto mark_done;
    }

mark_done:
    if (all)
    {
        /* Mark dirty root pages */
        for (i = 0; i < txn->mt_numdbs; i++)
        {
            if (txn->mt_dbflags[i] & DB_DIRTY)
            {
                pgno_t pgno = txn->mt_dbs[i].md_root;
                if (pgno == P_INVALID)
                    continue;
                rc = mdb_page_get(m0, pgno, &dp, &level);
                if (rc != MDB_SUCCESS)
                    break;
                if ((dp->mp_flags & Mask) == pflags && level <= 1)
                    dp->mp_flags ^= P_KEEP;
            }
        }
    }

    return rc;
}

// Flush (some) dirty pages to the map, after clearing their dirty flag.
// txn the transaction that's being committed
// keep number of initial pages in dirty_list to keep dirty.
// 0 on success, non-zero on failure.
//
int mdb_page_flush(MDB_txn* txn, int keep)
{
    MDB_env* env = txn->mt_env;
    MDB_ID2L dl = txn->mt_u.dirty_list;
    unsigned psize = env->me_psize, j;
    int i, pagecount = dl[0].mid, rc;
    size_t size = 0;
    MDB_OFF_T pos = 0;
    pgno_t pgno = 0;
    MDB_page* dp = NULL;
#ifdef _WIN32
    OVERLAPPED* ov = env->ov;
    MDB_page* wdp;
    int async_i = 0;
    HANDLE fd = (env->me_flags & MDB_NOSYNC) ? env->me_fd : env->me_ovfd;
#else
    struct iovec iov[MDB_COMMIT_PAGES];
    HANDLE fd = env->me_fd;
#endif
    ssize_t wsize = 0, wres;
    MDB_OFF_T wpos = 0, next_pos = 1; /* impossible pos, so pos != next_pos */
    int n = 0;

    j = i = keep;
    if (env->me_flags & MDB_WRITEMAP
#ifdef _WIN32
        /* In windows, we still do writes to the file (with write-through enabled in sync mode),
         * as this is faster than FlushViewOfFile/FlushFileBuffers */
        && (env->me_flags & MDB_NOSYNC)
#endif
    )
    {
        /* Clear dirty flags */
        while (++i <= pagecount)
        {
            dp = (MDB_page*)dl[i].mptr;
            /* Don't flush this page yet */
            if (dp->mp_flags & (P_LOOSE | P_KEEP))
            {
                dp->mp_flags &= ~P_KEEP;
                dl[++j] = dl[i];
                continue;
            }
            dp->mp_flags &= ~P_DIRTY;
        }
        goto done;
    }

#ifdef _WIN32
    if (pagecount - keep >= env->ovs)
    {
        /* ran out of room in ov array, and re-malloc, copy handles and free previous */
        int ovs = (pagecount - keep) * 1.5; /* provide extra padding to reduce number of re-allocations */
        int new_size = ovs * sizeof(OVERLAPPED);
        ov = (OVERLAPPED*)malloc(new_size);
        if (ov == NULL)
            return ENOMEM;
        int previous_size = env->ovs * sizeof(OVERLAPPED);
        memcpy(ov, env->ov, previous_size); /* Copy previous OVERLAPPED data to retain event handles */
        /* And clear rest of memory */
        memset(&ov[env->ovs], 0, new_size - previous_size);
        if (env->ovs > 0)
        {
            free(env->ov); /* release previous allocation */
        }

        env->ov = ov;
        env->ovs = ovs;
    }
#endif

    /* Write the pages */
    for (;;)
    {
        if (++i <= pagecount)
        {
            dp = (MDB_page*)dl[i].mptr;
            /* Don't flush this page yet */
            if (dp->mp_flags & (P_LOOSE | P_KEEP))
            {
                dp->mp_flags &= ~P_KEEP;
                dl[i].mid = 0;
                continue;
            }
            pgno = dl[i].mid;
            /* clear dirty flag */
            dp->mp_flags &= ~P_DIRTY;
            pos = pgno * psize;
            size = psize;
            if (IS_OVERFLOW(dp))
                size *= dp->mp_pages;
        }
        /* Write up to MDB_COMMIT_PAGES dirty pages at a time. */
        if (pos != next_pos || n == MDB_COMMIT_PAGES ||
            wsize + size > MAX_WRITE
#ifdef _WIN32
            /* If writemap is enabled, consecutive page positions infer
             * contiguous (mapped) memory.
             * Otherwise force write pages one at a time.
             * Windows actually supports scatter/gather I/O, but only on
             * unbuffered file handles. Since we're relying on the OS page
             * cache for all our data, that's self-defeating. So we just
             * write pages one at a time. We use the ov structure to set
             * the write offset, to at least save the overhead of a Seek
             * system call.
             */
            || !(env->me_flags & MDB_WRITEMAP)
#endif
        )
        {
            if (n)
            {
            retry_write:
                /* Write previous page(s) */
                DPRINTF(("committing page %" Z "u", pgno));
#ifdef _WIN32
                OVERLAPPED* this_ov = &ov[async_i];
                /* Clear status, and keep hEvent, we reuse that */
                this_ov->Internal = 0;
                this_ov->Offset = wpos & 0xffffffff;
                this_ov->OffsetHigh = wpos >> 16 >> 16;
                if (!F_ISSET(env->me_flags, MDB_NOSYNC) && !this_ov->hEvent)
                {
                    HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);
                    if (!event)
                    {
                        rc = ErrCode();
                        DPRINTF(("CreateEvent: %s", strerror(rc)));
                        return rc;
                    }
                    this_ov->hEvent = event;
                }
                if (!WriteFile(fd, wdp, wsize, NULL, this_ov))
                {
                    rc = ErrCode();
                    if (rc != ERROR_IO_PENDING)
                    {
                        DPRINTF(("WriteFile: %d", rc));
                        return rc;
                    }
                }
                async_i++;
#else
#ifdef MDB_USE_PWRITEV
                wres = pwritev(fd, iov, n, wpos);
#else
                if (n == 1)
                {
                    wres = pwrite(fd, iov[0].iov_base, wsize, wpos);
                }
                else
                {
                retry_seek:
                    if (lseek(fd, wpos, SEEK_SET) == -1)
                    {
                        rc = ErrCode();
                        if (rc == EINTR)
                            goto retry_seek;
                        DPRINTF(("lseek: %s", strerror(rc)));
                        return rc;
                    }
                    wres = writev(fd, iov, n);
                }
#endif
                if (wres != wsize)
                {
                    if (wres < 0)
                    {
                        rc = ErrCode();
                        if (rc == EINTR)
                            goto retry_write;
                        DPRINTF(("Write error: %s", strerror(rc)));
                    }
                    else
                    {
                        rc = EIO; /* TODO: Use which error code? */
                        DPUTS("short write, filesystem full?");
                    }
                    return rc;
                }
#endif /* _WIN32 */
                n = 0;
            }
            if (i > pagecount)
                break;
            wpos = pos;
            wsize = 0;
#ifdef _WIN32
            wdp = dp;
        }
#else
        }
        iov[n].iov_len = size;
        iov[n].iov_base = (char*)dp;
#endif /* _WIN32 */
        DPRINTF(("committing page %" Yu, pgno));
        next_pos = pos + size;
        wsize += size;
        n++;
    }

#ifdef _WIN32
    if (!F_ISSET(env->me_flags, MDB_NOSYNC))
    {
        /* Now wait for all the asynchronous/overlapped sync/write-through writes to complete.
         * We start with the last one so that all the others should already be complete and
         * we reduce thread suspend/resuming (in practice, typically about 99.5% of writes are
         * done after the last write is done) */
        rc = 0;
        while (--async_i >= 0)
        {
            if (ov[async_i].hEvent)
            {
                DWORD temp_wres;
                if (!GetOverlappedResult(fd, &ov[async_i], &temp_wres, TRUE))
                {
                    rc = ErrCode(); /* Continue on so that all the event signals are reset */
                }
                wres = temp_wres;
            }
        }
        if (rc)
        { /* any error on GetOverlappedResult, exit now */
            return rc;
        }
    }
#endif /* _WIN32 */

    if (!(env->me_flags & MDB_WRITEMAP))
    {
        for (i = keep; ++i <= pagecount;)
        {
            dp = (MDB_page*)dl[i].mptr;
            /* This is a page we skipped above */
            if (!dl[i].mid)
            {
                dl[++j] = dl[i];
                dl[j].mid = dp->mp_pgno;
                continue;
            }
            mdb_dpage_free(env, dp);
        }
    }

done:
    i--;
    txn->mt_dirty_room += i - j;
    dl[0].mid = j;
    return MDB_SUCCESS;
}

// Spill pages from the dirty list back to disk.
// This is intended to prevent running into #MDB_TXN_FULL situations,
// but note that they may still occur in a few cases:
// 1) our estimate of the txn size could be too small. Currently this
// seems unlikely, except with a large number of #MDB_MULTIPLE items.
// 2) child txns may run out of space if their parents dirtied a
// lot of pages and never spilled them. TODO: we probably should do
// a preemptive spill during #mdb_txn_begin() of a child txn, if
// the parent's dirty_room is below a given threshold.
//
// Otherwise, if not using nested txns, it is expected that apps will
// not run into #MDB_TXN_FULL any more. The pages are flushed to disk
// the same way as for a txn commit, e.g. their P_DIRTY flag is cleared.
// If the txn never references them again, they can be left alone.
// If the txn only reads them, they can be used without any fuss.
// If the txn writes them again, they can be dirtied immediately without
// going thru all of the work of #mdb_page_touch(). Such references are
// handled by #mdb_page_unspill().
//
// Also note, we never spill DB root pages, nor pages of active cursors,
// because we'll need these back again soon anyway. And in nested txns,
// we can't spill a page in a child txn if it was already spilled in a
// parent txn. That would alter the parent txns' data even though
// the child hasn't committed yet, and we'd have no way to undo it if
// the child aborted.
//
// m0 cursor A cursor handle identifying the transaction and
// database for which we are checking space.
// key For a put operation, the key being stored.
// data For a put operation, the data being stored.
// 0 on success, non-zero on failure.
/** Back up parent txn's cursors, then grab the originals for tracking */
int mdb_page_spill(MDB_cursor* m0, MDB_val* key, MDB_val* data)
{
    MDB_txn* txn = m0->mc_txn;
    MDB_page* dp;
    MDB_ID2L dl = txn->mt_u.dirty_list;
    unsigned int i, j, need;
    int rc;

    if (m0->mc_flags & C_SUB)
        return MDB_SUCCESS;

    /* Estimate how much space this op will take */
    i = m0->mc_db->md_depth;
    /* Named DBs also dirty the main DB */
    if (m0->mc_dbi >= CORE_DBS)
        i += txn->mt_dbs[MAIN_DBI].md_depth;
    /* For puts, roughly factor in the key+data size */
    if (key)
        i += (LEAFSIZE(key, data) + txn->mt_env->me_psize) / txn->mt_env->me_psize;
    i += i; /* double it for good measure */
    need = i;

    if (txn->mt_dirty_room > i)
        return MDB_SUCCESS;

    if (!txn->mt_spill_pgs)
    {
        txn->mt_spill_pgs = mdb_midl_alloc(MDB_IDL_UM_MAX);
        if (!txn->mt_spill_pgs)
            return ENOMEM;
    }
    else
    {
        /* purge deleted slots */
        MDB_IDL sl = txn->mt_spill_pgs;
        unsigned int num = sl[0];
        j = 0;
        for (i = 1; i <= num; i++)
        {
            if (!(sl[i] & 1))
                sl[++j] = sl[i];
        }
        sl[0] = j;
    }

    /* Preserve pages which may soon be dirtied again */
    rc = mdb_pages_xkeep(m0, P_DIRTY, 1);
    if (rc != MDB_SUCCESS)
        goto done;

    /* Less aggressive spill - we originally spilled the entire dirty list,
     * with a few exceptions for cursor pages and DB root pages. But this
     * turns out to be a lot of wasted effort because in a large txn many
     * of those pages will need to be used again. So now we spill only 1/8th
     * of the dirty pages. Testing revealed this to be a good tradeoff,
     * better than 1/2, 1/4, or 1/10.
     */
    if (need < MDB_IDL_UM_MAX / 8)
        need = MDB_IDL_UM_MAX / 8;

    /* Save the page IDs of all the pages we're flushing */
    /* flush from the tail forward, this saves a lot of shifting later on. */
    for (i = dl[0].mid; i && need; i--)
    {
        MDB_ID pn = dl[i].mid << 1;
        dp = (MDB_page*)dl[i].mptr;
        if (dp->mp_flags & (P_LOOSE | P_KEEP))
            continue;
        /* Can't spill twice, make sure it's not already in a parent's
         * spill list.
         */
        if (txn->mt_parent)
        {
            MDB_txn* tx2;
            for (tx2 = txn->mt_parent; tx2; tx2 = tx2->mt_parent)
            {
                if (tx2->mt_spill_pgs)
                {
                    j = mdb_midl_search(tx2->mt_spill_pgs, pn);
                    if (j <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[j] == pn)
                    {
                        dp->mp_flags |= P_KEEP;
                        break;
                    }
                }
            }
            if (tx2)
                continue;
        }
        rc = mdb_midl_append(&txn->mt_spill_pgs, pn);
        if (rc)
            goto done;
        need--;
    }
    mdb_midl_sort(txn->mt_spill_pgs);

    /* Flush the spilled part of dirty list */
    rc = mdb_page_flush(txn, i);
    if (rc != MDB_SUCCESS)
        goto done;

    /* Reset any dirty pages we kept that page_flush didn't see */
    rc = mdb_pages_xkeep(m0, P_DIRTY | P_KEEP, i);

done:
    txn->mt_flags |= rc ? MDB_TXN_ERROR : MDB_TXN_SPILLS;
    return rc;
}

// Find oldest txnid still referenced. Expects txn->mt_txnid > 0.
txnid_t mdb_find_oldest(MDB_txn* txn)
{
    int i;
    txnid_t mr, oldest = txn->mt_txnid - 1;
    if (txn->mt_env->me_txns)
    {
        MDB_reader* r = txn->mt_env->me_txns->mti_readers;
        for (i = txn->mt_env->me_txns->mti_numreaders; --i >= 0;)
        {
            if (r[i].mr_pid)
            {
                mr = r[i].mr_txnid;
                if (oldest > mr)
                    oldest = mr;
            }
        }
    }
    return oldest;
}

// Add a page to the txn's dirty list
void mdb_page_dirty(MDB_txn* txn, MDB_page* mp)
{
    MDB_ID2 mid;
    int rc, (*insert)(MDB_ID2L, MDB_ID2*);
#ifdef _WIN32 /* With Windows we always write dirty pages with WriteFile,                                              \
               * so we always want them ordered */
    insert = mdb_mid2l_insert;
#else /* but otherwise with writemaps, we just use msync, we                                                           \
       * don't need the ordering and just append */
    if (txn->mt_flags & MDB_TXN_WRITEMAP)
        insert = mdb_mid2l_append;
    else
        insert = mdb_mid2l_insert;
#endif
    mid.mid = mp->mp_pgno;
    mid.mptr = mp;
    rc = insert(txn->mt_u.dirty_list, &mid);
    mdb_tassert(txn, rc == 0);
    txn->mt_dirty_room--;
}

// Allocate page numbers and memory for writing.  Maintain me_pglast,
// me_pghead and mt_next_pgno.  Set #MDB_TXN_ERROR on failure.int
// If there are free pages available from older transactions, they
// are re-used first. Otherwise allocate a new page at mt_next_pgno.
// Do not modify the freedB, just merge freeDB records into me_pghead[]
// and move me_pglast to say which records were consumed.  Only this
// function can create me_pghead and move me_pglast/mt_next_pgno.
// mc cursor A cursor handle identifying the transaction and
// database for which we are allocating.
// num the number of pages to allocate.
// mp Address of the allocated page(s). Requests for multiple pages
// will always be satisfied by a single contiguous chunk of memory.
// 0 on success, non-zero on failure.
//
int mdb_page_alloc(MDB_cursor* mc, int num, MDB_page** mp)
{
#ifdef MDB_PARANOID  // Seems like we can ignore this now
    // Get at most <Max_retries> more freeDB records once me_pghead
    // has enough pages.  If not enough, use new pages from the map.
    // If <Paranoid> and mc is updating the freeDB, only get new
    // records if me_pghead is empty. Then the freelist cannot play
    // catch-up with itself by growing while trying to save it.
    enum
    {
        Paranoid = 1,
        Max_retries = 500
    };
#else
    enum
    {
        Paranoid = 0,
        Max_retries = INT_MAX /*infinite*/
    };
#endif
    int rc, retry = num * 60;
    MDB_txn* txn = mc->mc_txn;
    MDB_env* env = txn->mt_env;
    pgno_t pgno, *mop = env->me_pghead;
    unsigned i, j, mop_len = mop ? mop[0] : 0, n2 = num - 1;
    MDB_page* np;
    txnid_t oldest = 0, last;
    MDB_cursor_op op;
    MDB_cursor m2;
    int found_old = 0;

    /* If there are any loose pages, just use them */
    if (num == 1 && txn->mt_loose_pgs)
    {
        np = txn->mt_loose_pgs;
        txn->mt_loose_pgs = NEXT_LOOSE_PAGE(np);
        txn->mt_loose_count--;
        DPRINTF(("db %d use loose page %" Yu, DDBI(mc), np->mp_pgno));
        *mp = np;
        return MDB_SUCCESS;
    }

    *mp = NULL;

    /* If our dirty list is already full, we can't do anything */
    if (txn->mt_dirty_room == 0)
    {
        rc = MDB_TXN_FULL;
        goto fail;
    }

    for (op = MDB_FIRST;; op = MDB_NEXT)
    {
        MDB_val key, data;
        MDB_node* leaf;
        pgno_t* idl;

        /* Seek a big enough contiguous page range. Prefer
         * pages at the tail, just truncating the list.
         */
        if (mop_len > n2)
        {
            i = mop_len;
            do
            {
                pgno = mop[i];
                if (mop[i - n2] == pgno + n2)
                    goto search_done;
            } while (--i > n2);
            if (--retry < 0)
                break;
        }

        if (op == MDB_FIRST)
        { /* 1st iteration */
            /* Prepare to fetch more and coalesce */
            last = env->me_pglast;
            oldest = env->me_pgoldest;
            mdb_cursor_init(&m2, txn, FREE_DBI, NULL);
            if (last)
            {
                op = MDB_SET_RANGE;
                key.mv_data = &last; /* will look up last+1 */
                key.mv_size = sizeof(last);
            }
            if (Paranoid && mc->mc_dbi == FREE_DBI)
                retry = -1;
        }
        if (Paranoid && retry < 0 && mop_len)
            break;

        last++;
        /* Do not fetch more if the record will be too recent */
        if (oldest <= last)
        {
            if (!found_old)
            {
                oldest = mdb_find_oldest(txn);
                env->me_pgoldest = oldest;
                found_old = 1;
            }
            if (oldest <= last)
                break;
        }
        rc = mdb_cursor_get(&m2, &key, NULL, op);
        if (rc)
        {
            if (rc == MDB_NOTFOUND)
                break;
            goto fail;
        }
        last = *(txnid_t*)key.mv_data;
        if (oldest <= last)
        {
            if (!found_old)
            {
                oldest = mdb_find_oldest(txn);
                env->me_pgoldest = oldest;
                found_old = 1;
            }
            if (oldest <= last)
                break;
        }
        np = m2.mc_pg[m2.mc_top];
        leaf = NODEPTR(np, m2.mc_ki[m2.mc_top]);
        rc = mdb_node_read(&m2, leaf, &data);
        if (rc != MDB_SUCCESS)
            goto fail;

        idl = (MDB_ID*)data.mv_data;
        i = idl[0];
        if (!mop)
        {
            mop = mdb_midl_alloc(i);
            env->me_pghead = mop;
            if (!mop)
            {
                rc = ENOMEM;
                goto fail;
            }
        }
        else
        {
            rc = mdb_midl_need(&env->me_pghead, i);
            if (rc != 0)
                goto fail;
            mop = env->me_pghead;
        }
        env->me_pglast = last;
#if (MDB_DEBUG) > 1
        DPRINTF(("IDL read txn %" Yu " root %" Yu " num %u", last, txn->mt_dbs[FREE_DBI].md_root, i));
        for (j = i; j; j--)
            DPRINTF(("IDL %" Yu, idl[j]));
#endif
        /* Merge in descending sorted order */
        mdb_midl_xmerge(mop, idl);
        mop_len = mop[0];
    }

    /* Use new pages from the map when nothing suitable in the freeDB */
    i = 0;
    pgno = txn->mt_next_pgno;
    if (pgno + num >= env->me_maxpg)
    {
        DPUTS("DB size maxed out");
        rc = MDB_MAP_FULL;
        goto fail;
    }
#if defined(_WIN32)
    if (!(env->me_flags & MDB_RDONLY))
    {
        void* p;
        p = (MDB_page*)(env->me_map + env->me_psize * pgno);
        p = VirtualAlloc(p,
                         static_cast<SIZE_T>(env->me_psize) * num,
                         MEM_COMMIT,
                         (env->me_flags & MDB_WRITEMAP) ? PAGE_READWRITE : PAGE_READONLY);
        if (!p)
        {
            DPUTS("VirtualAlloc failed");
            rc = ErrCode();
            goto fail;
        }
    }
#endif

search_done:
    if (env->me_flags & MDB_WRITEMAP)
    {
        np = (MDB_page*)(env->me_map + env->me_psize * pgno);
    }
    else
    {
        np = mdb_page_malloc(txn, num);
        if (!np)
        {
            rc = ENOMEM;
            goto fail;
        }
    }
    if (i)
    {
        mop[0] = mop_len -= num;
        /* Move any stragglers down */
        for (j = i - num; j < mop_len;)
            mop[++j] = mop[++i];
    }
    else
    {
        txn->mt_next_pgno = pgno + num;
    }
    np->mp_pgno = pgno;
    mdb_page_dirty(txn, np);
    *mp = np;

    return MDB_SUCCESS;

fail:
    txn->mt_flags |= MDB_TXN_ERROR;
    return rc;
}

// Copy the used portions of a non-overflow page.
// dst page to copy into
// src page to copy from
// psize size of a page
//
void mdb_page_copy(MDB_page* dst, MDB_page* src, unsigned int psize)
{
    enum
    {
        Align = sizeof(pgno_t)
    };
    indx_t upper = src->mp_upper, lower = src->mp_lower, unused = upper - lower;

    /* If page isn't full, just copy the used portion. Adjust
     * alignment so memcpy may copy words instead of bytes.
     */
    unused &= -Align;
    if (unused && !IS_LEAF2(src))
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

// Pull a page off the txn's spill list, if present.
// If a page being referenced was spilled to disk in this txn, bring
// it back and make it dirty/writable again.
// txn the transaction handle.
// mp the page being referenced. It must not be dirty.
// ret the writable page, if any. ret is unchanged if
// mp wasn't spilled.
//
int mdb_page_unspill(MDB_txn* txn, MDB_page* mp, MDB_page** ret)
{
    MDB_env* env = txn->mt_env;
    const MDB_txn* tx2;
    unsigned x;
    pgno_t pgno = mp->mp_pgno, pn = pgno << 1;

    for (tx2 = txn; tx2; tx2 = tx2->mt_parent)
    {
        if (!tx2->mt_spill_pgs)
            continue;
        x = mdb_midl_search(tx2->mt_spill_pgs, pn);
        if (x <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[x] == pn)
        {
            MDB_page* np;
            int num;
            if (txn->mt_dirty_room == 0)
                return MDB_TXN_FULL;
            if (IS_OVERFLOW(mp))
                num = mp->mp_pages;
            else
                num = 1;
            if (env->me_flags & MDB_WRITEMAP)
            {
                np = mp;
            }
            else
            {
                np = mdb_page_malloc(txn, num);
                if (!np)
                    return ENOMEM;
                if (num > 1)
                    memcpy(np, mp, static_cast<size_t>(num) * env->me_psize);
                else
                    mdb_page_copy(np, mp, env->me_psize);
            }
            if (tx2 == txn)
            {
                /* If in current txn, this page is no longer spilled.
                 * If it happens to be the last page, truncate the spill list.
                 * Otherwise mark it as deleted by setting the LSB.
                 */
                if (x == txn->mt_spill_pgs[0])
                    txn->mt_spill_pgs[0]--;
                else
                    txn->mt_spill_pgs[x] |= 1;
            } /* otherwise, if belonging to a parent txn, the
               * page remains spilled until child commits
               */

            mdb_page_dirty(txn, np);
            np->mp_flags |= P_DIRTY;
            *ret = np;
            break;
        }
    }
    return MDB_SUCCESS;
}

// Touch a page: make it dirty and re-insert into tree with updated pgno.
// Set #MDB_TXN_ERROR on failure.
// mc cursor pointing to the page to be touched
// 0 on success, non-zero on failure.
//
int mdb_page_touch(MDB_cursor* mc)
{
    MDB_page *mp = mc->mc_pg[mc->mc_top], *np;
    MDB_txn* txn = mc->mc_txn;
    MDB_cursor *m2, *m3;
    pgno_t pgno;
    int rc;

    if (!F_ISSET(MP_FLAGS(mp), P_DIRTY))
    {
        if (txn->mt_flags & MDB_TXN_SPILLS)
        {
            np = NULL;
            rc = mdb_page_unspill(txn, mp, &np);
            if (rc)
                goto fail;
            if (np)
                goto done;
        }
        rc = mdb_midl_need(&txn->mt_free_pgs, 1);
        if (!rc)
            rc = mdb_page_alloc(mc, 1, &np);
        if (rc)
            goto fail;
        pgno = np->mp_pgno;
        DPRINTF(("touched db %d page %" Yu " -> %" Yu, DDBI(mc), mp->mp_pgno, pgno));
        mdb_cassert(mc, mp->mp_pgno != pgno);
        mdb_midl_xappend(txn->mt_free_pgs, mp->mp_pgno);
        /* Update the parent page, if any, to point to the new page */
        if (mc->mc_top)
        {
            MDB_page* parent = mc->mc_pg[mc->mc_top - 1];
            MDB_node* node = NODEPTR(parent, mc->mc_ki[mc->mc_top - 1]);
            SETPGNO(node, pgno);
        }
        else
        {
            mc->mc_db->md_root = pgno;
        }
    }
    else if (txn->mt_parent && !IS_SUBP(mp))
    {
        MDB_ID2 mid, *dl = txn->mt_u.dirty_list;
        pgno = mp->mp_pgno;
        /* If txn has a parent, make sure the page is in our
         * dirty list.
         */
        if (dl[0].mid)
        {
            unsigned x = mdb_mid2l_search(dl, pgno);
            if (x <= dl[0].mid && dl[x].mid == pgno)
            {
                if (mp != dl[x].mptr)
                { /* bad cursor? */
                    mc->mc_flags &= ~(C_INITIALIZED | C_EOF);
                    txn->mt_flags |= MDB_TXN_ERROR;
                    return MDB_PROBLEM;
                }
                return 0;
            }
        }
        mdb_cassert(mc, dl[0].mid < MDB_IDL_UM_MAX);
        /* No - copy it */
        np = mdb_page_malloc(txn, 1);
        if (!np)
            return ENOMEM;
        mid.mid = pgno;
        mid.mptr = np;
        rc = mdb_mid2l_insert(dl, &mid);
        mdb_cassert(mc, rc == 0);
    }
    else
    {
        return 0;
    }

    mdb_page_copy(np, mp, txn->mt_env->me_psize);
    np->mp_pgno = pgno;
    np->mp_flags |= P_DIRTY;

done:
    /* Adjust cursors pointing to mp */
    mc->mc_pg[mc->mc_top] = np;
    m2 = txn->mt_cursors[mc->mc_dbi];
    if (mc->mc_flags & C_SUB)
    {
        for (; m2; m2 = m2->mc_next)
        {
            m3 = &m2->mc_xcursor->mx_cursor;
            if (m3->mc_snum < mc->mc_snum)
                continue;
            if (m3->mc_pg[mc->mc_top] == mp)
                m3->mc_pg[mc->mc_top] = np;
        }
    }
    else
    {
        for (; m2; m2 = m2->mc_next)
        {
            if (m2->mc_snum < mc->mc_snum)
                continue;
            if (m2 == mc)
                continue;
            if (m2->mc_pg[mc->mc_top] == mp)
            {
                m2->mc_pg[mc->mc_top] = np;
                if (IS_LEAF(np))
                    XCURSOR_REFRESH(m2, mc->mc_top, np);
            }
        }
    }
    return 0;

fail:
    txn->mt_flags |= MDB_TXN_ERROR;
    return rc;
}

// Find the address of the page corresponding to a given page number.
// Set #MDB_TXN_ERROR on failure.
// mc the cursor accessing the page.
// pgno the page number for the page to retrieve.
// ret address of a pointer where the page's address will be stored.
// lvl dirty_list inheritance level of found page. 1=current txn, 0=mapped page.
// 0 on success, non-zero on failure.
//
int mdb_page_get(MDB_cursor* mc, pgno_t pgno, MDB_page** ret, int* lvl)
{
    MDB_txn* txn = mc->mc_txn;
    MDB_page* p = NULL;
    int level;

    if (!(mc->mc_flags & (C_ORIG_RDONLY | C_WRITEMAP)))
    {
        MDB_txn* tx2 = txn;
        level = 1;
        do
        {
            MDB_ID2L dl = tx2->mt_u.dirty_list;
            unsigned x;
            /* Spilled pages were dirtied in this txn and flushed
             * because the dirty list got full. Bring this page
             * back in from the map (but don't unspill it here,
             * leave that unless page_touch happens again).
             */
            if (tx2->mt_spill_pgs)
            {
                MDB_ID pn = pgno << 1;
                x = mdb_midl_search(tx2->mt_spill_pgs, pn);
                if (x <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[x] == pn)
                {
                    goto mapped;
                }
            }
            if (dl[0].mid)
            {
                unsigned x = mdb_mid2l_search(dl, pgno);
                if (x <= dl[0].mid && dl[x].mid == pgno)
                {
                    p = (MDB_page*)dl[x].mptr;
                    goto done;
                }
            }
            level++;
        } while ((tx2 = tx2->mt_parent) != NULL);
    }

    if (pgno >= txn->mt_next_pgno)
    {
        DPRINTF(("page %" Yu " not found", pgno));
        txn->mt_flags |= MDB_TXN_ERROR;
        return MDB_PAGE_NOTFOUND;
    }

    level = 0;

mapped:
{
    MDB_env* env = txn->mt_env;
    p = (MDB_page*)(env->me_map + env->me_psize * pgno);
}

done:
    *ret = p;
    if (lvl)
        *lvl = level;
    return MDB_SUCCESS;
}

// Finish #mdb_page_search() / #mdb_page_search_lowest().
// The cursor is at the root page, set up the rest of it.
//
int mdb_page_search_root(MDB_cursor* mc, MDB_val* key, int flags)
{
    MDB_page* mp = mc->mc_pg[mc->mc_top];
    int rc;
    DKBUF;

    while (IS_BRANCH(mp))
    {
        MDB_node* node;
        indx_t i;

        DPRINTF(("branch page %" Yu " has %u keys", mp->mp_pgno, NUMKEYS(mp)));
        /* Don't assert on branch pages in the FreeDB. We can get here
         * while in the process of rebalancing a FreeDB branch page; we must
         * let that proceed. ITS#8336
         */
        mdb_cassert(mc, !mc->mc_dbi || NUMKEYS(mp) > 1);
        DPRINTF(("found index 0 to page %" Yu, NODEPGNO(NODEPTR(mp, 0))));

        if (flags & (MDB_PS_FIRST | MDB_PS_LAST))
        {
            i = 0;
            if (flags & MDB_PS_LAST)
            {
                i = NUMKEYS(mp) - 1;
                /* if already init'd, see if we're already in right place */
                if (mc->mc_flags & C_INITIALIZED)
                {
                    if (mc->mc_ki[mc->mc_top] == i)
                    {
                        mc->mc_top = mc->mc_snum++;
                        mp = mc->mc_pg[mc->mc_top];
                        goto ready;
                    }
                }
            }
        }
        else
        {
            int exact;
            node = mdb_node_search(mc, key, &exact);
            if (node == NULL)
                i = NUMKEYS(mp) - 1;
            else
            {
                i = mc->mc_ki[mc->mc_top];
                if (!exact)
                {
                    mdb_cassert(mc, i > 0);
                    i--;
                }
            }
            DPRINTF(("following index %u for key [%s]", i, DKEY(key)));
        }

        mdb_cassert(mc, i < NUMKEYS(mp));
        node = NODEPTR(mp, i);

        rc = mdb_page_get(mc, NODEPGNO(node), &mp, NULL);
        if (rc != 0)
            return rc;

        mc->mc_ki[mc->mc_top] = i;
        rc = mdb_cursor_push(mc, mp);
        if (rc)
            return rc;

    ready:
        if (flags & MDB_PS_MODIFY)
        {
            rc = mdb_page_touch(mc);
            if (rc != 0)
                return rc;
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

// Search for the lowest key under the current branch page.
// This just bypasses a NUMKEYS check in the current page
// before calling mdb_page_search_root(), because the callers
// are all in situations where the current page is known to
// be underfilled.
//
int mdb_page_search_lowest(MDB_cursor* mc)
{
    MDB_page* mp = mc->mc_pg[mc->mc_top];
    MDB_node* node = NODEPTR(mp, 0);
    int rc;

    rc = mdb_page_get(mc, NODEPGNO(node), &mp, NULL);
    if (rc != 0)
        return rc;

    mc->mc_ki[mc->mc_top] = 0;
    rc = mdb_cursor_push(mc, mp);
    if (rc)
        return rc;
    return mdb_page_search_root(mc, NULL, MDB_PS_FIRST);
}

// Search for the lowest key under the current branch page.
// This just bypasses a NUMKEYS check in the current page
// before calling mdb_page_search_root(), because the callers
// are all in situations where the current page is known to
// be underfilled.
//
int mdb_page_search(MDB_cursor* mc, MDB_val* key, int flags)
{
    int rc;
    pgno_t root;

    /* Make sure the txn is still viable, then find the root from
     * the txn's db table and set it as the root of the cursor's stack.
     */
    if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED)
    {
        DPUTS("transaction may not be used now");
        return MDB_BAD_TXN;
    }
    else
    {
        /* Make sure we're using an up-to-date root */
        if (*mc->mc_dbflag & DB_STALE)
        {
            MDB_cursor mc2;
            if (TXN_DBI_CHANGED(mc->mc_txn, mc->mc_dbi))
                return MDB_BAD_DBI;
            mdb_cursor_init(&mc2, mc->mc_txn, MAIN_DBI, NULL);
            rc = mdb_page_search(&mc2, &mc->mc_dbx->md_name, 0);
            if (rc)
                return rc;
            {
                MDB_val data;
                int exact = 0;
                uint16_t flags;
                MDB_node* leaf = mdb_node_search(&mc2, &mc->mc_dbx->md_name, &exact);
                if (!exact)
                    return MDB_BAD_DBI;
                if ((leaf->mn_flags & (F_DUPDATA | F_SUBDATA)) != F_SUBDATA)
                    return MDB_INCOMPATIBLE; /* not a named DB */
                rc = mdb_node_read(&mc2, leaf, &data);
                if (rc)
                    return rc;
                memcpy(&flags, ((char*)data.mv_data + offsetof(MDB_db, md_flags)), sizeof(uint16_t));
                /* The txn may not know this DBI, or another process may
                 * have dropped and recreated the DB with other flags.
                 */
                if ((mc->mc_db->md_flags & PERSISTENT_FLAGS) != flags)
                    return MDB_INCOMPATIBLE;
                memcpy(mc->mc_db, data.mv_data, sizeof(MDB_db));
            }
            *mc->mc_dbflag &= ~DB_STALE;
        }
        root = mc->mc_db->md_root;

        if (root == P_INVALID)
        { /* Tree is empty. */
            DPUTS("tree is empty");
            return MDB_NOTFOUND;
        }
    }

    mdb_cassert(mc, root > 1);
    if (!mc->mc_pg[0] || mc->mc_pg[0]->mp_pgno != root)
    {
        rc = mdb_page_get(mc, root, &mc->mc_pg[0], NULL);
        if (rc != 0)
            return rc;
    }

    mc->mc_snum = 1;
    mc->mc_top = 0;

    DPRINTF(("db %d root page %" Yu " has flags 0x%X", DDBI(mc), root, mc->mc_pg[0]->mp_flags));

    if (flags & MDB_PS_MODIFY)
    {
        rc = mdb_page_touch(mc);
        if (rc)
            return rc;
    }

    if (flags & MDB_PS_ROOTONLY)
        return MDB_SUCCESS;

    return mdb_page_search_root(mc, key, flags);
}

int mdb_ovpage_free(MDB_cursor* mc, MDB_page* mp)
{
    MDB_txn* txn = mc->mc_txn;
    pgno_t pg = mp->mp_pgno;
    unsigned x = 0, ovpages = mp->mp_pages;
    MDB_env* env = txn->mt_env;
    MDB_IDL sl = txn->mt_spill_pgs;
    MDB_ID pn = pg << 1;
    int rc;

    DPRINTF(("free ov page %" Yu " (%d)", pg, ovpages));
    // If the page is dirty or on the spill list we just acquired it,
    // so we should give it back to our current free list, if any.
    // Otherwise put it onto the list of pages we freed in this txn.
    //
    // Won't create me_pghead: me_pglast must be inited along with it.
    // Unsupported in nested txns: They would need to hide the page
    // range in ancestor txns' dirty and spilled lists.
    //
    bool spill_condition = false;
    if (sl)
    {
        x = mdb_midl_search(sl, pn);
        spill_condition = x <= sl[0] && sl[x] == pn;
    }
    if (env->me_pghead && !txn->mt_parent && ((mp->mp_flags & P_DIRTY) || (sl && spill_condition)))
    {
        unsigned i, j;
        pgno_t* mop;
        MDB_ID2 *dl, ix, iy;
        rc = mdb_midl_need(&env->me_pghead, ovpages);
        if (rc)
            return rc;
        if (!(mp->mp_flags & P_DIRTY))
        {
            /* This page is no longer spilled */
            if (x == sl[0])
                sl[0]--;
            else
                sl[x] |= 1;
            goto release;
        }
        /* Remove from dirty list */
        dl = txn->mt_u.dirty_list;
        x = dl[0].mid--;
        for (ix = dl[x]; ix.mptr != mp; ix = iy)
        {
            if (x > 1)
            {
                x--;
                iy = dl[x];
                dl[x] = ix;
            }
            else
            {
                mdb_cassert(mc, x > 1);
                j = ++(dl[0].mid);
                dl[j] = ix; /* Unsorted. OK when MDB_TXN_ERROR. */
                txn->mt_flags |= MDB_TXN_ERROR;
                return MDB_PROBLEM;
            }
        }
        txn->mt_dirty_room++;
        if (!(env->me_flags & MDB_WRITEMAP))
            mdb_dpage_free(env, mp);
    release:
        /* Insert in me_pghead */
        mop = env->me_pghead;
        j = mop[0] + ovpages;
        for (i = mop[0]; i && mop[i] < pg; i--)
            mop[j--] = mop[i];
        while (j > i)
            mop[j--] = pg++;
        mop[0] += ovpages;
    }
    else
    {
        rc = mdb_midl_append_range(&txn->mt_free_pgs, pg, ovpages);
        if (rc)
            return rc;
    }
    mc->mc_db->md_overflow_pages -= ovpages;
    return 0;
}

// Allocate and initialize new pages for a database.
// Set #MDB_TXN_ERROR on failure.
// mc a cursor on the database being added to.
// flags flags defining what type of page is being allocated.
// num the number of pages to allocate. This is usually 1,
// unless allocating overflow pages for a large record.
// mp Address of a page, or NULL on failure.
// 0 on success, non-zero on failure.
//
int mdb_page_new(MDB_cursor* mc, uint32_t flags, int num, MDB_page** mp)
{
    MDB_page* np;
    int rc;

    rc = mdb_page_alloc(mc, num, &np);
    if (rc)
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

// Calculate the size of a leaf node.
// The size depends on the environment's page size; if a data item
// is too large it will be put onto an overflow page and the node
// size will only include the key and not the data. Sizes are always
// rounded up to an even number of bytes, to guarantee 2-byte alignment
// of the #MDB_node headers.
// env The environment handle.
// key The key for the node.
// data The data for the node.
// The number of bytes needed to store the node.
//
size_t mdb_leaf_size(MDB_env* env, MDB_val* key, MDB_val* data)
{
    size_t sz;

    sz = LEAFSIZE(key, data);
    if (sz > env->me_nodemax)
    {
        /* put on overflow page */
        sz -= data->mv_size - sizeof(pgno_t);
    }

    return EVEN(sz + sizeof(indx_t));
}

// Calculate the size of a branch node.
// The size should depend on the environment's page size but since
// we currently don't support spilling large keys onto overflow
// pages, it's simply the size of the #MDB_node header plus the
// size of the key. Sizes are always rounded up to an even number
// of bytes, to guarantee 2-byte alignment of the #MDB_node headers.
// env The environment handle.
// key The key for the node.
// The number of bytes needed to store the node.
//
size_t mdb_branch_size(MDB_env* env, MDB_val* key)
{
    size_t sz;

    sz = INDXSIZE(key);
    if (sz > env->me_nodemax)
    {
        /* put on overflow page */
        /* not implemented */
        /* sz -= key->size - sizeof(pgno_t); */
    }

    return sz + sizeof(indx_t);
}

// Add a node to the page pointed to by the cursor.
// Set #MDB_TXN_ERROR on failure.
// mc The cursor for this operation.
// indx The index on the page where the new node should be added.
// key The key for the new node.
// data The data for the new node, if any.
// pgno The page number, if adding a branch node.
// flags Flags for the node.
// 0 on success, non-zero on failure. Possible errors are:
// ENOMEM - failed to allocate overflow pages for the node.
// MDB_PAGE_FULL - there is insufficient room in the page. This error
// should never happen since all callers already calculate the
// page's free space before calling this function.
//
int mdb_node_add(MDB_cursor* mc, indx_t indx, MDB_val* key, MDB_val* data, pgno_t pgno, unsigned int flags)
{
    unsigned int i;
    size_t node_size = NODESIZE;
    ssize_t room;
    indx_t ofs;
    MDB_node* node;
    MDB_page* mp = mc->mc_pg[mc->mc_top];
    MDB_page* ofp = NULL; /* overflow page */
    void* ndata;
    DKBUF;

    mdb_cassert(mc, MP_UPPER(mp) >= MP_LOWER(mp));

    DPRINTF(("add to %s %spage %" Yu " index %i, data size %" Z "u key size %" Z "u [%s]",
             IS_LEAF(mp) ? "leaf" : "branch",
             IS_SUBP(mp) ? "sub-" : "",
             mdb_dbg_pgno(mp),
             indx,
             data ? data->mv_size : 0,
             key ? key->mv_size : 0,
             key ? DKEY(key) : "null"));

    if (IS_LEAF2(mp))
    {
        /* Move higher keys up one slot. */
        int ksize = mc->mc_db->md_pad, dif;
        char* ptr = LEAF2KEY(mp, indx, static_cast<size_t>(ksize));
        dif = NUMKEYS(mp) - indx;
        if (dif > 0)
            memmove(ptr + ksize, ptr, static_cast<size_t>(dif) * ksize);
        /* insert new key */
        memcpy(ptr, key->mv_data, ksize);

        /* Just using these for counting */
        MP_LOWER(mp) += sizeof(indx_t);
        MP_UPPER(mp) -= ksize - sizeof(indx_t);
        return MDB_SUCCESS;
    }

    room = (ssize_t)SIZELEFT(mp) - (ssize_t)sizeof(indx_t);
    if (key != NULL)
        node_size += key->mv_size;
    if (IS_LEAF(mp))
    {
        mdb_cassert(mc, key && data);
        if (F_ISSET(flags, F_BIGDATA))
        {
            /* Data already on overflow page. */
            node_size += sizeof(pgno_t);
        }
        else if (node_size + data->mv_size > mc->mc_txn->mt_env->me_nodemax)
        {
            int ovpages = OVPAGES(data->mv_size, mc->mc_txn->mt_env->me_psize);
            int rc;
            /* Put data on overflow page. */
            DPRINTF(("data size is %" Z "u, node would be %" Z "u, put data on overflow page",
                     data->mv_size,
                     node_size + data->mv_size));
            node_size = EVEN(node_size + sizeof(pgno_t));
            if ((ssize_t)node_size > room)
                goto full;
            rc = mdb_page_new(mc, P_OVERFLOW, ovpages, &ofp);
            if (rc)
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
    if ((ssize_t)node_size > room)
        goto full;

update:
    /* Move higher pointers up one slot. */
    for (i = NUMKEYS(mp); i > indx; i--)
        MP_PTRS(mp)[i] = MP_PTRS(mp)[i - 1];

    /* Adjust free space offsets. */
    ofs = MP_UPPER(mp) - node_size;
    mdb_cassert(mc, ofs >= MP_LOWER(mp) + sizeof(indx_t));
    MP_PTRS(mp)[indx] = ofs;
    MP_UPPER(mp) = ofs;
    MP_LOWER(mp) += sizeof(indx_t);

    /* Write the node data. */
    node = NODEPTR(mp, indx);
    node->mn_ksize = (key == NULL) ? 0 : key->mv_size;
    node->mn_flags = flags;
    if (IS_LEAF(mp))
        SETDSZ(node, data->mv_size);
    else
        SETPGNO(node, pgno);

    if (key)
        memcpy(NODEKEY(node), key->mv_data, key->mv_size);

    if (IS_LEAF(mp))
    {
        ndata = NODEDATA(node);
        if (ofp == NULL)
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
    DPRINTF(("not enough room in page %" Yu ", got %u ptrs", mdb_dbg_pgno(mp), NUMKEYS(mp)));
    DPRINTF(("upper-lower = %u - %u = %" Z "d", MP_UPPER(mp), MP_LOWER(mp), room));
    DPRINTF(("node size = %" Z "u", node_size));
    mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
    return MDB_PAGE_FULL;
}

// Delete the specified node from a page.
// mc Cursor pointing to the node to delete.
// ksize The size of a node. Only used if the page is
// part of a #MDB_DUPFIXED database.
//
void mdb_node_del(MDB_cursor* mc, int ksize)
{
    MDB_page* mp = mc->mc_pg[mc->mc_top];
    indx_t indx = mc->mc_ki[mc->mc_top];
    unsigned int sz;
    indx_t i, j, numkeys, ptr;
    MDB_node* node;
    char* base;

    DPRINTF(("delete node %u on %s page %" Yu, indx, IS_LEAF(mp) ? "leaf" : "branch", mdb_dbg_pgno(mp)));
    numkeys = NUMKEYS(mp);
    mdb_cassert(mc, indx < numkeys);

    if (IS_LEAF2(mp))
    {
        int x = numkeys - 1 - indx;
        base = LEAF2KEY(mp, indx, static_cast<size_t>(ksize));
        if (x)
            memmove(base, base + ksize, static_cast<size_t>(x) * ksize);
        MP_LOWER(mp) -= sizeof(indx_t);
        MP_UPPER(mp) += ksize - sizeof(indx_t);
        return;
    }

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

// Compact the main page after deleting a node on a subpage.
// mp The main page to operate on.
// indx The index of the subpage on the main page.
//
void mdb_node_shrink(MDB_page* mp, indx_t indx)
{
    MDB_node* node;
    MDB_page *sp, *xp;
    char* base;
    indx_t delta, len, ptr;
    unsigned int nsize;
    int i;

    node = NODEPTR(mp, indx);
    sp = reinterpret_cast<MDB_page*>(reinterpret_cast<char*>(node->mn_data) + node->mn_ksize);
    delta = SIZELEFT(sp);
    nsize = NODEDSZ(node) - delta;

    /* Prepare to shift upward, set len = length(subpage part to shift) */
    if (IS_LEAF2(sp))
    {
        len = nsize;
        if (nsize & 1)
            return; /* do not make the node uneven-sized */
    }
    else
    {
        xp = reinterpret_cast<MDB_page*>(reinterpret_cast<char*>(sp) + delta); /* destination subpage */
        for (i = NUMKEYS(sp); --i >= 0;)
            MP_PTRS(xp)[i] = MP_PTRS(sp)[i] - delta;
        len = PAGEHDRSZ;
    }
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

// Move a node from csrc to cdst.
//
int mdb_node_move(MDB_cursor* csrc, MDB_cursor* cdst, int fromleft)
{
    MDB_node* srcnode;
    MDB_val key, data;
    pgno_t srcpg;
    MDB_cursor mn;
    int rc;
    unsigned short flags;

    DKBUF;

    /* Mark src and dst as dirty. */
    rc = mdb_page_touch(csrc);
    if (rc)
        return rc;
    rc = mdb_page_touch(cdst);
    if (rc)
        return rc;

    if (IS_LEAF2(csrc->mc_pg[csrc->mc_top]))
    {
        key.mv_size = csrc->mc_db->md_pad;
        key.mv_data = LEAF2KEY(csrc->mc_pg[csrc->mc_top], csrc->mc_ki[csrc->mc_top], key.mv_size);
        data.mv_size = 0;
        data.mv_data = NULL;
        srcpg = 0;
        flags = 0;
    }
    else
    {
        srcnode = NODEPTR(csrc->mc_pg[csrc->mc_top], csrc->mc_ki[csrc->mc_top]);
        mdb_cassert(csrc, !((size_t)srcnode & 1));
        srcpg = NODEPGNO(srcnode);
        flags = srcnode->mn_flags;
        if (csrc->mc_ki[csrc->mc_top] == 0 && IS_BRANCH(csrc->mc_pg[csrc->mc_top]))
        {
            unsigned int snum = csrc->mc_snum;
            MDB_node* s2;
            /* must find the lowest key below src */
            rc = mdb_page_search_lowest(csrc);
            if (rc)
                return rc;
            if (IS_LEAF2(csrc->mc_pg[csrc->mc_top]))
            {
                key.mv_size = csrc->mc_db->md_pad;
                key.mv_data = LEAF2KEY(csrc->mc_pg[csrc->mc_top], 0, key.mv_size);
            }
            else
            {
                s2 = NODEPTR(csrc->mc_pg[csrc->mc_top], 0);
                key.mv_size = NODEKSZ(s2);
                key.mv_data = NODEKEY(s2);
            }
            csrc->mc_snum = snum--;
            csrc->mc_top = snum;
            csrc->mc_ki[snum] = 0;
            rc = mdb_update_key(csrc, &key);
            if (rc)
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
    mn.mc_xcursor = NULL;
    if (IS_BRANCH(cdst->mc_pg[cdst->mc_top]) && cdst->mc_ki[cdst->mc_top] == 0)
    {
        unsigned int snum = cdst->mc_snum;
        MDB_node* s2;
        MDB_val bkey;
        /* must find the lowest key below dst */
        mdb_cursor_copy(cdst, &mn);
        rc = mdb_page_search_lowest(&mn);
        if (rc)
            return rc;
        if (IS_LEAF2(mn.mc_pg[mn.mc_top]))
        {
            bkey.mv_size = mn.mc_db->md_pad;
            bkey.mv_data = LEAF2KEY(mn.mc_pg[mn.mc_top], 0, bkey.mv_size);
        }
        else
        {
            s2 = NODEPTR(mn.mc_pg[mn.mc_top], 0);
            bkey.mv_size = NODEKSZ(s2);
            bkey.mv_data = NODEKEY(s2);
        }
        mn.mc_snum = snum--;
        mn.mc_top = snum;
        mn.mc_ki[snum] = 0;
        rc = mdb_update_key(&mn, &bkey);
        if (rc)
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
        MDB_cursor *m2, *m3;
        MDB_dbi dbi = csrc->mc_dbi;
        MDB_page *mpd, *mps;

        mps = csrc->mc_pg[csrc->mc_top];
        /* If we're adding on the left, bump others up */
        if (fromleft)
        {
            mpd = cdst->mc_pg[csrc->mc_top];
            for (m2 = csrc->mc_txn->mt_cursors[dbi]; m2; m2 = m2->mc_next)
            {
                if (csrc->mc_flags & C_SUB)
                    m3 = &m2->mc_xcursor->mx_cursor;
                else
                    m3 = m2;
                if (!(m3->mc_flags & C_INITIALIZED) || m3->mc_top < csrc->mc_top)
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
            for (m2 = csrc->mc_txn->mt_cursors[dbi]; m2; m2 = m2->mc_next)
            {
                if (csrc->mc_flags & C_SUB)
                    m3 = &m2->mc_xcursor->mx_cursor;
                else
                    m3 = m2;
                if (m3 == csrc)
                    continue;
                if (!(m3->mc_flags & C_INITIALIZED) || m3->mc_top < csrc->mc_top)
                    continue;
                if (m3->mc_pg[csrc->mc_top] == mps)
                {
                    if (!m3->mc_ki[csrc->mc_top])
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
            if (IS_LEAF2(csrc->mc_pg[csrc->mc_top]))
            {
                key.mv_data = LEAF2KEY(csrc->mc_pg[csrc->mc_top], 0, key.mv_size);
            }
            else
            {
                srcnode = NODEPTR(csrc->mc_pg[csrc->mc_top], 0);
                key.mv_size = NODEKSZ(srcnode);
                key.mv_data = NODEKEY(srcnode);
            }
            DPRINTF(
                ("update separator for source page %" Yu " to [%s]", csrc->mc_pg[csrc->mc_top]->mp_pgno, DKEY(&key)));
            mdb_cursor_copy(csrc, &mn);
            mn.mc_snum--;
            mn.mc_top--;
            /* We want mdb_rebalance to find mn when doing fixups */
            WITH_CURSOR_TRACKING(mn, rc = mdb_update_key(&mn, &key));
            if (rc)
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
            if (IS_LEAF2(csrc->mc_pg[csrc->mc_top]))
            {
                key.mv_data = LEAF2KEY(cdst->mc_pg[cdst->mc_top], 0, key.mv_size);
            }
            else
            {
                srcnode = NODEPTR(cdst->mc_pg[cdst->mc_top], 0);
                key.mv_size = NODEKSZ(srcnode);
                key.mv_data = NODEKEY(srcnode);
            }
            DPRINTF(("update separator for destination page %" Yu " to [%s]",
                     cdst->mc_pg[cdst->mc_top]->mp_pgno,
                     DKEY(&key)));
            mdb_cursor_copy(cdst, &mn);
            mn.mc_snum--;
            mn.mc_top--;
            /* We want mdb_rebalance to find mn when doing fixups */
            WITH_CURSOR_TRACKING(mn, rc = mdb_update_key(&mn, &key));
            if (rc)
                return rc;
        }
        if (IS_BRANCH(cdst->mc_pg[cdst->mc_top]))
        {
            MDB_val nullkey;
            indx_t ix = cdst->mc_ki[cdst->mc_top];
            nullkey.mv_size = 0;
            cdst->mc_ki[cdst->mc_top] = 0;
            rc = mdb_update_key(cdst, &nullkey);
            cdst->mc_ki[cdst->mc_top] = ix;
            mdb_cassert(cdst, rc == MDB_SUCCESS);
        }
    }

    return MDB_SUCCESS;
}

// Merge one page into another.
// The nodes from the page pointed to by \b csrc will
// be copied to the page pointed to by \b cdst and then
// the \b csrc page will be freed.
// csrc Cursor pointing to the source page.
// cdst Cursor pointing to the destination page.
// 0 on success, non-zero on failure.
//
int mdb_page_merge(MDB_cursor* csrc, MDB_cursor* cdst)
{
    MDB_page *psrc, *pdst;
    MDB_node* srcnode;
    MDB_val key, data;
    unsigned nkeys;
    int rc;
    indx_t i, j;

    psrc = csrc->mc_pg[csrc->mc_top];
    pdst = cdst->mc_pg[cdst->mc_top];

    DPRINTF(("merging page %" Yu " into %" Yu, psrc->mp_pgno, pdst->mp_pgno));

    mdb_cassert(csrc, csrc->mc_snum > 1); /* can't merge root page */
    mdb_cassert(csrc, cdst->mc_snum > 1);

    /* Mark dst as dirty. */
    rc = mdb_page_touch(cdst);
    if (rc)
        return rc;

    /* get dst page again now that we've touched it. */
    pdst = cdst->mc_pg[cdst->mc_top];

    /* Move all nodes from src to dst.
     */
    j = nkeys = NUMKEYS(pdst);
    if (IS_LEAF2(psrc))
    {
        key.mv_size = csrc->mc_db->md_pad;
        key.mv_data = METADATA(psrc);
        for (unsigned int i = 0; i < NUMKEYS(psrc); i++, j++)
        {
            rc = mdb_node_add(cdst, j, &key, NULL, 0, 0);
            if (rc != MDB_SUCCESS)
                return rc;
            key.mv_data = (char*)key.mv_data + key.mv_size;
        }
    }
    else
    {
        for (unsigned int i = 0; i < NUMKEYS(psrc); i++, j++)
        {
            srcnode = NODEPTR(psrc, i);
            if (i == 0 && IS_BRANCH(psrc))
            {
                MDB_cursor mn;
                MDB_node* s2;
                mdb_cursor_copy(csrc, &mn);
                mn.mc_xcursor = NULL;
                /* must find the lowest key below src */
                rc = mdb_page_search_lowest(&mn);
                if (rc)
                    return rc;
                if (IS_LEAF2(mn.mc_pg[mn.mc_top]))
                {
                    key.mv_size = mn.mc_db->md_pad;
                    key.mv_data = LEAF2KEY(mn.mc_pg[mn.mc_top], 0, key.mv_size);
                }
                else
                {
                    s2 = NODEPTR(mn.mc_pg[mn.mc_top], 0);
                    key.mv_size = NODEKSZ(s2);
                    key.mv_data = NODEKEY(s2);
                }
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
    }

    DPRINTF(("dst page %" Yu " now has %u keys (%.1f%% filled)",
             pdst->mp_pgno,
             NUMKEYS(pdst),
             (float)PAGEFILL(cdst->mc_txn->mt_env, pdst) / 10));

    /* Unlink the src page from parent and add to free list.
     */
    csrc->mc_top--;
    mdb_node_del(csrc, 0);
    if (csrc->mc_ki[csrc->mc_top] == 0)
    {
        key.mv_size = 0;
        rc = mdb_update_key(csrc, &key);
        if (rc)
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
    rc = mdb_page_loose(csrc, psrc);
    if (rc)
        return rc;
    if (IS_LEAF(psrc))
        csrc->mc_db->md_leaf_pages--;
    else
        csrc->mc_db->md_branch_pages--;
    {
        /* Adjust other cursors pointing to mp */
        MDB_cursor *m2, *m3;
        MDB_dbi dbi = csrc->mc_dbi;
        unsigned int top = csrc->mc_top;

        for (m2 = csrc->mc_txn->mt_cursors[dbi]; m2; m2 = m2->mc_next)
        {
            if (csrc->mc_flags & C_SUB)
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
        /* Did the tree height change? */
        if (depth != cdst->mc_db->md_depth)
            snum += cdst->mc_db->md_depth - depth;
        cdst->mc_snum = snum;
        cdst->mc_top = snum - 1;
    }
    return rc;
}

// Rebalance the tree after a delete operation.
// mc Cursor pointing to the page where rebalancing
// should begin.
// 0 on success, non-zero on failure.
//
int mdb_rebalance(MDB_cursor* mc)
{
    MDB_node* node;
    int rc, fromleft;
    unsigned int ptop, minkeys, thresh;
    MDB_cursor mn;
    indx_t oldki;

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
    DPRINTF(("rebalancing %s page %" Yu " (has %u keys, %.1f%% full)",
             IS_LEAF(mc->mc_pg[mc->mc_top]) ? "leaf" : "branch",
             mdb_dbg_pgno(mc->mc_pg[mc->mc_top]),
             NUMKEYS(mc->mc_pg[mc->mc_top]),
             (float)PAGEFILL(mc->mc_txn->mt_env, mc->mc_pg[mc->mc_top]) / 10));

    if (PAGEFILL(mc->mc_txn->mt_env, mc->mc_pg[mc->mc_top]) >= thresh && NUMKEYS(mc->mc_pg[mc->mc_top]) >= minkeys)
    {
        DPRINTF(("no need to rebalance page %" Yu ", above fill threshold", mdb_dbg_pgno(mc->mc_pg[mc->mc_top])));
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
            rc = mdb_midl_append(&mc->mc_txn->mt_free_pgs, mp->mp_pgno);
            if (rc)
                return rc;
            /* Adjust cursors pointing to mp */
            mc->mc_snum = 0;
            mc->mc_top = 0;
            mc->mc_flags &= ~C_INITIALIZED;
            {
                MDB_cursor *m2, *m3;
                MDB_dbi dbi = mc->mc_dbi;

                for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2 = m2->mc_next)
                {
                    if (mc->mc_flags & C_SUB)
                        m3 = &m2->mc_xcursor->mx_cursor;
                    else
                        m3 = m2;
                    if (!(m3->mc_flags & C_INITIALIZED) || (m3->mc_snum < mc->mc_snum))
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
            rc = mdb_midl_append(&mc->mc_txn->mt_free_pgs, mp->mp_pgno);
            if (rc)
                return rc;
            mc->mc_db->md_root = NODEPGNO(NODEPTR(mp, 0));
            rc = mdb_page_get(mc, mc->mc_db->md_root, &mc->mc_pg[0], NULL);
            if (rc)
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
                MDB_cursor *m2, *m3;
                MDB_dbi dbi = mc->mc_dbi;

                for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2 = m2->mc_next)
                {
                    if (mc->mc_flags & C_SUB)
                        m3 = &m2->mc_xcursor->mx_cursor;
                    else
                        m3 = m2;
                    if (m3 == mc)
                        continue;
                    if (!(m3->mc_flags & C_INITIALIZED))
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
    mn.mc_xcursor = NULL;

    oldki = mc->mc_ki[mc->mc_top];
    if (mc->mc_ki[ptop] == 0)
    {
        /* We're the leftmost leaf in our parent.
         */
        DPUTS("reading right neighbor");
        mn.mc_ki[ptop]++;
        node = NODEPTR(mc->mc_pg[ptop], mn.mc_ki[ptop]);
        rc = mdb_page_get(mc, NODEPGNO(node), &mn.mc_pg[mn.mc_top], NULL);
        if (rc)
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
        rc = mdb_page_get(mc, NODEPGNO(node), &mn.mc_pg[mn.mc_top], NULL);
        if (rc)
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
        if (fromleft)
        {
            /* if we inserted on left, bump position up */
            oldki++;
        }
    }
    else
    {
        if (!fromleft)
        {
            rc = mdb_page_merge(&mn, mc);
        }
        else
        {
            oldki += NUMKEYS(mn.mc_pg[mn.mc_top]);
            mn.mc_ki[mn.mc_top] += mc->mc_ki[mn.mc_top] + 1;
            /* We want mdb_rebalance to find mn when doing fixups */
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
//
int mdb_page_split(MDB_cursor* mc, MDB_val* newkey, MDB_val* newdata, pgno_t newpgno, unsigned int nflags)
{
    unsigned int flags;
    int rc = MDB_SUCCESS, new_root = 0, did_split = 0;
    indx_t newindx;
    pgno_t pgno = 0;
    int i, j, split_indx, nkeys, pmax;
    MDB_env* env = mc->mc_txn->mt_env;
    MDB_node* node;
    MDB_val sepkey, rkey, xdata, *rdata = &xdata;
    MDB_page* copy = NULL;
    MDB_page *mp, *rp, *pp;
    int ptop;
    MDB_cursor mn;
    DKBUF;

    mp = mc->mc_pg[mc->mc_top];
    newindx = mc->mc_ki[mc->mc_top];
    nkeys = NUMKEYS(mp);

    DPRINTF(("-----> splitting %s page %" Yu " and adding [%s] at index %i/%i",
             IS_LEAF(mp) ? "leaf" : "branch",
             mp->mp_pgno,
             DKEY(newkey),
             mc->mc_ki[mc->mc_top],
             nkeys));

    /* Create a right sibling. */
    rc = mdb_page_new(mc, mp->mp_flags, 1, &rp);
    if (rc)
        return rc;
    rp->mp_pad = mp->mp_pad;
    DPRINTF(("new right sibling: page %" Yu, rp->mp_pgno));

    /* Usually when splitting the root page, the cursor
     * height is 1. But when called from mdb_update_key,
     * the cursor height may be greater because it walks
     * up the stack while finding the branch slot to update.
     */
    if (mc->mc_top < 1)
    {
        rc = mdb_page_new(mc, P_BRANCH, 1, &pp);
        if (rc)
            goto done;
        /* shift current top to make room for new parent */
        for (i = mc->mc_snum; i > 0; i--)
        {
            mc->mc_pg[i] = mc->mc_pg[i - 1];
            mc->mc_ki[i] = mc->mc_ki[i - 1];
        }
        mc->mc_pg[0] = pp;
        mc->mc_ki[0] = 0;
        mc->mc_db->md_root = pp->mp_pgno;
        DPRINTF(("root split! new root = %" Yu, pp->mp_pgno));
        new_root = mc->mc_db->md_depth++;

        /* Add left (implicit) pointer. */
        rc = mdb_node_add(mc, 0, NULL, NULL, mp->mp_pgno, 0);
        if (rc != MDB_SUCCESS)
        {
            /* undo the pre-push */
            mc->mc_pg[0] = mc->mc_pg[1];
            mc->mc_ki[0] = mc->mc_ki[1];
            mc->mc_db->md_root = mp->mp_pgno;
            mc->mc_db->md_depth--;
            goto done;
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

    mdb_cursor_copy(mc, &mn);
    mn.mc_xcursor = NULL;
    mn.mc_pg[mn.mc_top] = rp;
    mn.mc_ki[ptop] = mc->mc_ki[ptop] + 1;

    if (nflags & MDB_APPEND)
    {
        mn.mc_ki[mn.mc_top] = 0;
        sepkey = *newkey;
        split_indx = newindx;
        nkeys = 0;
    }
    else
    {
        split_indx = (nkeys + 1) / 2;

        if (IS_LEAF2(rp))
        {
            char *split, *ins;
            int x;
            unsigned int lsize, rsize, ksize;
            /* Move half of the keys to the right sibling */
            x = mc->mc_ki[mc->mc_top] - split_indx;
            ksize = mc->mc_db->md_pad;
            split = LEAF2KEY(mp, split_indx, static_cast<size_t>(ksize));
            rsize = static_cast<unsigned int>(nkeys - split_indx) * static_cast<unsigned int>(ksize);
            lsize = static_cast<unsigned int>(nkeys - split_indx) * sizeof(indx_t);
            mp->mp_lower -= lsize;
            rp->mp_lower += lsize;
            mp->mp_upper += rsize - lsize;
            rp->mp_upper -= rsize - lsize;
            sepkey.mv_size = ksize;
            if (newindx == split_indx)
            {
                sepkey.mv_data = newkey->mv_data;
            }
            else
            {
                sepkey.mv_data = split;
            }
            if (x < 0)
            {
                ins = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], static_cast<size_t>(ksize));
                memcpy(rp->mp_ptrs, split, rsize);
                sepkey.mv_data = rp->mp_ptrs;
                memmove(ins + ksize,
                        ins,
                        static_cast<size_t>(split_indx - mc->mc_ki[mc->mc_top]) * static_cast<size_t>(ksize));
                memcpy(ins, newkey->mv_data, ksize);
                mp->mp_lower += sizeof(indx_t);
                mp->mp_upper -= ksize - sizeof(indx_t);
            }
            else
            {
                if (x)
                    memcpy(rp->mp_ptrs, split, static_cast<size_t>(x) * ksize);
                ins = LEAF2KEY(rp, x, static_cast<size_t>(ksize));
                memcpy(ins, newkey->mv_data, ksize);
                memcpy(ins + ksize, split + static_cast<size_t>(x) * ksize, rsize - static_cast<size_t>(x) * ksize);
                rp->mp_lower += sizeof(indx_t);
                rp->mp_upper -= ksize - sizeof(indx_t);
                mc->mc_ki[mc->mc_top] = x;
            }
        }
        else
        {
            int psize, nsize, k, keythresh;

            /* Maximum free space in an empty page */
            pmax = env->me_psize - PAGEHDRSZ;
            /* Threshold number of keys considered "small" */
            keythresh = env->me_psize >> 7;

            if (IS_LEAF(mp))
                nsize = mdb_leaf_size(env, newkey, newdata);
            else
                nsize = mdb_branch_size(env, newkey);
            nsize = EVEN(nsize);

            /* grab a page to hold a temporary copy */
            copy = mdb_page_malloc(mc->mc_txn, 1);
            if (copy == NULL)
            {
                rc = ENOMEM;
                goto done;
            }
            copy->mp_pgno = mp->mp_pgno;
            copy->mp_flags = mp->mp_flags;
            copy->mp_lower = (PAGEHDRSZ - PAGEBASE);
            copy->mp_upper = env->me_psize - PAGEBASE;

            /* prepare to insert */
            for (i = 0, j = 0; i < nkeys; i++)
            {
                if (i == newindx)
                {
                    copy->mp_ptrs[j++] = 0;
                }
                copy->mp_ptrs[j++] = mp->mp_ptrs[i];
            }

            /* When items are relatively large the split point needs
             * to be checked, because being off-by-one will make the
             * difference between success or failure in mdb_node_add.
             *
             * It's also relevant if a page happens to be laid out
             * such that one half of its nodes are all "small" and
             * the other half of its nodes are "large." If the new
             * item is also "large" and falls on the half with
             * "large" nodes, it also may not fit.
             *
             * As a final tweak, if the new item goes on the last
             * spot on the page (and thus, onto the new page), bias
             * the split so the new page is emptier than the old page.
             * This yields better packing during sequential inserts.
             */
            if (nkeys < keythresh || nsize > pmax / 16 || newindx >= nkeys)
            {
                /* Find split point */
                psize = 0;
                if (newindx <= split_indx || newindx >= nkeys)
                {
                    i = 0;
                    j = 1;
                    k = newindx >= nkeys ? nkeys : split_indx + 1 + IS_LEAF(mp);
                }
                else
                {
                    i = nkeys;
                    j = -1;
                    k = split_indx - 1;
                }
                for (; i != k; i += j)
                {
                    if (i == newindx)
                    {
                        psize += nsize;
                        node = NULL;
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
                        split_indx = i + (j < 0);
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

    /* Copy separator key to the parent.
     */
    if (SIZELEFT(mn.mc_pg[ptop]) < mdb_branch_size(env, &sepkey))
    {
        int snum = mc->mc_snum;
        mn.mc_snum--;
        mn.mc_top--;
        did_split = 1;
        /* We want other splits to find mn when doing fixups */
        WITH_CURSOR_TRACKING(mn, rc = mdb_page_split(&mn, &sepkey, NULL, rp->mp_pgno, 0));
        if (rc)
            goto done;

        /* root split? */
        if (mc->mc_snum > snum)
        {
            ptop++;
        }
        /* Right page might now have changed parent.
         * Check if left page also changed parent.
         */
        if (mn.mc_pg[ptop] != mc->mc_pg[ptop] && mc->mc_ki[ptop] >= NUMKEYS(mc->mc_pg[ptop]))
        {
            for (i = 0; i < ptop; i++)
            {
                mc->mc_pg[i] = mn.mc_pg[i];
                mc->mc_ki[i] = mn.mc_ki[i];
            }
            mc->mc_pg[ptop] = mn.mc_pg[ptop];
            if (mn.mc_ki[ptop])
            {
                mc->mc_ki[ptop] = mn.mc_ki[ptop] - 1;
            }
            else
            {
                /* find right page's left sibling */
                mc->mc_ki[ptop] = mn.mc_ki[ptop];
                rc = mdb_cursor_sibling(mc, 0);
            }
        }
    }
    else
    {
        mn.mc_top--;
        rc = mdb_node_add(&mn, mn.mc_ki[ptop], &sepkey, NULL, rp->mp_pgno, 0);
        mn.mc_top++;
    }
    if (rc != MDB_SUCCESS)
    {
        if (rc == MDB_NOTFOUND) /* improper mdb_cursor_sibling() result */
            rc = MDB_PROBLEM;
        goto done;
    }
    if (nflags & MDB_APPEND)
    {
        mc->mc_pg[mc->mc_top] = rp;
        mc->mc_ki[mc->mc_top] = 0;
        rc = mdb_node_add(mc, 0, newkey, newdata, newpgno, nflags);
        if (rc)
            goto done;
        for (i = 0; i < mc->mc_top; i++)
            mc->mc_ki[i] = mn.mc_ki[i];
    }
    else if (!IS_LEAF2(mp))
    {
        /* Move nodes */
        mc->mc_pg[mc->mc_top] = rp;
        i = split_indx;
        j = 0;
        do
        {
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
                /* Update index for the new key. */
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
                /* First branch index doesn't need key data. */
                rkey.mv_size = 0;
            }

            rc = mdb_node_add(mc, j, &rkey, rdata, pgno, flags);
            if (rc)
                goto done;
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
        for (i = 0; i < nkeys; i++)
            mp->mp_ptrs[i] = copy->mp_ptrs[i];
        mp->mp_lower = copy->mp_lower;
        mp->mp_upper = copy->mp_upper;
        memcpy(NODEPTR(mp, nkeys - 1), NODEPTR(copy, nkeys - 1), env->me_psize - copy->mp_upper - PAGEBASE);

        /* reset back to original page */
        if (newindx < split_indx)
        {
            mc->mc_pg[mc->mc_top] = mp;
        }
        else
        {
            mc->mc_pg[mc->mc_top] = rp;
            mc->mc_ki[ptop]++;
            /* Make sure mc_ki is still valid.
             */
            if (mn.mc_pg[ptop] != mc->mc_pg[ptop] && mc->mc_ki[ptop] >= NUMKEYS(mc->mc_pg[ptop]))
            {
                for (i = 0; i <= ptop; i++)
                {
                    mc->mc_pg[i] = mn.mc_pg[i];
                    mc->mc_ki[i] = mn.mc_ki[i];
                }
            }
        }
        if (nflags & MDB_RESERVE)
        {
            node = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
            if (!(node->mn_flags & F_BIGDATA))
                newdata->mv_data = NODEDATA(node);
        }
    }
    else
    {
        if (newindx >= split_indx)
        {
            mc->mc_pg[mc->mc_top] = rp;
            mc->mc_ki[ptop]++;
            /* Make sure mc_ki is still valid.
             */
            if (mn.mc_pg[ptop] != mc->mc_pg[ptop] && mc->mc_ki[ptop] >= NUMKEYS(mc->mc_pg[ptop]))
            {
                for (i = 0; i <= ptop; i++)
                {
                    mc->mc_pg[i] = mn.mc_pg[i];
                    mc->mc_ki[i] = mn.mc_ki[i];
                }
            }
        }
    }

    {
        /* Adjust other cursors pointing to mp */
        MDB_cursor *m2, *m3;
        MDB_dbi dbi = mc->mc_dbi;
        nkeys = NUMKEYS(mp);

        for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2 = m2->mc_next)
        {
            if (mc->mc_flags & C_SUB)
                m3 = &m2->mc_xcursor->mx_cursor;
            else
                m3 = m2;
            if (m3 == mc)
                continue;
            if (!(m2->mc_flags & m3->mc_flags & C_INITIALIZED))
                continue;
            if (new_root)
            {
                int k;
                /* sub cursors may be on different DB */
                if (m3->mc_pg[0] != mp)
                    continue;
                /* root split */
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
                if (m3->mc_ki[mc->mc_top] >= newindx && !(nflags & MDB_SPLIT_REPLACE))
                    m3->mc_ki[mc->mc_top]++;
                if (m3->mc_ki[mc->mc_top] >= nkeys)
                {
                    m3->mc_pg[mc->mc_top] = rp;
                    m3->mc_ki[mc->mc_top] -= nkeys;
                    for (i = 0; i < mc->mc_top; i++)
                    {
                        m3->mc_ki[i] = mn.mc_ki[i];
                        m3->mc_pg[i] = mn.mc_pg[i];
                    }
                }
            }
            else if (!did_split && m3->mc_top >= ptop && m3->mc_pg[ptop] == mc->mc_pg[ptop] &&
                     m3->mc_ki[ptop] >= mc->mc_ki[ptop])
            {
                m3->mc_ki[ptop]++;
            }
            if (IS_LEAF(mp))
                XCURSOR_REFRESH(m3, mc->mc_top, m3->mc_pg[mc->mc_top]);
        }
    }
    DPRINTF(("mp left: %d, rp left: %d", SIZELEFT(mp), SIZELEFT(rp)));

done:
    if (copy) /* tmp page */
        mdb_page_free(env, copy);
    if (rc)
        mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
    return rc;
}

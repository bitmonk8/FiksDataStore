#include "mdb_page.h"

#include "mdb_cursor.h"
#include "mdb_db.h"
#include "mdb_debug.h"
#include "mdb_env.h"
#include "mdb_txn.h"

#include <utility>

// Page Management Operations
//
// Allocate memory for a page.
// Re-use old malloc'd pages first for singletons, otherwise just malloc.
// Set #MDB_TXN_ERROR on failure.
auto mdb_page_malloc(MDB_txn* txn, unsigned num) -> MDB_page*
{
    auto* env = txn->mt_env;

    // Fast path: For a single page, try to reuse one from the freelist.
    if (num == 1)
    {
        if (auto* page_from_freelist = env->me_dpages)
        {
            // A page is available. Pop it from the list and return it.
            env->me_dpages = page_from_freelist->mp_next;

            // These macros are for Valgrind memory tracking; their usage remains.
            const auto page_size = env->me_psize;
            VGMEMP_ALLOC(env, page_from_freelist, page_size);
            VGMEMP_DEFINED(page_from_freelist, sizeof(page_from_freelist->mp_next));

            return page_from_freelist;
        }
    }

    // Slow path: The freelist is empty or multiple pages were requested.
    // Allocate new memory.

    // Declare variables close to use with const where possible.
    const auto page_size = env->me_psize;
    const auto total_alloc_size = page_size * num;

    // Use static_cast for related type conversions instead of C-style casts.
    auto* new_page = static_cast<MDB_page*>(malloc(total_alloc_size));

    if (new_page != nullptr)
    {
        VGMEMP_ALLOC(env, new_page, total_alloc_size);

        // If memory initialization is not disabled, clear the relevant part of the memory.
        if ((env->me_flags & MDB_NOMEMINIT) == 0U)
        {
            // The logic for what to initialize is complex. We calculate the offset
            // and size to initialize without reusing variables.
            // For a single page, we clear everything after the header.
            // For multiple pages, we only clear the last page.
            const auto init_offset = (num == 1) ? PAGEHDRSZ : total_alloc_size - page_size;

            const auto init_size = (num == 1) ? page_size - PAGEHDRSZ : page_size;

            // Use reinterpret_cast for low-level, type-punned pointer manipulation.
            memset(reinterpret_cast<char*>(new_page) + init_offset, 0, init_size);

            // This field is only set during initialization.
            new_page->mp_pad = 0;
        }
    }
    else
    {
        // Allocation failed; mark the transaction with an error.
        txn->mt_flags |= MDB_TXN_ERROR;
    }

    return new_page;
}

// Free a single page.
// Saves single pages to a list, for future reuse.
// (This is not used for multi-page overflow pages.)
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
auto mdb_page_loose(MDB_cursor* mc, MDB_page* mp) -> int
{
    auto* const txn = mc->mc_txn;
    const auto pgno = mp->mp_pgno;

    bool is_loose_page = false;

    if (((mp->mp_flags & P_DIRTY) != 0) && mc->mc_dbi != FREE_DBI)
    {
        if (txn->mt_parent != nullptr)
        {
            auto* const dl = txn->mt_u.dirty_list;
            /* If txn has a parent, make sure the page is in our
             * dirty list.
             */
            if (dl[0].mid != 0U)
            {
                // Rule: Declare 'x' close to first use. Use 'const' and 'auto'.
                const auto x = mdb_mid2l_search(dl, pgno);
                if (x <= dl[0].mid && dl[x].mid == pgno)
                {
                    if (mp != dl[x].mptr)
                    { /* bad cursor? */
                        mc->mc_flags &= ~(C_INITIALIZED | C_EOF);
                        txn->mt_flags |= MDB_TXN_ERROR;
                        return MDB_PROBLEM;
                    }
                    /* ok, it's ours */
                    is_loose_page = true;
                }
            }
        }
        else
        {
            /* no parent txn, so it's just ours */
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

// Set or clear P_KEEP in dirty, non-overflow, non-sub pages watched by txn.
// mc A cursor handle for the current operation.
// pflags Flags of the pages to update:
// P_DIRTY to set P_KEEP, P_DIRTY|P_KEEP to clear it.
// all No shortcuts. Needed except after a full #mdb_page_flush().
// 0 on success, non-zero on failure.
auto mdb_pages_xkeep(MDB_cursor* mc, unsigned pflags, int all) -> int
{
    enum
    {
        Mask = P_SUBP | P_DIRTY | P_LOOSE | P_KEEP
    };
    const auto* const txn = mc->mc_txn;
    auto* const m0 = mc;  // Keep the starting cursor

    unsigned i = txn->mt_numdbs;
    while (true)
    {
        if ((mc->mc_flags & C_INITIALIZED) != 0U)
        {
            for (auto* m3 = mc; m3 != nullptr;)
            {
                MDB_page* mp{nullptr};
                unsigned j{0};
                for (unsigned j{0}; j < m3->mc_snum; j++)
                {
                    mp = m3->mc_pg[j];
                    if ((mp->mp_flags & Mask) == pflags)
                        mp->mp_flags ^= P_KEEP;
                }

                // Check if we can and should proceed to a sub-cursor (xcursor).
                auto* const mx = m3->mc_xcursor;
                if (mx == nullptr || (mx->mx_cursor.mc_flags & C_INITIALIZED) == 0U)
                    break;  // No valid sub-cursor to follow.

                if (mp == nullptr || (mp->mp_flags & P_LEAF) == 0)
                    break;  // Last page was not a leaf, cannot have a sub-db.

                const auto* leaf = NODEPTR(mp, m3->mc_ki[j - 1]);
                if ((leaf->mn_flags & F_SUBDATA) == 0)
                    break;  // Last node was not a sub-database entry.

                // Descend to the sub-cursor.
                m3 = &mx->mx_cursor;
            }
        }

        mc = mc->mc_next;

        // If we've finished the linked list or looped back to the start,
        // switch to iterating through the main transaction cursor array.
        while (mc == nullptr || mc == m0)
        {
            if (i == 0)
                goto all_cursors_processed;  // Exit the outer while-loop.
            mc = txn->mt_cursors[--i];
        }
    }

all_cursors_processed:
    if (all != 0)
    {
        // Mark dirty root pages
        for (unsigned k = 0; k < txn->mt_numdbs; k++)
        {
            if ((txn->mt_dbflags[k] & DB_DIRTY) != 0)
            {
                const auto pgno = txn->mt_dbs[k].md_root;
                if (pgno == P_INVALID)
                    continue;

                MDB_page* dp{nullptr};
                int level{0};
                int rc = mdb_page_get(m0, pgno, &dp, &level);
                if (rc != MDB_SUCCESS)
                    return rc;

                if ((dp->mp_flags & Mask) == pflags && level <= 1)
                    dp->mp_flags ^= P_KEEP;
            }
        }
    }

    return MDB_SUCCESS;
}

// Flush (some) dirty pages to the map, after clearing their dirty flag.
// txn the transaction that's being committed
// keep number of initial pages in dirty_list to keep dirty.
// 0 on success, non-zero on failure.
auto mdb_page_flush(MDB_txn* txn, int keep) -> int
{
    auto* const env = txn->mt_env;
    const auto dl = txn->mt_u.dirty_list;
    const auto psize = env->me_psize;
    const int pagecount = dl[0].mid;
    int rc{};
#ifdef _WIN32
    auto* ov = env->ov;
    const HANDLE fd = ((env->me_flags & MDB_NOSYNC) != 0U) ? env->me_fd : env->me_ovfd;
#else
    const HANDLE fd = env->me_fd;
#endif
    const int initial_keep_count = keep;
    int dirty_list_write_pos = initial_keep_count;
    int page_write_index = initial_keep_count;

    if (((env->me_flags & MDB_WRITEMAP) != 0U)
#ifdef _WIN32
        /* In windows, we still do writes to the file (with write-through enabled in sync mode),
         * as this is faster than FlushViewOfFile/FlushFileBuffers */
        && ((env->me_flags & MDB_NOSYNC) != 0U)
#endif
    )
    {
        /* Clear dirty flags */
        for (int i = initial_keep_count + 1; i <= pagecount; ++i)
        {
            auto* const page_to_clear = (MDB_page*)dl[i].mptr;
            /* Don't flush this page yet */
            if ((page_to_clear->mp_flags & (P_LOOSE | P_KEEP)) != 0)
            {
                page_to_clear->mp_flags &= ~P_KEEP;
                dl[++dirty_list_write_pos] = dl[i];
                continue;
            }
            page_to_clear->mp_flags &= ~P_DIRTY;
        }
        goto done;
    }

#ifdef _WIN32
    if (pagecount - keep >= env->ovs)
    {
        /* ran out of room in ov array, and re-malloc, copy handles and free previous */
        const int new_ovs_count =
            (pagecount - keep) * 1.5; /* provide extra padding to reduce number of re-allocations */
        const auto new_size = new_ovs_count * sizeof(OVERLAPPED);
        auto* const new_ov = (OVERLAPPED*)malloc(new_size);
        if (new_ov == nullptr)
            return ENOMEM;
        const auto previous_size = env->ovs * sizeof(OVERLAPPED);
        memcpy(new_ov, env->ov, previous_size); /* Copy previous OVERLAPPED data to retain event handles */
        /* And clear rest of memory */
        memset(&new_ov[env->ovs], 0, new_size - previous_size);
        if (env->ovs > 0)
        {
            free(env->ov); /* release previous allocation */
        }

        env->ov = new_ov;
        env->ovs = new_ovs_count;
        ov = new_ov;
    }
#endif

    /* Write the pages */
    {
#ifdef _WIN32
        int async_i{0};
        MDB_page* wdp{nullptr};
#else
        struct iovec iov[MDB_COMMIT_PAGES];
#endif
        ssize_t wsize{0};
        MDB_OFF_T wpos{0};
        MDB_OFF_T next_pos{1}; /* impossible pos, so pos != next_pos */
        int n{0};

        for (;;)
        {
            MDB_page* dp{nullptr};
            pgno_t pgno{0};
            MDB_OFF_T pos{0};
            size_t size{0};

            if (++page_write_index <= pagecount)
            {
                dp = (MDB_page*)dl[page_write_index].mptr;
                /* Don't flush this page yet */
                if ((dp->mp_flags & (P_LOOSE | P_KEEP)) != 0)
                {
                    dp->mp_flags &= ~P_KEEP;
                    dl[page_write_index].mid = 0;
                    continue;
                }
                pgno = dl[page_write_index].mid;
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
                || ((env->me_flags & MDB_WRITEMAP) == 0U)
#endif
            )
            {
                if (n != 0)
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
                    if (!F_ISSET(env->me_flags, MDB_NOSYNC) && (this_ov->hEvent == nullptr))
                    {
                        const HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                        if (event == nullptr)
                        {
                            rc = ErrCode();
                            DPRINTF(("CreateEvent: %s", strerror(rc)));
                            return rc;
                        }
                        this_ov->hEvent = event;
                    }
                    if (WriteFile(fd, wdp, wsize, nullptr, this_ov) == 0)
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
                    ssize_t wres;
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
                if (page_write_index > pagecount)
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
                if (ov[async_i].hEvent != nullptr)
                {
                    DWORD bytes_written;
                    if (GetOverlappedResult(fd, &ov[async_i], &bytes_written, TRUE) == 0)
                    {
                        rc = ErrCode(); /* Continue on so that all the event signals are reset */
                    }
                    [[maybe_unused]] const ssize_t wres = bytes_written;
                }
            }
            if (rc != 0)
            { /* any error on GetOverlappedResult, exit now */
                return rc;
            }
        }
#endif /* _WIN32 */
    }

    if ((env->me_flags & MDB_WRITEMAP) == 0U)
    {
        for (int i = initial_keep_count + 1; i <= pagecount; ++i)
        {
            auto* const page_to_cleanup = (MDB_page*)dl[i].mptr;
            /* This is a page we skipped above */
            if (dl[i].mid == 0U)
            {
                dl[++dirty_list_write_pos] = dl[i];
                dl[dirty_list_write_pos].mid = page_to_cleanup->mp_pgno;
                continue;
            }
            mdb_dpage_free(env, page_to_cleanup);
        }
    }

done:
    const int final_processed_count = page_write_index - 1;
    txn->mt_dirty_room += final_processed_count - dirty_list_write_pos;
    dl[0].mid = dirty_list_write_pos;
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
// Back up parent txn's cursors, then grab the originals for tracking
auto mdb_page_spill(MDB_cursor* m0, MDB_val* key, MDB_val* data) -> int
{
    auto* const txn = m0->mc_txn;

    if ((m0->mc_flags & C_SUB) != 0U)
        return MDB_SUCCESS;

    // Estimate how much space this op will take
    unsigned int space_estimate = m0->mc_db->md_depth;

    // Named DBs also dirty the main DB
    if (m0->mc_dbi >= CORE_DBS)
        space_estimate += txn->mt_dbs[MAIN_DBI].md_depth;

    // For puts, roughly factor in the key+data size
    if (key != nullptr)
        space_estimate += (LEAFSIZE(key, data) + txn->mt_env->me_psize) / txn->mt_env->me_psize;

    space_estimate += space_estimate;  // double it for good measure
    unsigned int need = space_estimate;

    if (txn->mt_dirty_room > need)
        return MDB_SUCCESS;

    if (txn->mt_spill_pgs == nullptr)
    {
        txn->mt_spill_pgs = mdb_midl_alloc(MDB_IDL_UM_MAX);
        if (txn->mt_spill_pgs == nullptr)
            return ENOMEM;
    }
    else
    {
        // purge deleted slots
        auto sl = txn->mt_spill_pgs;
        const auto num = sl[0];
        unsigned int new_count = 0;
        for (unsigned int i = 1; i <= num; i++)
        {
            if ((sl[i] & 1) == 0U)
                sl[++new_count] = sl[i];
        }
        sl[0] = new_count;
    }

    // Preserve pages which may soon be dirtied again
    const int pagesXKeepResult1 = mdb_pages_xkeep(m0, P_DIRTY, 1);
    if (pagesXKeepResult1 != MDB_SUCCESS)
    {
        txn->mt_flags |= MDB_TXN_ERROR;
        return pagesXKeepResult1;
    }

    /* Less aggressive spill - we originally spilled the entire dirty list,
     * with a few exceptions for cursor pages and DB root pages. But this
     * turns out to be a lot of wasted effort because in a large txn many
     * of those pages will need to be used again. So now we spill only 1/8th
     * of the dirty pages. Testing revealed this to be a good tradeoff,
     * better than 1/2, 1/4, or 1/10.
     */
    if (need < MDB_IDL_UM_MAX / 8)
        need = MDB_IDL_UM_MAX / 8;

    const auto dl = txn->mt_u.dirty_list;

    // Save the page IDs of all the pages we're flushing
    // flush from the tail forward, this saves a lot of shifting later on.
    unsigned int dirty_idx = dl[0].mid;
    for (; (dirty_idx != 0U) && (need != 0U); dirty_idx--)
    {
        const MDB_ID pn = dl[dirty_idx].mid << 1;
        auto* dp = (MDB_page*)dl[dirty_idx].mptr;
        if ((dp->mp_flags & (P_LOOSE | P_KEEP)) != 0)
            continue;

        // Can't spill twice, make sure it's not already in a parent's spill list.
        if (txn->mt_parent != nullptr)
        {
            bool page_is_kept = false;
            for (auto* tx2 = txn->mt_parent; tx2 != nullptr; tx2 = tx2->mt_parent)
            {
                if (tx2->mt_spill_pgs != nullptr)
                {
                    const auto search_idx = mdb_midl_search(tx2->mt_spill_pgs, pn);
                    if (search_idx <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[search_idx] == pn)
                    {
                        dp->mp_flags |= P_KEEP;
                        page_is_kept = true;
                        break;
                    }
                }
            }
            if (page_is_kept)
                continue;
        }
        const int midlAppendResult = mdb_midl_append(&txn->mt_spill_pgs, pn);
        if (midlAppendResult != 0)
        {
            txn->mt_flags |= MDB_TXN_ERROR;
            return midlAppendResult;
        }
        need--;
    }
    mdb_midl_sort(txn->mt_spill_pgs);

    /* Flush the spilled part of dirty list */
    const int pageFlushResult = mdb_page_flush(txn, dirty_idx);
    if (pageFlushResult != MDB_SUCCESS)
    {
        txn->mt_flags |= MDB_TXN_ERROR;
        return pageFlushResult;
    }

    /* Reset any dirty pages we kept that page_flush didn't see */
    const int pagesXKeepResult2 = mdb_pages_xkeep(m0, P_DIRTY | P_KEEP, dirty_idx);
    txn->mt_flags |= (pagesXKeepResult2 != 0) ? MDB_TXN_ERROR : MDB_TXN_SPILLS;
    return pagesXKeepResult2;
}

// Find oldest txnid still referenced. Expects txn->mt_txnid > 0.
auto mdb_find_oldest(MDB_txn* txn) -> txnid_t
{
    auto oldest = txn->mt_txnid - 1;
    if (txn->mt_env->me_txns != nullptr)
    {
        const auto* const r = txn->mt_env->me_txns->mti_readers;
        for (int i = txn->mt_env->me_txns->mti_numreaders; --i >= 0;)
        {
            if (r[i].mr_pid != 0)
            {
                const auto mr = r[i].mr_txnid;
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
#ifdef _WIN32  // With Windows we always write dirty pages with WriteFile, so we always want them ordered
    const auto insert = mdb_mid2l_insert;
#else  // but otherwise with writemaps, we just use msync, we don't need the ordering and just append
    const auto insert = (txn->mt_flags & MDB_TXN_WRITEMAP) ? mdb_mid2l_append : mdb_mid2l_insert;
#endif

    MDB_ID2 mid = {.mid=mp->mp_pgno, .mptr=mp};
    const auto rc = insert(txn->mt_u.dirty_list, &mid);
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
auto mdb_page_alloc(MDB_cursor* mc, int num, MDB_page** mp) -> int
{
    const auto txn = mc->mc_txn;
    const auto env = txn->mt_env;

    // Helper lambda to centralize error handling logic.
    auto set_error_and_return = [&](int error_code)
    {
        txn->mt_flags |= MDB_TXN_ERROR;
        return error_code;
    };

    /* If there are any loose pages, just use them (for single-page requests). */
    if (num == 1 && (txn->mt_loose_pgs != nullptr))
    {
        MDB_page* loose_page = txn->mt_loose_pgs;
        txn->mt_loose_pgs = NEXT_LOOSE_PAGE(loose_page);
        txn->mt_loose_count--;
        DPRINTF(("db %d use loose page %" Yu, DDBI(mc), loose_page->mp_pgno));
        *mp = loose_page;
        return MDB_SUCCESS;
    }

    *mp = nullptr;

    /* If our dirty list is already full, we can't allocate more pages. */
    if (txn->mt_dirty_room == 0)
    {
        return set_error_and_return(MDB_TXN_FULL);
    }

    pgno_t found_freelist_pgno = 0;
    unsigned freelist_idx = 0;

    /*
     * The main allocation logic:
     * 1. Try to find a contiguous block of pages in the in-memory freelist (me_pghead).
     * 2. If not found, fetch records from the on-disk freeDB, merge them into the
     *    in-memory freelist, and retry step 1.
     * 3. This continues until a block is found, retries are exhausted, or no more
     *    records can be fetched from the freeDB.
     * 4. If no suitable block is found in the freelist, allocate new pages from
     *    the end of the database map.
     */
    {
        MDB_cursor m2{};
        bool free_db_cursor_inited = false;
        txnid_t last_freed_txn_id = 0;
        txnid_t oldest_reader_txn_id = 0;
        bool oldest_reader_found = false;
        auto next_op = MDB_FIRST;
        int retry_count = num * 60;

        while (true)
        {
            pgno_t* current_mop = env->me_pghead;
            const unsigned current_mop_len = (current_mop != nullptr) ? current_mop[0] : 0;
            const unsigned contiguous_pages_needed = num - 1;

            if (current_mop_len > contiguous_pages_needed)
            {
                // Search for a contiguous block, preferring pages at the tail.
                for (unsigned i = current_mop_len; i > contiguous_pages_needed; --i)
                {
                    if (current_mop[i - contiguous_pages_needed] == current_mop[i] + contiguous_pages_needed)
                    {
                        found_freelist_pgno = current_mop[i];
                        freelist_idx = i;
                        goto search_complete;  // Found a block, exit the loop.
                    }
                }
            }

            if (--retry_count < 0)
            {
                break;  // Retries exhausted.
            }

            // --- Logic to fetch more pages from the freeDB ---
            if (!free_db_cursor_inited)
            {
                last_freed_txn_id = env->me_pglast;
                oldest_reader_txn_id = env->me_pgoldest;
                mdb_cursor_init(&m2, txn, FREE_DBI, nullptr);
                if (last_freed_txn_id != 0U)
                {
                    next_op = MDB_SET_RANGE;
                }
                free_db_cursor_inited = true;
            }

            last_freed_txn_id++;

            // Do not fetch more if the record is too recent for this transaction to see.
            if (oldest_reader_txn_id <= last_freed_txn_id)
            {
                if (!oldest_reader_found)
                {
                    oldest_reader_txn_id = mdb_find_oldest(txn);
                    env->me_pgoldest = oldest_reader_txn_id;
                    oldest_reader_found = true;
                }
                if (oldest_reader_txn_id <= last_freed_txn_id)
                {
                    break;  // Still too recent, can't fetch more.
                }
            }

            MDB_val key{};
            if (next_op == MDB_SET_RANGE)
            {
                key.mv_data = &last_freed_txn_id;
                key.mv_size = sizeof(last_freed_txn_id);
            }
            const int get_rc = mdb_cursor_get(&m2, &key, nullptr, next_op);
            next_op = MDB_NEXT;  // Subsequent gets will be MDB_NEXT.

            if (get_rc != MDB_SUCCESS)
            {
                if (get_rc == MDB_NOTFOUND)
                    break;  // No more records in freeDB.
                return set_error_and_return(get_rc);
            }

            const auto txn_id_from_key = *static_cast<txnid_t*>(key.mv_data);
            if (oldest_reader_txn_id <= txn_id_from_key)
            {
                if (!oldest_reader_found)
                {
                    oldest_reader_txn_id = mdb_find_oldest(txn);
                    env->me_pgoldest = oldest_reader_txn_id;
                    oldest_reader_found = true;
                }
                if (oldest_reader_txn_id <= txn_id_from_key)
                {
                    break;
                }
            }

            auto* page_in_cursor = m2.mc_pg[m2.mc_top];
            auto* leaf_node = NODEPTR(page_in_cursor, m2.mc_ki[m2.mc_top]);
            MDB_val data;
            const int node_read_rc = mdb_node_read(&m2, leaf_node, &data);
            if (node_read_rc != MDB_SUCCESS)
            {
                return set_error_and_return(node_read_rc);
            }

            auto* idl = static_cast<MDB_ID*>(data.mv_data);
            const unsigned idl_count = idl[0];

            // Ensure the in-memory freelist (mop) has enough space.
            if (env->me_pghead == nullptr)
            {
                auto* new_mop = mdb_midl_alloc(idl_count);
                if (new_mop == nullptr)
                    return set_error_and_return(ENOMEM);
                env->me_pghead = new_mop;
            }
            else
            {
                const int midl_need_rc = mdb_midl_need(&env->me_pghead, idl_count);
                if (midl_need_rc != 0)
                    return set_error_and_return(midl_need_rc);
            }

            env->me_pglast = txn_id_from_key;
            mdb_midl_xmerge(env->me_pghead, idl);
        }
    }
search_complete:

    pgno_t final_pgno;
    MDB_page* result_page;

    if (found_freelist_pgno != 0)
    {
        // --- A suitable block was found in the freelist ---
        final_pgno = found_freelist_pgno;

        if ((env->me_flags & MDB_WRITEMAP) != 0U)
        {
            result_page = reinterpret_cast<MDB_page*>(env->me_map + (env->me_psize * final_pgno));
        }
        else
        {
            result_page = mdb_page_malloc(txn, num);
            if (result_page == nullptr)
                return set_error_and_return(ENOMEM);
        }

        // Remove the allocated pages from the in-memory freelist.
        pgno_t* mop = env->me_pghead;
        const unsigned old_mop_len = mop[0];
        const unsigned new_mop_len = old_mop_len - num;

        pgno_t* dest = &mop[freelist_idx - num + 1];
        const pgno_t* src = &mop[freelist_idx + 1];
        const size_t num_to_move = old_mop_len - freelist_idx;

        if (num_to_move > 0)
        {
            memmove(dest, src, num_to_move * sizeof(pgno_t));
        }
        mop[0] = new_mop_len;
    }
    else
    {
        // --- No block found, allocate new pages from the end of the map ---
        const pgno_t new_pgno = txn->mt_next_pgno;

        if (new_pgno + num > env->me_maxpg)
        {
            DPUTS("DB size maxed out");
            return set_error_and_return(MDB_MAP_FULL);
        }
        final_pgno = new_pgno;

#if defined(_WIN32)
        if ((env->me_flags & MDB_RDONLY) == 0U)
        {
            void* p = VirtualAlloc(env->me_map + (env->me_psize * new_pgno),
                                   static_cast<SIZE_T>(env->me_psize) * num,
                                   MEM_COMMIT,
                                   ((env->me_flags & MDB_WRITEMAP) != 0U) ? PAGE_READWRITE : PAGE_READONLY);
            if (p == nullptr)
            {
                DPUTS("VirtualAlloc failed");
                return set_error_and_return(ErrCode());
            }
        }
#endif
        if ((env->me_flags & MDB_WRITEMAP) != 0U)
        {
            result_page = reinterpret_cast<MDB_page*>(env->me_map + (env->me_psize * final_pgno));
        }
        else
        {
            result_page = mdb_page_malloc(txn, num);
            if (result_page == nullptr)
                return set_error_and_return(ENOMEM);
        }

        txn->mt_next_pgno = new_pgno + num;
    }

    result_page->mp_pgno = final_pgno;
    mdb_page_dirty(txn, result_page);
    *mp = result_page;

    return MDB_SUCCESS;
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
    indx_t upper = src->mp_upper;
    indx_t lower = src->mp_lower;
    indx_t unused = upper - lower;

    /* If page isn't full, just copy the used portion. Adjust
     * alignment so memcpy may copy words instead of bytes.
     */
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

// Pull a page off the txn's spill list, if present.
// If a page being referenced was spilled to disk in this txn, bring
// it back and make it dirty/writable again.
// txn the transaction handle.
// mp the page being referenced. It must not be dirty.
// ret the writable page, if any. ret is unchanged if
// mp wasn't spilled.
auto mdb_page_unspill(MDB_txn* txn, MDB_page* mp, MDB_page** ret) -> int
{
    const auto* const env = txn->mt_env;
    const auto pgno = mp->mp_pgno;
    const auto pn = pgno << 1;

    for (const MDB_txn* tx2 = txn; tx2 != nullptr; tx2 = tx2->mt_parent)
    {
        if (tx2->mt_spill_pgs == nullptr)
            continue;

        const auto x = mdb_midl_search(tx2->mt_spill_pgs, pn);
        if (x <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[x] == pn)
        {
            if (txn->mt_dirty_room == 0)
                return MDB_TXN_FULL;

            const auto num = IS_OVERFLOW(mp) ? mp->mp_pages : 1;

            MDB_page* np;
            if ((env->me_flags & MDB_WRITEMAP) != 0U)
            {
                np = mp;
            }
            else
            {
                np = mdb_page_malloc(txn, num);
                if (np == nullptr)
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
        /* Update the parent page, if any, to point to the new page */
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
        /* If txn has a parent, make sure the page is in our
         * dirty list.
         */
        if (dl[0].mid != 0U)
        {
            const auto x = mdb_mid2l_search(dl, current_pgno);
            if (x <= dl[0].mid && dl[x].mid == current_pgno)
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
        new_page = mdb_page_malloc(txn, 1);
        if (new_page == nullptr)
            return ENOMEM;
        MDB_ID2 mid = {.mid=current_pgno, .mptr=new_page};
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
    /* Adjust cursors pointing to mp */
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

// Find the address of the page corresponding to a given page number.
// Set #MDB_TXN_ERROR on failure.
// mc the cursor accessing the page.
// pgno the page number for the page to retrieve.
// ret address of a pointer where the page's address will be stored.
// lvl dirty_list inheritance level of found page. 1=current txn, 0=mapped page.
// 0 on success, non-zero on failure.
auto mdb_page_get(MDB_cursor* mc, pgno_t pgno, MDB_page** ret, int* lvl) -> int
{
    auto* const txn = mc->mc_txn;

    if ((mc->mc_flags & (C_ORIG_RDONLY | C_WRITEMAP)) == 0U)
    {
        auto* tx2 = txn;
        int search_level = 1;
        do
        {
            // Spilled pages were dirtied in this txn and flushed
            // because the dirty list got full. Bring this page
            // back in from the map (but don't unspill it here,
            // leave that unless page_touch happens again).
            if (tx2->mt_spill_pgs != nullptr)
            {
                const auto pn = pgno << 1;
                const auto spill_idx = mdb_midl_search(tx2->mt_spill_pgs, pn);
                if (spill_idx <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[spill_idx] == pn)
                    break;
            }

            const auto dl = tx2->mt_u.dirty_list;
            if (dl[0].mid != 0U)
            {
                const auto dirty_idx = mdb_mid2l_search(dl, pgno);
                if (dirty_idx <= dl[0].mid && dl[dirty_idx].mid == pgno)
                {
                    *ret = (MDB_page*)dl[dirty_idx].mptr;
                    if (lvl != nullptr)
                        *lvl = search_level;
                    return MDB_SUCCESS;
                }
            }
            search_level++;
        } while ((tx2 = tx2->mt_parent) != nullptr);
    }

    if (pgno >= txn->mt_next_pgno)
    {
        DPRINTF(("page %" Yu " not found", pgno));
        txn->mt_flags |= MDB_TXN_ERROR;
        return MDB_PAGE_NOTFOUND;
    }

    const auto* env = txn->mt_env;
    *ret = (MDB_page*)(env->me_map + (env->me_psize * pgno));

    if (lvl != nullptr)
        *lvl = 0;

    return MDB_SUCCESS;
}

// Finish #mdb_page_search() / #mdb_page_search_lowest().
// The cursor is at the root page, set up the rest of it.
auto mdb_page_search_root(MDB_cursor* mc, MDB_val* key, int flags) -> int
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

        if ((flags & (MDB_PS_FIRST | MDB_PS_LAST)) != 0)
        {
            i = 0;
            if ((flags & MDB_PS_LAST) != 0)
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

        if ((flags & MDB_PS_MODIFY) != 0)
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

// Search for the lowest key under the current branch page.
// This just bypasses a NUMKEYS check in the current page
// before calling mdb_page_search_root(), because the callers
// are all in situations where the current page is known to
// be underfilled.
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

// Search for the lowest key under the current branch page.
// This just bypasses a NUMKEYS check in the current page
// before calling mdb_page_search_root(), because the callers
// are all in situations where the current page is known to
// be underfilled.
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

auto mdb_ovpage_free(MDB_cursor* mc, MDB_page* mp) -> int
{
    const auto txn = mc->mc_txn;
    auto pg = mp->mp_pgno;
    const auto ovpages = mp->mp_pages;
    const auto env = txn->mt_env;

    DPRINTF(("free ov page %" Yu " (%d)", pg, ovpages));
    // If the page is dirty or on the spill list we just acquired it,
    // so we should give it back to our current free list, if any.
    // Otherwise put it onto the list of pages we freed in this txn.
    //
    // Won't create me_pghead: me_pglast must be inited along with it.
    // Unsupported in nested txns: They would need to hide the page
    // range in ancestor txns' dirty and spilled lists.
    const auto sl = txn->mt_spill_pgs;
    unsigned spill_idx = 0;
    bool is_spilled = false;
    if (sl != nullptr)
    {
        const MDB_ID pn = pg << 1;
        spill_idx = mdb_midl_search(sl, pn);
        is_spilled = spill_idx <= sl[0] && sl[spill_idx] == pn;
    }

    const bool is_dirty = (mp->mp_flags & P_DIRTY) != 0;
    if ((env->me_pghead != nullptr) && (txn->mt_parent == nullptr) && (is_dirty || is_spilled))
    {
        if (const int rc = mdb_midl_need(&env->me_pghead, ovpages); rc != 0)
            return rc;

        if (is_dirty)
        {
            // Remove from dirty list
            const auto dl = txn->mt_u.dirty_list;
            unsigned search_idx = dl[0].mid--;
            MDB_ID2 current_item = dl[search_idx];
            while (current_item.mptr != mp)
            {
                if (search_idx > 1)
                {
                    --search_idx;
                    const MDB_ID2 next_item = dl[search_idx];
                    dl[search_idx] = current_item;
                    current_item = next_item;
                }
                else
                {
                    mdb_cassert(mc, search_idx > 1);
                    const unsigned restored_idx = ++(dl[0].mid);
                    dl[restored_idx] = current_item;  // Unsorted. OK when MDB_TXN_ERROR.
                    txn->mt_flags |= MDB_TXN_ERROR;
                    return MDB_PROBLEM;
                }
            }
            txn->mt_dirty_room++;
            if ((env->me_flags & MDB_WRITEMAP) == 0U)
                mdb_dpage_free(env, mp);
        }
        else
        {
            // This page is no longer spilled
            if (spill_idx == sl[0])
                sl[0]--;
            else
                sl[spill_idx] |= 1;
        }

        // Insert in me_pghead
        const auto mop = env->me_pghead;
        const unsigned new_count = mop[0] + ovpages;
        unsigned write_pos = new_count;
        unsigned read_pos;
        for (read_pos = mop[0]; (read_pos != 0U) && mop[read_pos] < pg; read_pos--)
            mop[write_pos--] = mop[read_pos];

        while (write_pos > read_pos)
            mop[write_pos--] = pg++;

        mop[0] += ovpages;
    }
    else
    {
        if (const int rc = mdb_midl_append_range(&txn->mt_free_pgs, pg, ovpages); rc != 0)
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
auto mdb_leaf_size(MDB_env* env, MDB_val* key, MDB_val* data) -> size_t
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
auto mdb_branch_size(MDB_env* env, MDB_val* key) -> size_t
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
auto mdb_node_add(MDB_cursor* mc, indx_t indx, MDB_val* key, MDB_val* data, pgno_t pgno, unsigned int flags) -> int
{
    unsigned int i;
    size_t node_size = NODESIZE;
    ssize_t room;
    indx_t ofs;
    MDB_node* node;
    MDB_page* mp = mc->mc_pg[mc->mc_top];
    MDB_page* ofp = nullptr; /* overflow page */
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

    room = (ssize_t)SIZELEFT(mp) - (ssize_t)sizeof(indx_t);
    if (key != nullptr)
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
    DPRINTF(("not enough room in page %" Yu ", got %u ptrs", mdb_dbg_pgno(mp), NUMKEYS(mp)));
    DPRINTF(("upper-lower = %u - %u = %" Z "d", MP_UPPER(mp), MP_LOWER(mp), room));
    DPRINTF(("node size = %" Z "u", node_size));
    mc->mc_txn->mt_flags |= MDB_TXN_ERROR;
    return MDB_PAGE_FULL;
}

// Delete the specified node from a page.
// mc Cursor pointing to the node to delete.
// ksize The size of a node (unused parameter).
//
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

    DPRINTF(("delete node %u on %s page %" Yu, indx, IS_LEAF(mp) ? "leaf" : "branch", mdb_dbg_pgno(mp)));
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

// Compact the main page after deleting a node on a subpage.
// mp The main page to operate on.
// indx The index of the subpage on the main page.
//
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

// Move a node from csrc to cdst.
//
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

    /* Mark src and dst as dirty. */
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
            /* must find the lowest key below src */
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
        /* must find the lowest key below dst */
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
            /* We want mdb_rebalance to find mn when doing fixups */
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

    mdb_cassert(csrc, csrc->mc_snum > 1); /* can't merge root page */
    mdb_cassert(csrc, cdst->mc_snum > 1);

    /* Mark dst as dirty. */
    rc = mdb_page_touch(cdst);
    if (rc != 0)
        return rc;

    /* get dst page again now that we've touched it. */
    pdst = cdst->mc_pg[cdst->mc_top];

    /* Move all nodes from src to dst.
     */
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
            /* must find the lowest key below src */
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

    /* Unlink the src page from parent and add to free list.
     */
    csrc->mc_top--;
    mdb_node_del(csrc, 0);
    if (csrc->mc_ki[csrc->mc_top] == 0)
    {
        key.mv_size = 0;
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
            rc = mdb_midl_append(&mc->mc_txn->mt_free_pgs, mp->mp_pgno);
            if (rc != 0)
                return rc;
            mc->mc_db->md_root = NODEPGNO(NODEPTR(mp, 0));
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

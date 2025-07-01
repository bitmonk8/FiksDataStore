#include "page_io.h"

#include "btree.h"
#include "cursor.h"
#include "db.h"
#include "debug.h"
#include "env.h"
#include "txn.h"

#include <cstdlib>
#include <cstring>
#include <utility>

// Page Management Operations

// Allocate memory for a page.
// Re-use old malloc'd pages first for singletons, otherwise just malloc.
// Set #FDS_TXN_ERROR on failure.
auto fds_page_malloc(FDS_txn* txn, unsigned num) -> FDS_page*
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
    auto* new_page = static_cast<FDS_page*>(malloc(total_alloc_size));

    if (new_page != nullptr)
    {
        VGMEMP_ALLOC(env, new_page, total_alloc_size);

        // If memory initialization is not disabled, clear the relevant part of the memory.
        if ((env->me_flags & FDS_NOMEMINIT) == 0U)
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
        txn->mt_flags |= FDS_TXN_ERROR;
    }

    return new_page;
}

// Free a single page.
// Saves single pages to a list, for future reuse.
// (This is not used for multi-page overflow pages.)
void fds_page_free(FDS_env* env, FDS_page* mp)
{
    mp->mp_next = env->me_dpages;
    VGMEMP_FREE(env, mp);
    env->me_dpages = mp;
}

// Free a dirty page
void fds_dpage_free(FDS_env* env, FDS_page* dp)
{
    if (!IS_OVERFLOW(dp) || dp->mp_pages == 1)
    {
        fds_page_free(env, dp);
    }
    else
    {
        // large pages just get freed directly
        VGMEMP_FREE(env, dp);
        free(dp);
    }
}

// Flush (some) dirty pages to the map, after clearing their dirty flag.
// txn the transaction that's being committed
// keep number of initial pages in dirty_list to keep dirty.
// 0 on success, non-zero on failure.
auto fds_page_flush(FDS_txn* txn, int keep) -> int
{
    auto* const env = txn->mt_env;
    const auto dl = txn->mt_u.dirty_list;
    const auto psize = env->me_psize;
    const int pagecount = dl[0].mid;
    int rc{};
#ifdef _WIN32
    auto* ov = env->ov;
    const HANDLE fd = ((env->me_flags & FDS_NOSYNC) != 0U) ? env->me_fd : env->me_ovfd;
#else
    const HANDLE fd = env->me_fd;
#endif
    const int initial_keep_count = keep;
    int dirty_list_write_pos = initial_keep_count;
    int page_write_index = initial_keep_count;

    if (((env->me_flags & FDS_WRITEMAP) != 0U)
#ifdef _WIN32
        // In windows, we still do writes to the file (with write-through enabled in sync mode),
        // as this is faster than FlushViewOfFile/FlushFileBuffers
        && ((env->me_flags & FDS_NOSYNC) != 0U)
#endif
    )
    {
        // Clear dirty flags
        for (int i = initial_keep_count + 1; i <= pagecount; ++i)
        {
            auto* const page_to_clear = (FDS_page*)dl[i].mptr;
            // Don't flush this page yet
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
        // ran out of room in ov array, and re-malloc, copy handles and free previous
        const int new_ovs_count = (pagecount - keep) * 1.5;  // provide extra padding to reduce number of re-allocations
        const auto new_size = new_ovs_count * sizeof(OVERLAPPED);
        auto* const new_ov = (OVERLAPPED*)malloc(new_size);
        if (new_ov == nullptr)
            return ENOMEM;
        const auto previous_size = env->ovs * sizeof(OVERLAPPED);
        memcpy(new_ov, env->ov, previous_size);  // Copy previous OVERLAPPED data to retain event handles
        // And clear rest of memory
        memset(&new_ov[env->ovs], 0, new_size - previous_size);
        if (env->ovs > 0)
        {
            free(env->ov);  // release previous allocation
        }

        env->ov = new_ov;
        env->ovs = new_ovs_count;
        ov = new_ov;
    }
#endif

    // Write the pages
    {
#ifdef _WIN32
        int async_i{0};
        FDS_page* wdp{nullptr};
#else
        struct iovec iov[FDS_COMMIT_PAGES];
#endif
        ssize_t wsize{0};
        FDS_OFF_T wpos{0};
        FDS_OFF_T next_pos{1}; /* impossible pos, so pos != next_pos */
        int n{0};

        for (;;)
        {
            FDS_page* dp{nullptr};
            pgno_t pgno{0};
            FDS_OFF_T pos{0};
            size_t size{0};

            if (++page_write_index <= pagecount)
            {
                dp = (FDS_page*)dl[page_write_index].mptr;
                // Don't flush this page yet
                if ((dp->mp_flags & (P_LOOSE | P_KEEP)) != 0)
                {
                    dp->mp_flags &= ~P_KEEP;
                    dl[page_write_index].mid = 0;
                    continue;
                }
                pgno = dl[page_write_index].mid;
                // clear dirty flag
                dp->mp_flags &= ~P_DIRTY;
                pos = pgno * psize;
                size = psize;
                if (IS_OVERFLOW(dp))
                    size *= dp->mp_pages;
            }
            // Write up to FDS_COMMIT_PAGES dirty pages at a time.
            if (pos != next_pos || n == FDS_COMMIT_PAGES ||
                wsize + size > MAX_WRITE
#ifdef _WIN32
                // If writemap is enabled, consecutive page positions infer
                // contiguous (mapped) memory.
                // Otherwise force write pages one at a time.
                // Windows actually supports scatter/gather I/O, but only on
                // unbuffered file handles. Since we're relying on the OS page
                // cache for all our data, that's self-defeating. So we just
                // write pages one at a time. We use the ov structure to set
                // the write offset, to at least save the overhead of a Seek
                // system call.
                || ((env->me_flags & FDS_WRITEMAP) == 0U)
#endif
            )
            {
                if (n != 0)
                {
                retry_write:
                    // Write previous page(s)
                    DPRINTF(("committing page %" Z "u", pgno));
#ifdef _WIN32
                    OVERLAPPED* this_ov = &ov[async_i];
                    // Clear status, and keep hEvent, we reuse that
                    this_ov->Internal = 0;
                    this_ov->Offset = wpos & 0xffffffff;
                    this_ov->OffsetHigh = wpos >> 16 >> 16;
                    if (!F_ISSET(env->me_flags, FDS_NOSYNC) && (this_ov->hEvent == nullptr))
                    {
                        const HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                        if (event == nullptr)
                            return ErrCode();
                            
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
                            rc = EIO;  // TODO: Use which error code?
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
        if (!F_ISSET(env->me_flags, FDS_NOSYNC))
        {
            // Now wait for all the asynchronous/overlapped sync/write-through writes to complete.
            // We start with the last one so that all the others should already be complete and
            // we reduce thread suspend/resuming (in practice, typically about 99.5% of writes are
            // done after the last write is done)
            rc = 0;
            while (--async_i >= 0)
            {
                if (ov[async_i].hEvent != nullptr)
                {
                    DWORD bytes_written;
                    if (GetOverlappedResult(fd, &ov[async_i], &bytes_written, TRUE) == 0)
                    {
                        rc = ErrCode();  // Continue on so that all the event signals are reset
                    }
                    [[maybe_unused]] const ssize_t wres = bytes_written;
                }
            }
            if (rc != 0)
            {  // any error on GetOverlappedResult, exit now
                return rc;
            }
        }
#endif  // _WIN32
    }

    if ((env->me_flags & FDS_WRITEMAP) == 0U)
    {
        for (int i = initial_keep_count + 1; i <= pagecount; ++i)
        {
            auto* const page_to_cleanup = (FDS_page*)dl[i].mptr;
            // This is a page we skipped above
            if (dl[i].mid == 0U)
            {
                dl[++dirty_list_write_pos] = dl[i];
                dl[dirty_list_write_pos].mid = page_to_cleanup->mp_pgno;
                continue;
            }
            fds_dpage_free(env, page_to_cleanup);
        }
    }

done:
    const int final_processed_count = page_write_index - 1;
    txn->mt_dirty_room += final_processed_count - dirty_list_write_pos;
    dl[0].mid = dirty_list_write_pos;
    return FDS_SUCCESS;
}

// Set or clear P_KEEP in dirty, non-overflow, non-sub pages watched by txn.
// mc A cursor handle for the current operation.
// pflags Flags of the pages to update:
// P_DIRTY to set P_KEEP, P_DIRTY|P_KEEP to clear it.
// all No shortcuts. Needed except after a full #fds_page_flush().
// 0 on success, non-zero on failure.
static auto fds_pages_xkeep(FDS_cursor* mc, unsigned pflags, int all) -> int
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
                FDS_page* mp{nullptr};
                unsigned j{0};
                for (unsigned j{0}; j < m3->mc_snum; j++)
                {
                    mp = m3->mc_pg[j];
                    if ((mp->mp_flags & Mask) == pflags)
                        mp->mp_flags ^= P_KEEP;
                }

                break;  // No valid sub-cursor to follow.
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

                FDS_page* dp{nullptr};
                int level{0};
                int rc = fds_page_get(m0, pgno, &dp, &level);
                if (rc != FDS_SUCCESS)
                    return rc;

                if ((dp->mp_flags & Mask) == pflags && level <= 1)
                    dp->mp_flags ^= P_KEEP;
            }
        }
    }

    return FDS_SUCCESS;
}

// Spill pages from the dirty list back to disk.
// This is intended to prevent running into #FDS_TXN_FULL situations,
// but note that they may still occur in a few cases:
// 1) our estimate of the txn size could be too small. Currently this
// seems unlikely, except with a large number of #FDS_MULTIPLE items.
// 2) child txns may run out of space if their parents dirtied a
// lot of pages and never spilled them. TODO: we probably should do
// a preemptive spill during #fds_txn_begin() of a child txn, if
// the parent's dirty_room is below a given threshold.
//
// Otherwise, if not using nested txns, it is expected that apps will
// not run into #FDS_TXN_FULL any more. The pages are flushed to disk
// the same way as for a txn commit, e.g. their P_DIRTY flag is cleared.
// If the txn never references them again, they can be left alone.
// If the txn only reads them, they can be used without any fuss.
// If the txn writes them again, they can be dirtied immediately without
// going thru all of the work of #fds_page_touch(). Such references are
// handled by #fds_page_unspill().
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
auto fds_page_spill(FDS_cursor* m0, FDS_val* key, FDS_val* data) -> int
{
    auto* const txn = m0->mc_txn;

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
        return FDS_SUCCESS;

    if (txn->mt_spill_pgs == nullptr)
    {
        txn->mt_spill_pgs = fds_midl_alloc(FDS_IDL_UM_MAX);
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
    const int pagesXKeepResult1 = fds_pages_xkeep(m0, P_DIRTY, 1);
    if (pagesXKeepResult1 != FDS_SUCCESS)
    {
        txn->mt_flags |= FDS_TXN_ERROR;
        return pagesXKeepResult1;
    }

    /* Less aggressive spill - we originally spilled the entire dirty list,
     * with a few exceptions for cursor pages and DB root pages. But this
     * turns out to be a lot of wasted effort because in a large txn many
     * of those pages will need to be used again. So now we spill only 1/8th
     * of the dirty pages. Testing revealed this to be a good tradeoff,
     * better than 1/2, 1/4, or 1/10.
     */
    if (need < FDS_IDL_UM_MAX / 8)
        need = FDS_IDL_UM_MAX / 8;

    const auto dl = txn->mt_u.dirty_list;

    // Save the page IDs of all the pages we're flushing
    // flush from the tail forward, this saves a lot of shifting later on.
    unsigned int dirty_idx = dl[0].mid;
    for (; (dirty_idx != 0U) && (need != 0U); dirty_idx--)
    {
        const FDS_ID pn = dl[dirty_idx].mid << 1;
        auto* dp = (FDS_page*)dl[dirty_idx].mptr;
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
                    const auto search_idx = fds_midl_search(tx2->mt_spill_pgs, pn);
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
        const int midlAppendResult = fds_midl_append(&txn->mt_spill_pgs, pn);
        if (midlAppendResult != 0)
        {
            txn->mt_flags |= FDS_TXN_ERROR;
            return midlAppendResult;
        }
        need--;
    }
    fds_midl_sort(txn->mt_spill_pgs);

    // Flush the spilled part of dirty list
    const int pageFlushResult = fds_page_flush(txn, dirty_idx);
    if (pageFlushResult != FDS_SUCCESS)
    {
        txn->mt_flags |= FDS_TXN_ERROR;
        return pageFlushResult;
    }

    // Reset any dirty pages we kept that page_flush didn't see
    const int pagesXKeepResult2 = fds_pages_xkeep(m0, P_DIRTY | P_KEEP, dirty_idx);
    txn->mt_flags |= (pagesXKeepResult2 != 0) ? FDS_TXN_ERROR : FDS_TXN_SPILLS;
    return pagesXKeepResult2;
}

// Add a page to the txn's dirty list
void fds_page_dirty(FDS_txn* txn, FDS_page* mp)
{
#ifdef _WIN32  // With Windows we always write dirty pages with WriteFile, so we always want them ordered
    const auto insert = fds_mid2l_insert;
#else  // but otherwise with writemaps, we just use msync, we don't need the ordering and just append
    const auto insert = (txn->mt_flags & FDS_TXN_WRITEMAP) ? fds_mid2l_append : fds_mid2l_insert;
#endif

    FDS_ID2 mid;
    mid.mid = mp->mp_pgno;
    mid.mptr = mp;
    const auto rc = insert(txn->mt_u.dirty_list, &mid);
    fds_tassert(txn, rc == 0);
    txn->mt_dirty_room--;
}

// Find oldest txnid still referenced. Expects txn->mt_txnid > 0.
static auto fds_find_oldest(FDS_txn* txn) -> txnid_t
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

// Allocate page numbers and memory for writing.  Maintain me_pglast,
// me_pghead and mt_next_pgno.  Set #FDS_TXN_ERROR on failure.int
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
auto fds_page_alloc(FDS_cursor* mc, int num, FDS_page** mp) -> int
{
    const auto txn = mc->mc_txn;
    const auto env = txn->mt_env;

    // Helper lambda to centralize error handling logic.
    auto set_error_and_return = [&](int error_code)
    {
        txn->mt_flags |= FDS_TXN_ERROR;
        return error_code;
    };

    // If there are any loose pages, just use them (for single-page requests).
    if (num == 1 && (txn->mt_loose_pgs != nullptr))
    {
        FDS_page* loose_page = txn->mt_loose_pgs;
        txn->mt_loose_pgs = NEXT_LOOSE_PAGE(loose_page);
        txn->mt_loose_count--;
        DPRINTF(("db %d use loose page %" Yu, DDBI(mc), loose_page->mp_pgno));
        *mp = loose_page;
        return FDS_SUCCESS;
    }

    *mp = nullptr;

    // If our dirty list is already full, we can't allocate more pages.
    if (txn->mt_dirty_room == 0)
    {
        return set_error_and_return(FDS_TXN_FULL);
    }

    pgno_t found_freelist_pgno = 0;
    unsigned freelist_idx = 0;

    //
    // The main allocation logic:
    // 1. Try to find a contiguous block of pages in the in-memory freelist (me_pghead).
    // 2. If not found, fetch records from the on-disk freeDB, merge them into the
    //    in-memory freelist, and retry step 1.
    // 3. This continues until a block is found, retries are exhausted, or no more
    //    records can be fetched from the freeDB.
    // 4. If no suitable block is found in the freelist, allocate new pages from
    //    the end of the database map.
    //
    {
        FDS_cursor m2{};
        bool free_db_cursor_inited = false;
        txnid_t last_freed_txn_id = 0;
        txnid_t oldest_reader_txn_id = 0;
        bool oldest_reader_found = false;
        auto next_op = FDS_FIRST;
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
                fds_cursor_init(&m2, txn, FREE_DBI);
                if (last_freed_txn_id != 0U)
                {
                    next_op = FDS_SET_RANGE;
                }
                free_db_cursor_inited = true;
            }

            last_freed_txn_id++;

            // Do not fetch more if the record is too recent for this transaction to see.
            if (oldest_reader_txn_id <= last_freed_txn_id)
            {
                if (!oldest_reader_found)
                {
                    oldest_reader_txn_id = fds_find_oldest(txn);
                    env->me_pgoldest = oldest_reader_txn_id;
                    oldest_reader_found = true;
                }
                if (oldest_reader_txn_id <= last_freed_txn_id)
                {
                    break;  // Still too recent, can't fetch more.
                }
            }

            FDS_val key{};
            if (next_op == FDS_SET_RANGE)
            {
                key.mv_data = &last_freed_txn_id;
                key.mv_size = sizeof(last_freed_txn_id);
            }
            const int get_rc = fds_cursor_get(&m2, &key, nullptr, next_op);
            next_op = FDS_NEXT;  // Subsequent gets will be FDS_NEXT.

            if (get_rc != FDS_SUCCESS)
            {
                if (get_rc == FDS_NOTFOUND)
                    break;  // No more records in freeDB.
                return set_error_and_return(get_rc);
            }

            const auto txn_id_from_key = *static_cast<txnid_t*>(key.mv_data);
            if (oldest_reader_txn_id <= txn_id_from_key)
            {
                if (!oldest_reader_found)
                {
                    oldest_reader_txn_id = fds_find_oldest(txn);
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
            FDS_val data;

            const int node_read_rc = fds_node_read(&m2, leaf_node, &data);
            if (node_read_rc != FDS_SUCCESS)
            {
                return set_error_and_return(node_read_rc);
            }

            auto* idl = static_cast<FDS_ID*>(data.mv_data);
            const unsigned idl_count = idl[0];

            // Ensure the in-memory freelist (mop) has enough space.
            if (env->me_pghead == nullptr)
            {
                auto* new_mop = fds_midl_alloc(idl_count);
                if (new_mop == nullptr)
                    return set_error_and_return(ENOMEM);
                env->me_pghead = new_mop;
            }
            else
            {
                const int midl_need_rc = fds_midl_need(&env->me_pghead, idl_count);
                if (midl_need_rc != 0)
                    return set_error_and_return(midl_need_rc);
            }

            env->me_pglast = txn_id_from_key;
            fds_midl_xmerge(env->me_pghead, idl);
        }
    }
search_complete:

    pgno_t final_pgno;
    FDS_page* result_page;

    if (found_freelist_pgno != 0)
    {
        // --- A suitable block was found in the freelist ---
        final_pgno = found_freelist_pgno;

        if ((env->me_flags & FDS_WRITEMAP) != 0U)
        {
            result_page = reinterpret_cast<FDS_page*>(env->me_map + (env->me_psize * final_pgno));
        }
        else
        {
            result_page = fds_page_malloc(txn, num);
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
            return set_error_and_return(FDS_MAP_FULL);
        }
        final_pgno = new_pgno;

#if defined(_WIN32)
        if ((env->me_flags & FDS_RDONLY) == 0U)
        {
            void* p = VirtualAlloc(env->me_map + (env->me_psize * new_pgno),
                                   static_cast<SIZE_T>(env->me_psize) * num,
                                   MEM_COMMIT,
                                   ((env->me_flags & FDS_WRITEMAP) != 0U) ? PAGE_READWRITE : PAGE_READONLY);
            if (p == nullptr)
            {
                DPUTS("VirtualAlloc failed");
                return set_error_and_return(ErrCode());
            }
        }
#endif
        if ((env->me_flags & FDS_WRITEMAP) != 0U)
        {
            result_page = reinterpret_cast<FDS_page*>(env->me_map + (env->me_psize * final_pgno));
        }
        else
        {
            result_page = fds_page_malloc(txn, num);
            if (result_page == nullptr)
                return set_error_and_return(ENOMEM);
        }

        txn->mt_next_pgno = new_pgno + num;
    }

    result_page->mp_pgno = final_pgno;
    fds_page_dirty(txn, result_page);
    *mp = result_page;

    return FDS_SUCCESS;
}

// Pull a page off the txn's spill list, if present.
// If a page being referenced was spilled to disk in this txn, bring
// it back and make it dirty/writable again.
// txn the transaction handle.
// mp the page being referenced. It must not be dirty.
// ret the writable page, if any. ret is unchanged if
// mp wasn't spilled.
auto fds_page_unspill(FDS_txn* txn, FDS_page* mp, FDS_page** ret) -> int
{
    const auto* const env = txn->mt_env;
    const auto pgno = mp->mp_pgno;
    const auto pn = pgno << 1;

    for (const FDS_txn* tx2 = txn; tx2 != nullptr; tx2 = tx2->mt_parent)
    {
        if (tx2->mt_spill_pgs == nullptr)
            continue;

        const auto x = fds_midl_search(tx2->mt_spill_pgs, pn);
        if (x <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[x] == pn)
        {
            if (txn->mt_dirty_room == 0)
                return FDS_TXN_FULL;

            const auto num = IS_OVERFLOW(mp) ? mp->mp_pages : 1;

            FDS_page* np;
            if ((env->me_flags & FDS_WRITEMAP) != 0U)
            {
                np = mp;
            }
            else
            {
                np = fds_page_malloc(txn, num);
                if (np == nullptr)
                    return ENOMEM;

                if (num > 1)
                    memcpy(np, mp, static_cast<size_t>(num) * env->me_psize);
                else
                    fds_page_copy(np, mp, env->me_psize);
            }
            if (tx2 == txn)
            {
                // If in current txn, this page is no longer spilled.
                // If it happens to be the last page, truncate the spill list.
                // Otherwise mark it as deleted by setting the LSB.
                if (x == txn->mt_spill_pgs[0])
                    txn->mt_spill_pgs[0]--;
                else
                    txn->mt_spill_pgs[x] |= 1;
            }  // otherwise, if belonging to a parent txn, the
               // page remains spilled until child commits
               //

            fds_page_dirty(txn, np);
            np->mp_flags |= P_DIRTY;
            *ret = np;
            break;
        }
    }
    return FDS_SUCCESS;
}

// Find the address of the page corresponding to a given page number.
// Set #FDS_TXN_ERROR on failure.
// mc the cursor accessing the page.
// pgno the page number for the page to retrieve.
// ret address of a pointer where the page's address will be stored.
// lvl dirty_list inheritance level of found page. 1=current txn, 0=mapped page.
// 0 on success, non-zero on failure.
auto fds_page_get(FDS_cursor* mc, pgno_t pgno, FDS_page** mp, int* lvl) -> int
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
                const auto spill_idx = fds_midl_search(tx2->mt_spill_pgs, pn);
                if (spill_idx <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[spill_idx] == pn)
                    break;
            }

            const auto dl = tx2->mt_u.dirty_list;
            if (dl[0].mid != 0U)
            {
                const auto dirty_idx = fds_mid2l_search(dl, pgno);
                if (dirty_idx <= dl[0].mid && dl[dirty_idx].mid == pgno)
                {
                    *mp = (FDS_page*)dl[dirty_idx].mptr;
                    if (lvl != nullptr)
                        *lvl = search_level;
                    return FDS_SUCCESS;
                }
            }
            search_level++;
        } while ((tx2 = tx2->mt_parent) != nullptr);
    }

    if (pgno >= txn->mt_next_pgno)
    {
        DPRINTF(("page %" Yu " not found", pgno));
        txn->mt_flags |= FDS_TXN_ERROR;
        return FDS_PAGE_NOTFOUND;
    }

    const auto* env = txn->mt_env;
    *mp = (FDS_page*)(env->me_map + (env->me_psize * pgno));

    if (lvl != nullptr)
        *lvl = 0;

    return FDS_SUCCESS;
}

auto fds_ovpage_free(FDS_cursor* mc, FDS_page* mp) -> int
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
        const FDS_ID pn = pg << 1;
        spill_idx = fds_midl_search(sl, pn);
        is_spilled = spill_idx <= sl[0] && sl[spill_idx] == pn;
    }

    const bool is_dirty = (mp->mp_flags & P_DIRTY) != 0;
    if ((env->me_pghead != nullptr) && (txn->mt_parent == nullptr) && (is_dirty || is_spilled))
    {
        if (const int rc = fds_midl_need(&env->me_pghead, ovpages); rc != 0)
            return rc;

        if (is_dirty)
        {
            // Remove from dirty list
            const auto dl = txn->mt_u.dirty_list;
            unsigned search_idx = dl[0].mid--;
            FDS_ID2 current_item = dl[search_idx];
            while (current_item.mptr != mp)
            {
                if (search_idx > 1)
                {
                    --search_idx;
                    const FDS_ID2 next_item = dl[search_idx];
                    dl[search_idx] = current_item;
                    current_item = next_item;
                }
                else
                {
                    fds_cassert(mc, search_idx > 1);
                    const unsigned restored_idx = ++(dl[0].mid);
                    dl[restored_idx] = current_item;  // Unsorted. OK when FDS_TXN_ERROR.
                    txn->mt_flags |= FDS_TXN_ERROR;
                    return FDS_PROBLEM;
                }
            }
            txn->mt_dirty_room++;
            if ((env->me_flags & FDS_WRITEMAP) == 0U)
                fds_dpage_free(env, mp);
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
        if (const int rc = fds_midl_append_range(&txn->mt_free_pgs, pg, ovpages); rc != 0)
            return rc;
    }

    mc->mc_db->md_overflow_pages -= ovpages;
    return 0;
}
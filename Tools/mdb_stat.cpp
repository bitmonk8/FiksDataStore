// mdb_stat.c - memory-mapped database status tool
//
// Copyright 2011-2021 Howard Chu, Symas Corp.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted only as authorized by the OpenLDAP
// Public License.
//
// A copy of this license is available in the file LICENSE in the
// top-level directory of the distribution or, alternatively, at
// <http://www.OpenLDAP.org/license.html>.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
using ssize_t = SSIZE_T;
#else
#include <unistd.h>
#endif

#include "lmdb.h"

#define Z MDB_FMT_Z
#define Yu MDB_PRIy(u)

static void prstat(MDB_stat* ms)
{
#if 0
//	printf("  Page size: %u\n", ms->ms_psize);
#endif
    printf("  Tree depth: %u\n", ms->ms_depth);
    printf("  Branch pages: %" Yu "\n", ms->ms_branch_pages);
    printf("  Leaf pages: %" Yu "\n", ms->ms_leaf_pages);
    printf("  Overflow pages: %" Yu "\n", ms->ms_overflow_pages);
    printf("  Entries: %" Yu "\n", ms->ms_entries);
}

static void usage(const char* prog)
{
    fprintf(stderr, "usage: %s [-V] [-n] [-e] [-r[r]] [-f[f[f]]] [-v] [-a|-s subdb] dbpath\n", prog);
    exit(EXIT_FAILURE);
}

auto main(int argc, char* argv[]) -> int
{
    int rc;
    MDB_env* env;
    MDB_txn* txn;
    MDB_dbi dbi;
    MDB_stat mst;
    MDB_envinfo mei;

    // options
    const char* prog = argv[0];
    const char* envname = nullptr;
    const char* subname = nullptr;
    int alldbs = 0;
    int envinfo = 0;
    int freinfo = 0;
    int rdrinfo = 0;
    unsigned envflags = 0;

    // ------------- option parser (replaces getopt) -------------
    int current_arg_index = 1; /* first argv index to examine */

    while (current_arg_index < argc && argv[current_arg_index][0] == '-')
    {
        const char* const current_arg = argv[current_arg_index];
        ++current_arg_index;

        // lone "--" terminates option scanning
        if (strcmp(current_arg, "--") == 0)
            break;

        // walk through the cluster, skipping the leading "-"
        for (size_t char_pos = 1; current_arg[char_pos] != 0; ++char_pos)
        {
            const char current_option = current_arg[char_pos];

            switch (current_option)
            {
            case 'V':
                printf("%s\n", MDB_VERSION_STRING);
                return 0;

            case 'a':
                if (subname != nullptr)
                    usage(prog);
                alldbs = 1;
                break;

            case 'e':
                envinfo = 1;
                break;

            case 'f':
                freinfo = 1;
                break;

            case 'n':
                envflags |= MDB_NOSUBDIR;
                break;

            case 'v':
                envflags |= MDB_PREVSNAPSHOT;
                break;

            case 'r':
                rdrinfo = 1;
                break;

            case 's': /* needs an argument */
                      // if characters remain in the same token, use them
                if (current_arg[char_pos + 1] != 0)
                {
                    subname = &current_arg[char_pos + 1];
                    char_pos = strlen(current_arg) - 1; /* exit inner loop */
                }
                else
                {
                    // otherwise take the next argv element
                    if (current_arg_index >= argc)
                        usage(prog);
                    subname = argv[current_arg_index];
                    ++current_arg_index;
                }
                if (alldbs != 0) /* -s conflicts with -a */
                    usage(prog);
                // stop processing the rest of this cluster
                char_pos = strlen(current_arg) - 1;
                break;

            default:
                usage(prog);
            }
        }
    }
    // ------------- end of option parser ------------------------

    // exactly one non-option argument (the environment path)
    if (current_arg_index != argc - 1)
        usage(prog);
    envname = argv[current_arg_index];
    rc = mdb_env_create(&env);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_env_create failed, error %d %s\n", rc, mdb_strerror(rc));
        return EXIT_FAILURE;
    }

    if ((alldbs != 0) || (subname != nullptr))
    {
        mdb_env_set_maxdbs(env, 4);
    }

    rc = mdb_env_open(env, envname, envflags | MDB_RDONLY, 0664);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_env_open failed, error %d %s\n", rc, mdb_strerror(rc));
        goto env_close;
    }

    if (envinfo != 0)
    {
        (void)mdb_env_stat(env, &mst);
        (void)mdb_env_info(env, &mei);
        printf("Environment Info\n");
        printf("  Map size: %" Yu "\n", mei.me_mapsize);
        printf("  Page size: %u\n", mst.ms_psize);
        printf("  Max pages: %" Yu "\n", mei.me_mapsize / mst.ms_psize);
        printf("  Number of pages used: %" Yu "\n", mei.me_last_pgno + 1);
        printf("  Last transaction ID: %" Yu "\n", mei.me_last_txnid);
        printf("  Max readers: %u\n", mei.me_maxreaders);
        printf("  Number of readers used: %u\n", mei.me_numreaders);
    }

    if (rdrinfo != 0)
    {
        printf("Reader Table Status\n");
        rc = mdb_reader_list(env, (MDB_msg_func)fputs, stdout);
        if (rdrinfo > 1)
        {
            int dead;
            mdb_reader_check(env, &dead);
            printf("  %d stale readers cleared.\n", dead);
            rc = mdb_reader_list(env, (MDB_msg_func)fputs, stdout);
        }
        if ((subname == nullptr) && (alldbs == 0) && (freinfo == 0))
            goto env_close;
    }

    rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_txn_begin failed, error %d %s\n", rc, mdb_strerror(rc));
        goto env_close;
    }

    if (freinfo != 0)
    {
        MDB_cursor* cursor;
        MDB_val key;
        MDB_val data;
        mdb_size_t pages = 0;
        mdb_size_t* iptr;

        printf("Freelist Status\n");
        dbi = 0;
        rc = mdb_cursor_open(txn, dbi, &cursor);
        if (rc != 0)
        {
            fprintf(stderr, "mdb_cursor_open failed, error %d %s\n", rc, mdb_strerror(rc));
            goto txn_abort;
        }
        rc = mdb_stat(txn, dbi, &mst);
        if (rc != 0)
        {
            fprintf(stderr, "mdb_stat failed, error %d %s\n", rc, mdb_strerror(rc));
            goto txn_abort;
        }
        prstat(&mst);
        while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0)
        {
            iptr = (mdb_size_t*)data.mv_data;
            const mdb_size_t entry_page_count = *iptr;
            pages += entry_page_count;

            if (freinfo > 1)
            {
                const char* sequence_status = "";
                const mdb_size_t* const page_list = iptr + 1;
                const ssize_t total_pages = entry_page_count;
                ssize_t max_span = 0;

                // Check sequence validity and find max span
                mdb_size_t previous_page = 1;
                for (ssize_t page_idx = total_pages - 1; page_idx >= 0; --page_idx)
                {
                    const mdb_size_t current_page = page_list[page_idx];
                    if (current_page <= previous_page)
                        sequence_status = " [bad sequence]";
                    previous_page = current_page;

                    // Calculate span for this page
                    mdb_size_t span_base = current_page;
                    ssize_t current_span = 0;
                    for (ssize_t span_idx = page_idx;
                         span_idx >= current_span && page_list[span_idx - current_span] == span_base + current_span;
                         ++current_span, ++span_base)
                        ;
                    if (current_span > max_span)
                        max_span = current_span;
                }

                printf("    Transaction %" Yu ", %" Z "d pages, maxspan %" Z "d%s\n",
                       *(mdb_size_t*)key.mv_data,
                       total_pages,
                       max_span,
                       sequence_status);

                if (freinfo > 2)
                {
                    // Print detailed page ranges
                    for (ssize_t detail_idx = total_pages - 1; detail_idx >= 0;)
                    {
                        const mdb_size_t range_start = page_list[detail_idx];
                        ssize_t range_length = 1;

                        // Find consecutive pages
                        while (detail_idx > 0)
                        {
                            const ssize_t next_idx = detail_idx - 1;
                            if (page_list[next_idx] != range_start + range_length)
                                break;
                            ++range_length;
                            detail_idx = next_idx;
                        }
                        --detail_idx;

                        printf(range_length > 1 ? "     %9" Yu "[%" Z "d]\n" : "     %9" Yu "\n",
                               range_start,
                               range_length);
                    }
                }
            }
        }
        mdb_cursor_close(cursor);
        printf("  Free pages: %" Yu "\n", pages);
    }

    rc = mdb_open(txn, subname, 0, &dbi);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_open failed, error %d %s\n", rc, mdb_strerror(rc));
        goto txn_abort;
    }

    rc = mdb_stat(txn, dbi, &mst);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_stat failed, error %d %s\n", rc, mdb_strerror(rc));
        goto txn_abort;
    }
    printf("Status of %s\n", (subname != nullptr) ? subname : "Main DB");
    prstat(&mst);

    if (alldbs != 0)
    {
        MDB_cursor* cursor;
        MDB_val key;

        rc = mdb_cursor_open(txn, dbi, &cursor);
        if (rc != 0)
        {
            fprintf(stderr, "mdb_cursor_open failed, error %d %s\n", rc, mdb_strerror(rc));
            goto txn_abort;
        }
        while ((rc = mdb_cursor_get(cursor, &key, nullptr, MDB_NEXT)) == 0)
        {
            char* str;
            MDB_dbi db2;
            if (memchr(key.mv_data, '\0', key.mv_size) != nullptr)
                continue;
            str = (char*)malloc(key.mv_size + 1);
            memcpy(str, key.mv_data, key.mv_size);
            str[key.mv_size] = '\0';
            rc = mdb_open(txn, str, 0, &db2);
            if (rc == MDB_SUCCESS)
                printf("Status of %s\n", str);
            free(str);
            if (rc != 0)
                continue;
            rc = mdb_stat(txn, db2, &mst);
            if (rc != 0)
            {
                fprintf(stderr, "mdb_stat failed, error %d %s\n", rc, mdb_strerror(rc));
                goto txn_abort;
            }
            prstat(&mst);
            mdb_close(env, db2);
        }
        mdb_cursor_close(cursor);
    }

    if (rc == MDB_NOTFOUND)
        rc = MDB_SUCCESS;

    mdb_close(env, dbi);
txn_abort:
    mdb_txn_abort(txn);
env_close:
    mdb_env_close(env);

    return (rc != 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

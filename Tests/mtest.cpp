// mtest.cpp - memory-mapped database tester/toy
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
#include "fds.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef FDS_WINDOWS
#include <direct.h>
#define mkdir(dir, mode) _mkdir(dir)
#endif

#define E(expr) CHECK((rc = (expr)) == FDS_SUCCESS, #expr)
#define RES(err, expr)                                                                                                 \
    (                                                                                                                  \
        [&]()                                                                                                          \
        {                                                                                                              \
            rc = (expr);                                                                                               \
            return (rc == (err) || (CHECK(!rc, #expr), 0));                                                            \
        }())
#define CHECK(test, msg)                                                                                               \
    ((test) ? (void)0                                                                                                  \
            : ((void)fprintf(stderr, "TEST FAILED: %s:%d: %s: %s\n", __FILE__, __LINE__, msg, fds_strerror(rc)),       \
               abort()))

auto main(int argc, char* argv[]) -> int
{
    int i = 0;
    int j = 0;
    int rc;
    FDS_env* env;
    FDS_dbi dbi;
    FDS_val key;
    FDS_val data;
    FDS_txn* txn;
    FDS_stat mst;
    FDS_cursor* cursor;
    FDS_cursor* cur2;
    FDS_cursor_op op;
    int count;
    int* values;
    char sval[32] = "";

    struct stat st = {.st_dev = 0};
    if (stat("./testdb1", &st) == -1)
        mkdir("./testdb1", 0700);

    srand(static_cast<unsigned int>(time(nullptr)));

    count = (rand() % 384) + 64;
    values = static_cast<int*>(malloc(count * sizeof(int)));

    for (i = 0; i < count; i++)
    {
        values[i] = rand() % 1024;
    }

    E(fds_env_create(&env));
    E(fds_env_set_maxreaders(env, 1));
    E(fds_env_set_mapsize(env, 10485760));
    E(fds_env_open(env, "./testdb1", 0 /*|FDS_NOSYNC*/, 0664));

    E(fds_txn_begin(env, nullptr, 0, &txn));
    E(fds_dbi_open(txn, nullptr, 0, &dbi));

    key.mv_size = sizeof(int);
    key.mv_data = sval;

    printf("Adding %d values\n", count);
    int duplicate_count = 0;
    for (int insert_idx = 0; insert_idx < count; insert_idx++)
    {
        snprintf(sval, sizeof(sval), "%03x %d foo bar", values[insert_idx], values[insert_idx]);
        // Set <data> in each iteration, since FDS_NOOVERWRITE may modify it
        data.mv_size = sizeof(sval);
        data.mv_data = sval;
        if (RES(FDS_KEYEXIST, fds_put(txn, dbi, &key, &data, FDS_NOOVERWRITE)))
        {
            duplicate_count++;
            data.mv_size = sizeof(sval);
            data.mv_data = sval;
        }
    }
    if (duplicate_count != 0)
        printf("%d duplicates skipped\n", duplicate_count);
    E(fds_txn_commit(txn));
    E(fds_env_stat(env, &mst));

    E(fds_txn_begin(env, nullptr, FDS_RDONLY, &txn));
    E(fds_cursor_open(txn, dbi, &cursor));
    while ((rc = fds_cursor_get(cursor, &key, &data, FDS_NEXT)) == 0)
    {
        printf("key: %p %.*s, data: %p %.*s\n",
               key.mv_data,
               static_cast<int>(key.mv_size),
               static_cast<char*>(key.mv_data),
               data.mv_data,
               static_cast<int>(data.mv_size),
               static_cast<char*>(data.mv_data));
    }
    CHECK(rc == FDS_NOTFOUND, "fds_cursor_get");
    fds_cursor_close(cursor);
    fds_txn_abort(txn);

    int deletion_count = 0;
    key.mv_data = sval;
    for (int delete_idx = count - 1; delete_idx > -1; delete_idx -= (rand() % 5))
    {
        deletion_count++;
        txn = nullptr;
        E(fds_txn_begin(env, nullptr, 0, &txn));
        snprintf(sval, sizeof(sval), "%03x ", values[delete_idx]);
        if (RES(FDS_NOTFOUND, fds_del(txn, dbi, &key, nullptr)))
        {
            deletion_count--;
            fds_txn_abort(txn);
        }
        else
        {
            E(fds_txn_commit(txn));
        }
    }
    free(values);
    printf("Deleted %d values\n", deletion_count);

    E(fds_env_stat(env, &mst));
    E(fds_txn_begin(env, nullptr, FDS_RDONLY, &txn));
    E(fds_cursor_open(txn, dbi, &cursor));
    printf("Cursor next\n");
    while ((rc = fds_cursor_get(cursor, &key, &data, FDS_NEXT)) == 0)
    {
        printf("key: %.*s, data: %.*s\n",
               static_cast<int>(key.mv_size),
               static_cast<char*>(key.mv_data),
               static_cast<int>(data.mv_size),
               static_cast<char*>(data.mv_data));
    }
    CHECK(rc == FDS_NOTFOUND, "fds_cursor_get");
    printf("Cursor last\n");
    E(fds_cursor_get(cursor, &key, &data, FDS_LAST));
    printf("key: %.*s, data: %.*s\n",
           static_cast<int>(key.mv_size),
           static_cast<char*>(key.mv_data),
           static_cast<int>(data.mv_size),
           static_cast<char*>(data.mv_data));
    printf("Cursor prev\n");
    while ((rc = fds_cursor_get(cursor, &key, &data, FDS_PREV)) == 0)
    {
        printf("key: %.*s, data: %.*s\n",
               static_cast<int>(key.mv_size),
               static_cast<char*>(key.mv_data),
               static_cast<int>(data.mv_size),
               static_cast<char*>(data.mv_data));
    }
    CHECK(rc == FDS_NOTFOUND, "fds_cursor_get");
    printf("Cursor last/prev\n");
    E(fds_cursor_get(cursor, &key, &data, FDS_LAST));
    printf("key: %.*s, data: %.*s\n",
           static_cast<int>(key.mv_size),
           static_cast<char*>(key.mv_data),
           static_cast<int>(data.mv_size),
           static_cast<char*>(data.mv_data));
    E(fds_cursor_get(cursor, &key, &data, FDS_PREV));
    printf("key: %.*s, data: %.*s\n",
           static_cast<int>(key.mv_size),
           static_cast<char*>(key.mv_data),
           static_cast<int>(data.mv_size),
           static_cast<char*>(data.mv_data));

    fds_cursor_close(cursor);
    fds_txn_abort(txn);

    printf("Deleting with cursor\n");
    E(fds_txn_begin(env, nullptr, 0, &txn));
    E(fds_cursor_open(txn, dbi, &cur2));
    for (int cursor_del_idx = 0; cursor_del_idx < 50; cursor_del_idx++)
    {
        if (RES(FDS_NOTFOUND, fds_cursor_get(cur2, &key, &data, FDS_NEXT)))
            break;
        printf("key: %p %.*s, data: %p %.*s\n",
               key.mv_data,
               static_cast<int>(key.mv_size),
               static_cast<char*>(key.mv_data),
               data.mv_data,
               static_cast<int>(data.mv_size),
               static_cast<char*>(data.mv_data));
        E(fds_del(txn, dbi, &key, nullptr));
    }

    printf("Restarting cursor in txn\n");
    for (FDS_cursor_op cursor_op = FDS_FIRST;; cursor_op = FDS_NEXT)
    {
        if (RES(FDS_NOTFOUND, fds_cursor_get(cur2, &key, &data, cursor_op)))
            break;
        printf("key: %p %.*s, data: %p %.*s\n",
               key.mv_data,
               static_cast<int>(key.mv_size),
               static_cast<char*>(key.mv_data),
               data.mv_data,
               static_cast<int>(data.mv_size),
               static_cast<char*>(data.mv_data));
    }
    fds_cursor_close(cur2);
    E(fds_txn_commit(txn));

    printf("Restarting cursor outside txn\n");
    E(fds_txn_begin(env, nullptr, 0, &txn));
    E(fds_cursor_open(txn, dbi, &cursor));
    for (FDS_cursor_op final_cursor_op = FDS_FIRST;; final_cursor_op = FDS_NEXT)
    {
        if (RES(FDS_NOTFOUND, fds_cursor_get(cursor, &key, &data, final_cursor_op)))
            break;
        printf("key: %p %.*s, data: %p %.*s\n",
               key.mv_data,
               static_cast<int>(key.mv_size),
               static_cast<char*>(key.mv_data),
               data.mv_data,
               static_cast<int>(data.mv_size),
               static_cast<char*>(data.mv_data));
    }
    fds_cursor_close(cursor);
    fds_txn_abort(txn);

    fds_dbi_close(env, dbi);
    fds_env_close(env);

    return 0;
}
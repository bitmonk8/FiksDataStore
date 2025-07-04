// mtest6.c - memory-mapped database tester/toy
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
// Tests for DB splits and merges
#include "fds.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

char dkbuf[1024];

auto main(int argc, char* argv[]) -> int
{
    int i = 0;
    int j = 0;
    int rc;
    FDS_env* env;
    FDS_dbi dbi;
    FDS_val key;
    FDS_val data;
    FDS_val sdata;
    FDS_txn* txn;
    FDS_stat mst;
    FDS_cursor* cursor;
    char kbuf[16];
    char* sval;
    struct stat st;
    if (stat("testdb", &st) == -1)
        mkdir("testdb", 0700);

    srand((unsigned int)time(nullptr));

    E(fds_env_create(&env));
    E(fds_env_set_mapsize(env, 10485760));
    E(fds_env_set_maxdbs(env, 4));
    E(fds_env_open(env, "testdb", 0));

    E(fds_txn_begin(env, nullptr, 0, &txn));
    E(fds_dbi_open(txn, "id6", FDS_CREATE, &dbi));
    E(fds_cursor_open(txn, dbi, &cursor));
    E(fds_stat(txn, dbi, &mst));

    sval = (char*)calloc(1, mst.ms_psize / 4);
    key.mv_data = kbuf;
    sdata.mv_size = mst.ms_psize / 4 - 30;
    sdata.mv_data = sval;

    printf("Adding 6 values, should yield 2 splits\n");
    for (i = 0; i < 6; i++)
    {
        snprintf(kbuf, sizeof(kbuf), "%03d", i * 5);
        key.mv_size = strlen(kbuf);
        snprintf(sval, mst.ms_psize / 4, "%03d", i * 5);
        data = sdata;
        (void)RES(FDS_KEYEXIST, fds_cursor_put(cursor, &key, &data, FDS_NOOVERWRITE));
    }
    printf("Adding 6 more values, should yield 2 splits\n");
    for (i = 0; i < 6; i++)
    {
        snprintf(kbuf, sizeof(kbuf), "%03d", (i * 5) + 4);
        key.mv_size = strlen(kbuf);
        snprintf(sval, mst.ms_psize / 4, "%03d", (i * 5) + 4);
        data = sdata;
        (void)RES(FDS_KEYEXIST, fds_cursor_put(cursor, &key, &data, FDS_NOOVERWRITE));
    }
    E(fds_cursor_get(cursor, &key, &data, FDS_FIRST));

    do
    {
        // printf("key: %p %s, data: %p %.*s\n",
        // 	key.mv_data,  fds_dkey(&key, dkbuf),
        // 	data.mv_data, (int) data.mv_size, (char *) data.mv_data);
    } while ((rc = fds_cursor_get(cursor, &key, &data, FDS_NEXT)) == 0);
    CHECK(rc == FDS_NOTFOUND, "fds_cursor_get");
    fds_cursor_close(cursor);
    fds_txn_commit(txn);
    fds_env_close(env);

    return 0;
}

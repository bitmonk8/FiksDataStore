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
#include "lmdb.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <ctime>

#ifdef _WIN32
#include <direct.h>
#define mkdir(dir, mode) _mkdir(dir)
#endif

#define E(expr) CHECK((rc = (expr)) == MDB_SUCCESS, #expr)
#define RES(err, expr)                                                                                                 \
    (                                                                                                                  \
        [&]()                                                                                                          \
        {                                                                                                              \
            rc = (expr);                                                                                               \
            return (rc == (err) || (CHECK(!rc, #expr), 0));                                                            \
        }())
#define CHECK(test, msg)                                                                                               \
    ((test) ? (void)0                                                                                                  \
            : ((void)fprintf(stderr, "TEST FAILED: %s:%d: %s: %s\n", __FILE__, __LINE__, msg, mdb_strerror(rc)),       \
               abort()))

char dkbuf[1024];

auto main(int argc, char* argv[]) -> int
{
    int i = 0;
    int j = 0;
    int rc;
    MDB_env* env;
    MDB_dbi dbi;
    MDB_val key;
    MDB_val data;
    MDB_val sdata;
    MDB_txn* txn;
    MDB_stat mst;
    MDB_cursor* cursor;
    char kbuf[16];
    char* sval;
    struct stat st = {.st_dev=0};
    if (stat("testdb", &st) == -1)
        mkdir("testdb", 0700);

    srand((unsigned int)time(nullptr));

    E(mdb_env_create(&env));
    E(mdb_env_set_mapsize(env, 10485760));
    E(mdb_env_set_maxdbs(env, 4));
    E(mdb_env_open(env, "testdb", MDB_NOSYNC, 0664));

    E(mdb_txn_begin(env, nullptr, 0, &txn));
    E(mdb_dbi_open(txn, "id6", MDB_CREATE, &dbi));
    E(mdb_cursor_open(txn, dbi, &cursor));
    E(mdb_stat(txn, dbi, &mst));

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
        (void)RES(MDB_KEYEXIST, mdb_cursor_put(cursor, &key, &data, MDB_NOOVERWRITE));
    }
    printf("Adding 6 more values, should yield 2 splits\n");
    for (i = 0; i < 6; i++)
    {
        snprintf(kbuf, sizeof(kbuf), "%03d", (i * 5) + 4);
        key.mv_size = strlen(kbuf);
        snprintf(sval, mst.ms_psize / 4, "%03d", (i * 5) + 4);
        data = sdata;
        (void)RES(MDB_KEYEXIST, mdb_cursor_put(cursor, &key, &data, MDB_NOOVERWRITE));
    }
    E(mdb_cursor_get(cursor, &key, &data, MDB_FIRST));

    do
    {
        // printf("key: %p %s, data: %p %.*s\n",
        // 	key.mv_data,  mdb_dkey(&key, dkbuf),
        // 	data.mv_data, (int) data.mv_size, (char *) data.mv_data);
    } while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0);
    CHECK(rc == MDB_NOTFOUND, "mdb_cursor_get");
    mdb_cursor_close(cursor);
    mdb_txn_commit(txn);
    mdb_env_close(env);

    return 0;
}

// mtest3.cpp - memory-mapped database tester/toy
//
//  Copyright 2011-2021 Howard Chu, Symas Corp.
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted only as authorized by the OpenLDAP
//  Public License.
//
//  A copy of this license is available in the file LICENSE in the
//  top-level directory of the distribution or, alternatively, at
//  <http://www.OpenLDAP.org/license.html>.
//
// Tests for sorted duplicate DBs
//
#ifdef _MSC_VER
#define CRT_SECURE_NO_WARNINGS
#endif

#include "lmdb.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

int main(int argc, char* argv[])
{
    int i = 0;
    int j = 0;
    int rc;
    MDB_env* env;
    MDB_dbi dbi;
    MDB_val key;
    MDB_val data;
    MDB_txn* txn;
    MDB_stat mst;
    MDB_cursor* cursor;
    int count;
    int* values;
    char sval[32];
    char kval[sizeof(int)];
    struct stat st = {0};
    if (stat("./testdb", &st) == -1)
        mkdir("./testdb", 0700);

    srand(static_cast<unsigned int>(time(NULL)));

    memset(sval, 0, sizeof(sval));

    count = (rand() % 384) + 64;
    values = static_cast<int*>(malloc(count * sizeof(int)));

    for (i = 0; i < count; i++)
    {
        values[i] = rand() % 1024;
    }

    E(mdb_env_create(&env));
    E(mdb_env_set_mapsize(env, 10485760));
    E(mdb_env_set_maxdbs(env, 4));
    E(mdb_env_open(env, "./testdb", MDB_FIXEDMAP | MDB_NOSYNC, 0664));

    E(mdb_txn_begin(env, NULL, 0, &txn));
    E(mdb_dbi_open(txn, "id2", MDB_CREATE | MDB_DUPSORT, &dbi));

    key.mv_size = sizeof(int);
    key.mv_data = kval;
    data.mv_size = sizeof(sval);
    data.mv_data = sval;

    printf("Adding %d values\n", count);
    int duplicate_count = 0;
    for (int insert_idx = 0; insert_idx < count; insert_idx++)
    {
        if ((insert_idx & 0x0f) == 0)
            snprintf(kval, sizeof(kval), "%03x", values[insert_idx]);
        snprintf(sval, sizeof(sval), "%03x %d foo bar", values[insert_idx], values[insert_idx]);
        if (RES(MDB_KEYEXIST, mdb_put(txn, dbi, &key, &data, MDB_NODUPDATA)))
            duplicate_count++;
    }
    if (duplicate_count != 0)
        printf("%d duplicates skipped\n", duplicate_count);
    E(mdb_txn_commit(txn));
    E(mdb_env_stat(env, &mst));

    E(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
    E(mdb_cursor_open(txn, dbi, &cursor));
    while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0)
    {
        printf("key: %p %.*s, data: %p %.*s\n",
               key.mv_data,
               static_cast<int>(key.mv_size),
               static_cast<char*>(key.mv_data),
               data.mv_data,
               static_cast<int>(data.mv_size),
               static_cast<char*>(data.mv_data));
    }
    CHECK(rc == MDB_NOTFOUND, "mdb_cursor_get");
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    int deletion_count = 0;

    for (int delete_idx = count - 1; delete_idx > -1; delete_idx -= (rand() % 5))
    {
        deletion_count++;
        txn = NULL;
        E(mdb_txn_begin(env, NULL, 0, &txn));
        snprintf(kval, sizeof(kval), "%03x", values[delete_idx & ~0x0f]);
        snprintf(sval, sizeof(sval), "%03x %d foo bar", values[delete_idx], values[delete_idx]);
        key.mv_size = sizeof(int);
        key.mv_data = kval;
        data.mv_size = sizeof(sval);
        data.mv_data = sval;
        if (RES(MDB_NOTFOUND, mdb_del(txn, dbi, &key, &data)))
        {
            deletion_count--;
            mdb_txn_abort(txn);
        }
        else
        {
            E(mdb_txn_commit(txn));
        }
    }
    free(values);
    printf("Deleted %d values\n", deletion_count);

    E(mdb_env_stat(env, &mst));
    E(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
    E(mdb_cursor_open(txn, dbi, &cursor));
    printf("Cursor next\n");
    while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0)
    {
        printf("key: %.*s, data: %.*s\n",
               static_cast<int>(key.mv_size),
               static_cast<char*>(key.mv_data),
               static_cast<int>(data.mv_size),
               static_cast<char*>(data.mv_data));
    }
    CHECK(rc == MDB_NOTFOUND, "mdb_cursor_get");
    printf("Cursor prev\n");
    while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_PREV)) == 0)
    {
        printf("key: %.*s, data: %.*s\n",
               static_cast<int>(key.mv_size),
               static_cast<char*>(key.mv_data),
               static_cast<int>(data.mv_size),
               static_cast<char*>(data.mv_data));
    }
    CHECK(rc == MDB_NOTFOUND, "mdb_cursor_get");
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    mdb_dbi_close(env, dbi);
    mdb_env_close(env);
    return 0;
}
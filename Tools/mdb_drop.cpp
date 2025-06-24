// mdb_drop.c - memory-mapped database delete tool
//
// Copyright 2016-2021 Howard Chu, Symas Corp.
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
#include "lmdb.h"

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static volatile sig_atomic_t gotsig;

static void dumpsig(int sig)
{
    gotsig = 1;
}

static void usage(char* prog)
{
    fprintf(stderr, "usage: %s [-V] [-n] [-d] [-s subdb] dbpath\n", prog);
    exit(EXIT_FAILURE);
}

static auto parse_cmdline(int argc, char** argv, int* envflags, int* do_delete, char** subname) -> int
{
    int current_arg_index = 1;  // skip argv[0]

    while (current_arg_index < argc)
    {
        const char* const current_arg = argv[current_arg_index];

        // stop when the first non-option is seen
        if (current_arg[0] != '-')
            break;

        if (strcmp(current_arg, "-d") == 0)
        {
            *do_delete = 1;
            ++current_arg_index;
        }
        else if (strcmp(current_arg, "-n") == 0)
        {
            *envflags |= MDB_NOSUBDIR;
            ++current_arg_index;
        }
        else if (strcmp(current_arg, "-V") == 0)
        {
            printf("%s\n", MDB_VERSION_STRING);
            exit(EXIT_SUCCESS);
        }
        else if (strcmp(current_arg, "-s") == 0)
        {
            const int next_arg_index = current_arg_index + 1;
            if (next_arg_index == argc)  // need a value after -s
                usage(argv[0]);
            *subname = argv[next_arg_index];
            current_arg_index = next_arg_index + 1;  // skip both -s and its argument
        }
        else
        {
            usage(argv[0]);  // unknown option
        }
    }
    return current_arg_index;
}

auto main(int argc, char* argv[]) -> int
{
    int i;
    int rc;
    MDB_env* env;
    MDB_txn* txn;
    MDB_dbi dbi;
    char* prog = argv[0];
    char* envname;
    char* subname = nullptr;
    int envflags = 0;
    int _delete = 0;
    int arg_index = 0;

    if (argc < 2)
    {
        usage(prog);
    }

    arg_index = parse_cmdline(argc, argv, &envflags, &_delete, &subname);

    if (arg_index != argc - 1)
        usage(prog);

#ifdef SIGPIPE
    signal(SIGPIPE, dumpsig);
#endif
#ifdef SIGHUP
    signal(SIGHUP, dumpsig);
#endif
    signal(SIGINT, dumpsig);
    signal(SIGTERM, dumpsig);

    envname = argv[arg_index];
    rc = mdb_env_create(&env);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_env_create failed, error %d %s\n", rc, mdb_strerror(rc));
        return EXIT_FAILURE;
    }

    mdb_env_set_maxdbs(env, 2);

    rc = mdb_env_open(env, envname, envflags, 0664);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_env_open failed, error %d %s\n", rc, mdb_strerror(rc));
        goto env_close;
    }

    rc = mdb_txn_begin(env, nullptr, 0, &txn);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_txn_begin failed, error %d %s\n", rc, mdb_strerror(rc));
        goto env_close;
    }

    rc = mdb_open(txn, subname, 0, &dbi);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_open failed, error %d %s\n", rc, mdb_strerror(rc));
        goto txn_abort;
    }

    rc = mdb_drop(txn, dbi, _delete);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_drop failed, error %d %s\n", rc, mdb_strerror(rc));
        goto txn_abort;
    }
    rc = mdb_txn_commit(txn);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_txn_commit failed, error %d %s\n", rc, mdb_strerror(rc));
        goto txn_abort;
    }
    txn = nullptr;

txn_abort:
    if (txn != nullptr)
        mdb_txn_abort(txn);
env_close:
    mdb_env_close(env);

    return (rc != 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

// mdb_copy.c - memory-mapped database backup tool
//
// Copyright 2012-2021 Howard Chu, Symas Corp.
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
#ifdef _WIN32
#include <windows.h>
#define MDB_STDOUT GetStdHandle(STD_OUTPUT_HANDLE)
#else
#define MDB_STDOUT 1
#endif
#include "lmdb.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static void sighandle(int sig)
{
}

int main(int argc, char* argv[])
{
    int rc;
    MDB_env* env;
    const char* const progname = argv[0];
    const char* act;
    unsigned flags = MDB_RDONLY;
    unsigned cpflags = 0;

    // Parse options without modifying original argc/argv
    int remaining_args = argc;
    char** current_argv = argv;

    while (remaining_args > 1 && current_argv[1][0] == '-')
    {
        const char* const option = current_argv[1];

        if (option[1] == 'n' && option[2] == '\0')
            flags |= MDB_NOSUBDIR;
        else if (option[1] == 'v' && option[2] == '\0')
            flags |= MDB_PREVSNAPSHOT;
        else if (option[1] == 'c' && option[2] == '\0')
            cpflags |= MDB_CP_COMPACT;
        else if (option[1] == 'V' && option[2] == '\0')
        {
            printf("%s\n", MDB_VERSION_STRING);
            exit(0);
        }
        else
        {
            remaining_args = 0;  // Invalid option - force usage error
            break;
        }

        --remaining_args;
        ++current_argv;
    }

    if (remaining_args < 2 || remaining_args > 3)
    {
        fprintf(stderr, "usage: %s [-V] [-c] [-n] [-v] srcpath [dstpath]\n", progname);
        exit(EXIT_FAILURE);
    }

#ifdef SIGPIPE
    signal(SIGPIPE, sighandle);
#endif
#ifdef SIGHUP
    signal(SIGHUP, sighandle);
#endif
    signal(SIGINT, sighandle);
    signal(SIGTERM, sighandle);

    act = "opening environment";
    rc = mdb_env_create(&env);
    if (rc == MDB_SUCCESS)
    {
        rc = mdb_env_open(env, current_argv[1], flags, 0600);
    }
    if (rc == MDB_SUCCESS)
    {
        act = "copying";
        if (remaining_args == 2)
            rc = mdb_env_copyfd2(env, MDB_STDOUT, cpflags);
        else
            rc = mdb_env_copy2(env, current_argv[2], cpflags);
    }
    if (rc != 0)
        fprintf(stderr, "%s: %s failed, error %d (%s)\n", progname, act, rc, mdb_strerror(rc));
    mdb_env_close(env);

    return (rc != 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

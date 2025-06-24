// mdb_dump.c - memory-mapped database dump tool
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
#ifdef _MSC_VER
#define CRT_SECURE_NO_WARNINGS
#endif

#include <cctype>
#include <cerrno>
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

#include <csignal>

#define Yu MDB_PRIy(u)

enum {
PRINT = 1
};
static int mode;

struct flagbit
{
    int bit;
    const char* name;
};

flagbit dbflags[] = {
    {.bit=MDB_REVERSEKEY, .name="reversekey"},
    {             .bit=0,         .name=nullptr}
};

static volatile sig_atomic_t gotsig;

static void dumpsig(int sig)
{
    gotsig = 1;
}

static const char hexc[] = "0123456789abcdef";

static void hex(unsigned char c)
{
    putchar(hexc[c >> 4]);
    putchar(hexc[c & 0xf]);
}

static void text(MDB_val* v)
{
    unsigned char* c;
    unsigned char* end;

    putchar(' ');
    c = (unsigned char*)v->mv_data;
    end = c + v->mv_size;
    while (c < end)
    {
        if (isprint(*c) != 0)
        {
            if (*c == '\\')
                putchar('\\');
            putchar(*c);
        }
        else
        {
            putchar('\\');
            hex(*c);
        }
        c++;
    }
    putchar('\n');
}

static void byte2(MDB_val* v)
{
    unsigned char* c;
    unsigned char* end;

    putchar(' ');
    c = (unsigned char*)v->mv_data;
    end = c + v->mv_size;
    while (c < end)
    {
        hex(*c++);
    }
    putchar('\n');
}

// Dump in BDB-compatible format
static auto dumpit(MDB_txn* txn, MDB_dbi dbi, char* name) -> int
{
    MDB_cursor* mc;
    MDB_stat ms;
    MDB_val key;
    MDB_val data;
    MDB_envinfo info;
    unsigned int flags;
    int rc;
    int i;

    rc = mdb_dbi_flags(txn, dbi, &flags);
    if (rc != 0)
        return rc;

    rc = mdb_stat(txn, dbi, &ms);
    if (rc != 0)
        return rc;

    rc = mdb_env_info(mdb_txn_env(txn), &info);
    if (rc != 0)
        return rc;

    printf("VERSION=3\n");
    printf("format=%s\n", ((mode & PRINT) != 0) ? "print" : "bytevalue");
    if (name != nullptr)
        printf("database=%s\n", name);
    printf("type=btree\n");
    printf("mapsize=%" Yu "\n", info.me_mapsize);
    printf("maxreaders=%u\n", info.me_maxreaders);

    for (int flag_idx = 0; dbflags[flag_idx].bit != 0; flag_idx++)
        if ((flags & dbflags[flag_idx].bit) != 0U)
            printf("%s=1\n", dbflags[flag_idx].name);

    printf("db_pagesize=%d\n", ms.ms_psize);
    printf("HEADER=END\n");

    rc = mdb_cursor_open(txn, dbi, &mc);
    if (rc != 0)
        return rc;

    while ((rc = static_cast<int>(mdb_cursor_get(mc, &key, &data, MDB_NEXT) == MDB_SUCCESS)) != 0)
    {
        if (gotsig != 0)
        {
            rc = EINTR;
            break;
        }
        if ((mode & PRINT) != 0)
        {
            text(&key);
            text(&data);
        }
        else
        {
            byte2(&key);
            byte2(&data);
        }
    }
    printf("DATA=END\n");
    if (rc == MDB_NOTFOUND)
        rc = MDB_SUCCESS;

    return rc;
}

static void usage(char* prog)
{
    fprintf(stderr, "usage: %s [-V] [-f output] [-l] [-n] [-p] [-v] [-a|-s subdb] dbpath\n", prog);
    exit(EXIT_FAILURE);
}

auto main(int argc, char* argv[]) -> int
{
    int alldbs = 0;
    int envflags = 0;
    int list = 0;
    int mode = 0;
    int i;  // outer argv index
    MDB_env* env;
    MDB_txn* txn;
    MDB_dbi dbi;
    char* prog = argv[0];
    char* envname = nullptr;
    char* subname = nullptr;

    // ---------- manual option parsing (no getopt) ----------
    for (i = 1; i < argc; ++i)
    {
        char* arg = argv[i];

        // stop at first non-option or at “--”
        if (arg[0] != '-' || strcmp(arg, "--") == 0)
        {
            if (strcmp(arg, "--") == 0)  // skip “--” itself
                ++i;
            break;
        }

        // scan each character after the leading “-”
        for (size_t j = 1; arg[j] != '\0'; ++j)
        {
            char opt = arg[j];
            char* optarg = nullptr;  // value, if needed

            switch (opt)
            {
            case 'V':
                printf("%s\n", MDB_VERSION_STRING);
                exit(EXIT_SUCCESS);

            case 'l':
                list = 1;
                // FALLTHROUGH
            case 'a':
                if (subname != nullptr)
                    usage(prog);
                ++alldbs;
                break;

            case 'n':
                envflags |= MDB_NOSUBDIR;
                break;

            case 'v':
                envflags |= MDB_PREVSNAPSHOT;
                break;

            case 'p':
                mode |= PRINT;
                break;

            // ---- options that take an argument ----
            case 'f':
            case 's':
                // any characters left on this option?
                if (arg[j + 1] != '\0')
                {
                    optarg = &arg[j + 1];  //  -ffile
                    j = strlen(arg) - 1;   //  stop scanning this arg
                }
                else
                {
                    if (++i >= argc)  //  -f file
                        usage(prog);
                    optarg = argv[i];
                }

                if (opt == 'f')
                {
#ifdef _WIN32
                    FILE* new_stdout;
                    errno_t err = freopen_s(&new_stdout, optarg, "w", stdout);
                    if (err != 0)
                    {
                        char error_msg[256];
                        strerror_s(error_msg, sizeof(error_msg), errno);
                        fprintf(stderr, "%s: %s: reopen: %s\n", prog, optarg, error_msg);
                        exit(EXIT_FAILURE);
                    }
#else
                    if (freopen(optarg, "w", stdout) == NULL)
                    {
                        fprintf(stderr, "%s: %s: reopen: %s\n", prog, optarg, strerror(errno));
                        exit(EXIT_FAILURE);
                    }
#endif
                }
                else
                {  // opt == 's'
                    if (alldbs != 0)
                        usage(prog);
                    subname = optarg;
                }
                // reset inner loop because we consumed next argv (if any)
                j = strlen(arg) - 1;
                break;

            default:
                usage(prog);
            }
        }
    }

    // ---------- positional arguments ----------
    if (i != argc - 1)  // need exactly one env path
        usage(prog);

    envname = argv[i];

    // ---------- signal handling (unchanged) ----
#ifdef SIGPIPE
    signal(SIGPIPE, dumpsig);
#endif
#ifdef SIGHUP
    signal(SIGHUP, dumpsig);
#endif
    signal(SIGINT, dumpsig);
    signal(SIGTERM, dumpsig);

    int rc = mdb_env_create(&env);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_env_create failed, error %d %s\n", rc, mdb_strerror(rc));
        return EXIT_FAILURE;
    }

    if ((alldbs != 0) || (subname != nullptr))
    {
        mdb_env_set_maxdbs(env, 2);
    }

    rc = mdb_env_open(env, envname, envflags | MDB_RDONLY, 0664);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_env_open failed, error %d %s\n", rc, mdb_strerror(rc));
        goto env_close;
    }

    rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
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

    if (alldbs != 0)
    {
        MDB_cursor* cursor;
        MDB_val key;
        int count = 0;

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
            count++;
            str = (char*)malloc(key.mv_size + 1);
            memcpy(str, key.mv_data, key.mv_size);
            str[key.mv_size] = '\0';
            rc = mdb_open(txn, str, 0, &db2);
            if (rc == MDB_SUCCESS)
            {
                if (list != 0)
                {
                    printf("%s\n", str);
                    // Note: list variable re-purposed as counter here - should be refactored
                    list++;
                }
                else
                {
                    rc = dumpit(txn, db2, str);
                    if (rc != 0)
                        break;
                }
                mdb_close(env, db2);
            }
            free(str);
            if (rc != 0)
                continue;
        }
        mdb_cursor_close(cursor);
        if (count == 0)
        {
            fprintf(stderr, "%s: %s does not contain multiple databases\n", prog, envname);
            rc = MDB_NOTFOUND;
        }
        else if (rc == MDB_NOTFOUND)
        {
            rc = MDB_SUCCESS;
        }
    }
    else
    {
        rc = dumpit(txn, dbi, subname);
    }
    if ((rc != 0) && rc != MDB_NOTFOUND)
        fprintf(stderr, "%s: %s: %s\n", prog, envname, mdb_strerror(rc));

    mdb_close(env, dbi);
txn_abort:
    mdb_txn_abort(txn);
env_close:
    mdb_env_close(env);

    return (rc != 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

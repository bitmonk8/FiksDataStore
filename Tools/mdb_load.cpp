// mdb_load.c - memory-mapped database load tool
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

#include "lmdb.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define mdb_strdup _strdup
#else
#define mdb_strdup strdup
#endif

#define PRINT 1
#define NOHDR 2
static int mode;

static char* subname = NULL;

static mdb_size_t lineno;
static int version;

static int flags;

static char* prog;

static int Eof;

static MDB_envinfo info;

static MDB_val kbuf, dbuf;
static MDB_val k0buf;

#define Yu MDB_PRIy(u)

#define STRLENOF(s) (sizeof(s) - 1)

struct flagbit
{
    int bit;
    const char* name;
    int len;
};

#define S(s) s, STRLENOF(s)

flagbit dbflags[] = {
    {MDB_REVERSEKEY, S("reversekey")},
    {MDB_DUPSORT, S("dupsort")},
    {MDB_INTEGERKEY, S("integerkey")},
    {MDB_DUPFIXED, S("dupfixed")},
    {MDB_INTEGERDUP, S("integerdup")},
    {MDB_REVERSEDUP, S("reversedup")},
    {0, NULL, 0}
};

static void readhdr(void)
{
    char* ptr;

    flags = 0;
    while (fgets((char*)dbuf.mv_data, (int)dbuf.mv_size, stdin) != NULL)
    {
        lineno++;
        if (strncmp((char*)dbuf.mv_data, "VERSION=", STRLENOF("VERSION=")) == 0)
        {
            version = atoi((char*)dbuf.mv_data + STRLENOF("VERSION="));
            if (version > 3)
            {
                fprintf(stderr, "%s: line %" Yu ": unsupported VERSION %d\n", prog, lineno, version);
                exit(EXIT_FAILURE);
            }
        }
        else if (strncmp((char*)dbuf.mv_data, "HEADER=END", STRLENOF("HEADER=END")) == 0)
        {
            break;
        }
        else if (strncmp((char*)dbuf.mv_data, "format=", STRLENOF("format=")) == 0)
        {
            if (strncmp((char*)dbuf.mv_data + STRLENOF("FORMAT="), "print", STRLENOF("print")) == 0)
                mode |= PRINT;
            else if (strncmp((char*)dbuf.mv_data + STRLENOF("FORMAT="), "bytevalue", STRLENOF("bytevalue")) != 0)
            {
                fprintf(stderr,
                        "%s: line %" Yu ": unsupported FORMAT %s\n",
                        prog,
                        lineno,
                        (char*)dbuf.mv_data + STRLENOF("FORMAT="));
                exit(EXIT_FAILURE);
            }
        }
        else if (strncmp((char*)dbuf.mv_data, "database=", STRLENOF("database=")) == 0)
        {
            ptr = (char*)memchr(dbuf.mv_data, '\n', dbuf.mv_size);
            if (ptr != nullptr)
                *ptr = '\0';
            if (subname != nullptr)
                free(subname);
            subname = mdb_strdup((char*)dbuf.mv_data + STRLENOF("database="));
        }
        else if (strncmp((char*)dbuf.mv_data, "type=", STRLENOF("type=")) == 0)
        {
            if (strncmp((char*)dbuf.mv_data + STRLENOF("type="), "btree", STRLENOF("btree")) != 0)
            {
                fprintf(stderr,
                        "%s: line %" Yu ": unsupported type %s\n",
                        prog,
                        lineno,
                        (char*)dbuf.mv_data + STRLENOF("type="));
                exit(EXIT_FAILURE);
            }
        }
        else if (strncmp((char*)dbuf.mv_data, "mapaddr=", STRLENOF("mapaddr=")) == 0)
        {
            int i;
            ptr = (char*)memchr(dbuf.mv_data, '\n', dbuf.mv_size);
            if (ptr != nullptr)
                *ptr = '\0';
#ifdef _WIN32
            i = sscanf_s((char*)dbuf.mv_data + STRLENOF("mapaddr="), "%p", &info.me_mapaddr);
#else
            i = sscanf((char*)dbuf.mv_data + STRLENOF("mapaddr="), "%p", &info.me_mapaddr);
#endif
            if (i != 1)
            {
                fprintf(stderr,
                        "%s: line %" Yu ": invalid mapaddr %s\n",
                        prog,
                        lineno,
                        (char*)dbuf.mv_data + STRLENOF("mapaddr="));
                exit(EXIT_FAILURE);
            }
        }
        else if (strncmp((char*)dbuf.mv_data, "mapsize=", STRLENOF("mapsize=")) == 0)
        {
            int i;
            ptr = (char*)memchr(dbuf.mv_data, '\n', dbuf.mv_size);
            if (ptr != nullptr)
                *ptr = '\0';
#ifdef _WIN32
            i = sscanf_s((char*)dbuf.mv_data + STRLENOF("mapsize="), "%" MDB_SCNy(u), &info.me_mapsize);
#else
            i = sscanf((char*)dbuf.mv_data + STRLENOF("mapsize="), "%" MDB_SCNy(u), &info.me_mapsize);
#endif
            if (i != 1)
            {
                fprintf(stderr,
                        "%s: line %" Yu ": invalid mapsize %s\n",
                        prog,
                        lineno,
                        (char*)dbuf.mv_data + STRLENOF("mapsize="));
                exit(EXIT_FAILURE);
            }
        }
        else if (strncmp((char*)dbuf.mv_data, "maxreaders=", STRLENOF("maxreaders=")) == 0)
        {
            int i;
            ptr = (char*)memchr(dbuf.mv_data, '\n', dbuf.mv_size);
            if (ptr != nullptr)
                *ptr = '\0';
#ifdef _WIN32
            i = sscanf_s((char*)dbuf.mv_data + STRLENOF("maxreaders="), "%u", &info.me_maxreaders);
#else
            i = sscanf((char*)dbuf.mv_data + STRLENOF("maxreaders="), "%u", &info.me_maxreaders);
#endif
            if (i != 1)
            {
                fprintf(stderr,
                        "%s: line %" Yu ": invalid maxreaders %s\n",
                        prog,
                        lineno,
                        (char*)dbuf.mv_data + STRLENOF("maxreaders="));
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            int i;
            for (i = 0; dbflags[i].bit != 0; i++)
            {
                if ((strncmp((char*)dbuf.mv_data, dbflags[i].name, dbflags[i].len) == 0) &&
                    ((char*)dbuf.mv_data)[dbflags[i].len] == '=')
                {
                    flags |= dbflags[i].bit;
                    break;
                }
            }
            if (dbflags[i].bit == 0)
            {
                ptr = (char*)memchr(dbuf.mv_data, '=', dbuf.mv_size);
                if (ptr == nullptr)
                {
                    fprintf(stderr, "%s: line %" Yu ": unexpected format\n", prog, lineno);
                    exit(EXIT_FAILURE);
                }
                else
                {
                    *ptr = '\0';
                    fprintf(stderr,
                            "%s: line %" Yu ": unrecognized keyword ignored: %s\n",
                            prog,
                            lineno,
                            (char*)dbuf.mv_data);
                }
            }
        }
    }
}

static void badend(void)
{
    fprintf(stderr, "%s: line %" Yu ": unexpected end of input\n", prog, lineno);
}

static int unhex(unsigned char* c2)
{
    int x;
    int c;
    x = *c2++ & 0x4f;
    if ((x & 0x40) != 0)
        x -= 55;
    c = x << 4;
    x = *c2 & 0x4f;
    if ((x & 0x40) != 0)
        x -= 55;
    c |= x;
    return c;
}

static int readline(MDB_val* out, MDB_val* buf)
{
    unsigned char* c1;
    unsigned char* c2;
    unsigned char* end;
    size_t len;
    size_t l2;
    int c;

    if ((mode & NOHDR) == 0)
    {
        c = fgetc(stdin);
        if (c == EOF)
        {
            Eof = 1;
            return EOF;
        }
        if (c != ' ')
        {
            lineno++;
            if (fgets((char*)buf->mv_data, (int)buf->mv_size, stdin) == NULL)
            {
            badend:
                Eof = 1;
                badend();
                return EOF;
            }
            if (c == 'D' && (strncmp((char*)buf->mv_data, "ATA=END", STRLENOF("ATA=END")) == 0))
                return EOF;
            goto badend;
        }
    }
    if (fgets((char*)buf->mv_data, (int)buf->mv_size, stdin) == NULL)
    {
        Eof = 1;
        return EOF;
    }
    lineno++;

    c1 = (unsigned char*)buf->mv_data;
    len = strlen((char*)c1);
    l2 = len;

    // Is buffer too short?
    while (c1[len - 1] != '\n')
    {
        void* new_data = realloc(buf->mv_data, buf->mv_size * 2);
        if (new_data == nullptr)
        {
            Eof = 1;
            fprintf(stderr, "%s: line %" Yu ": out of memory, line too long\n", prog, lineno);
            return EOF;
        }
        buf->mv_data = new_data;
        c1 = (unsigned char*)buf->mv_data;
        c1 += l2;
        if (fgets((char*)c1, (int)buf->mv_size + 1, stdin) == NULL)
        {
            Eof = 1;
            badend();
            return EOF;
        }
        buf->mv_size *= 2;
        len = strlen((char*)c1);
        l2 += len;
    }
    c1 = c2 = (unsigned char*)buf->mv_data;
    len = l2;
    c1[--len] = '\0';
    end = c1 + len;

    if ((mode & PRINT) != 0)
    {
        while (c2 < end)
        {
            if (*c2 == '\\')
            {
                if (c2[1] == '\\')
                {
                    *c1++ = *c2;
                }
                else
                {
                    if (c2 + 3 > end || (isxdigit(c2[1]) == 0) || (isxdigit(c2[2]) == 0))
                    {
                        Eof = 1;
                        badend();
                        return EOF;
                    }
                    *c1++ = unhex(++c2);
                }
                c2 += 2;
            }
            else
            {
                // copies are redundant when no escapes were used
                *c1++ = *c2++;
            }
        }
    }
    else
    {
        // odd length not allowed
        if ((len & 1) != 0u)
        {
            Eof = 1;
            badend();
            return EOF;
        }
        while (c2 < end)
        {
            if ((isxdigit(*c2) == 0) || (isxdigit(c2[1]) == 0))
            {
                Eof = 1;
                badend();
                return EOF;
            }
            *c1++ = unhex(c2);
            c2 += 2;
        }
    }
    c2 = (unsigned char*)(out->mv_data = buf->mv_data);
    out->mv_size = c1 - c2;

    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: %s [-V] [-a] [-f input] [-n] [-s name] [-N] [-T] dbpath\n", prog);
    exit(EXIT_FAILURE);
}

static int greater(const MDB_val* a, const MDB_val* b)
{
    return 1;
}

int main(int argc, char* argv[])
{
    int i;
    int rc;
    MDB_env* env;
    MDB_txn* txn;
    MDB_cursor* mc;
    MDB_dbi dbi;
    char* envname;
    int envflags = MDB_NOSYNC;
    int putflags = 0;
    int dohdr = 0;
    int append = 0;
    MDB_val prevk;

    prog = argv[0];

    if (argc < 2)
    {
        usage();
    }

    // simple, portable argument scanner
    int idx = 1;
    while (idx < argc)
    {
        const char* arg = argv[idx];

        // first non-option stops the scan → env-path
        if (arg[0] != '-')
            break;

        // single-letter options identical to original ------------------
        if (strcmp(arg, "-a") == 0)
        {
            append = 1;
        }
        else if (strcmp(arg, "-f") == 0)
        {
            if (++idx == argc)  // need the file name
                usage();
            const char* fname = argv[idx];
#ifdef _WIN32
            FILE* new_stdin;
            errno_t err = freopen_s(&new_stdin, fname, "r", stdin);
            if (err != 0)
            {
                char error_msg[256];
                strerror_s(error_msg, sizeof(error_msg), errno);
                fprintf(stderr, "%s: %s: reopen: %s\n", prog, fname, error_msg);
                return EXIT_FAILURE;
            }
#else
            if (freopen(fname, "r", stdin) == NULL)
            {
                fprintf(stderr, "%s: %s: reopen: %s\n", prog, fname, strerror(errno));
                return EXIT_FAILURE;
            }
#endif
        }
        else if (strcmp(arg, "-n") == 0)
        {
            envflags |= MDB_NOSUBDIR;
        }
        else if (strcmp(arg, "-s") == 0)
        {
            if (++idx == argc)
                usage();
            subname = mdb_strdup(argv[idx]);  // unchanged helper
        }
        else if (strcmp(arg, "-N") == 0)
        {
            putflags = MDB_NOOVERWRITE | MDB_NODUPDATA;
        }
        else if (strcmp(arg, "-Q") == 0)
        {
            envflags |= MDB_NOSYNC;
        }
        else if (strcmp(arg, "-T") == 0)
        {
            mode |= NOHDR | PRINT;
        }
        else if (strcmp(arg, "-V") == 0)
        {
            printf("%s\n", MDB_VERSION_STRING);
            return 0;
        }
        else
        {  // unknown switch
            usage();
        }

        ++idx;  // advance to next argv item
    }

    // after options, exactly one positional argument must remain --------
    if (idx != argc - 1)
        usage();

    envname = argv[idx];

    dbuf.mv_size = 4096;
    dbuf.mv_data = malloc(dbuf.mv_size);

    if ((mode & NOHDR) == 0)
        readhdr();

    rc = mdb_env_create(&env);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_env_create failed, error %d %s\n", rc, mdb_strerror(rc));
        return EXIT_FAILURE;
    }

    mdb_env_set_maxdbs(env, 2);

    if (info.me_maxreaders != 0u)
        mdb_env_set_maxreaders(env, info.me_maxreaders);

    if (info.me_mapsize != 0u)
        mdb_env_set_mapsize(env, info.me_mapsize);

    if (info.me_mapaddr != nullptr)
        envflags |= MDB_FIXEDMAP;

    rc = mdb_env_open(env, envname, envflags, 0664);
    if (rc != 0)
    {
        fprintf(stderr, "mdb_env_open failed, error %d %s\n", rc, mdb_strerror(rc));
        goto env_close;
    }

    kbuf.mv_size = mdb_env_get_maxkeysize(env) * 2 + 2;
    kbuf.mv_data = malloc(kbuf.mv_size * 2);
    k0buf.mv_size = kbuf.mv_size;
    k0buf.mv_data = (char*)kbuf.mv_data + kbuf.mv_size;
    prevk.mv_data = k0buf.mv_data;

    while (Eof == 0)
    {
        MDB_val key;
        MDB_val data;
        int batch = 0;
        int appflag;

        if (dohdr == 0)
        {
            dohdr = 1;
        }
        else if ((mode & NOHDR) == 0)
            readhdr();

        rc = mdb_txn_begin(env, NULL, 0, &txn);
        if (rc != 0)
        {
            fprintf(stderr, "mdb_txn_begin failed, error %d %s\n", rc, mdb_strerror(rc));
            return EXIT_FAILURE;
        }

        rc = mdb_dbi_open(txn, subname, flags | MDB_CREATE, &dbi);
        if (rc != 0)
        {
            fprintf(stderr, "mdb_dbi_open failed, error %d %s\n", rc, mdb_strerror(rc));
            goto txn_abort;
        }
        prevk.mv_size = 0;
        if (append != 0)
        {
            mdb_set_compare(txn, dbi, greater);
            if ((flags & MDB_DUPSORT) != 0)
                mdb_set_dupsort(txn, dbi, greater);
        }

        rc = mdb_cursor_open(txn, dbi, &mc);
        if (rc != 0)
        {
            fprintf(stderr, "mdb_cursor_open failed, error %d %s\n", rc, mdb_strerror(rc));
            goto txn_abort;
        }

        while (true)
        {
            rc = readline(&key, &kbuf);
            if (rc != 0)  // rc == EOF
                break;

            rc = readline(&data, &dbuf);
            if (rc != 0)
            {
                fprintf(stderr, "%s: line %" Yu ": failed to read key value\n", prog, lineno);
                goto txn_abort;
            }

            if (append != 0)
            {
                appflag = MDB_APPEND;
                if ((flags & MDB_DUPSORT) != 0)
                {
                    if (prevk.mv_size == key.mv_size && (memcmp(prevk.mv_data, key.mv_data, key.mv_size) == 0))
                        appflag = MDB_CURRENT | MDB_APPENDDUP;
                    else
                    {
                        memcpy(prevk.mv_data, key.mv_data, key.mv_size);
                        prevk.mv_size = key.mv_size;
                    }
                }
            }
            else
            {
                appflag = 0;
            }
            rc = mdb_cursor_put(mc, &key, &data, putflags | appflag);
            if (rc == MDB_KEYEXIST && (putflags != 0))
                continue;
            if (rc != 0)
            {
                fprintf(stderr,
                        "%s: line %" Yu ": mdb_cursor_put failed, error %d %s\n",
                        prog,
                        lineno,
                        rc,
                        mdb_strerror(rc));
                goto txn_abort;
            }
            batch++;
            if (batch == 100)
            {
                rc = mdb_txn_commit(txn);
                if (rc != 0)
                {
                    fprintf(stderr, "%s: line %" Yu ": txn_commit: %s\n", prog, lineno, mdb_strerror(rc));
                    goto env_close;
                }
                rc = mdb_txn_begin(env, NULL, 0, &txn);
                if (rc != 0)
                {
                    fprintf(stderr, "mdb_txn_begin failed, error %d %s\n", rc, mdb_strerror(rc));
                    goto env_close;
                }
                rc = mdb_cursor_open(txn, dbi, &mc);
                if (rc != 0)
                {
                    fprintf(stderr, "mdb_cursor_open failed, error %d %s\n", rc, mdb_strerror(rc));
                    goto txn_abort;
                }
                if (append != 0)
                {
                    MDB_val k;
                    MDB_val d;
                    mdb_cursor_get(mc, &k, &d, MDB_LAST);
                    memcpy(prevk.mv_data, k.mv_data, k.mv_size);
                    prevk.mv_size = k.mv_size;
                }
                batch = 0;
            }
        }
        rc = mdb_txn_commit(txn);
        txn = NULL;
        if (rc != 0)
        {
            fprintf(stderr, "%s: line %" Yu ": txn_commit: %s\n", prog, lineno, mdb_strerror(rc));
            goto env_close;
        }
        if ((envflags & MDB_NOSYNC) != 0)
        {
            rc = mdb_env_sync(env, 1);
            if (rc != 0)
            {
                fprintf(stderr, "mdb_env_sync failed, error %d %s\n", rc, mdb_strerror(rc));
                goto env_close;
            }
        }
        mdb_dbi_close(env, dbi);
    }

txn_abort:
    mdb_txn_abort(txn);
env_close:
    mdb_env_close(env);

    return (rc != 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

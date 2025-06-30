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

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define mdb_strdup _strdup
#else
#define mdb_strdup strdup
#endif

enum
{
    PRINT = 1,
    NOHDR = 2
};
static int mode;

static char* subname = nullptr;

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
    {.bit = MDB_REVERSEKEY, .name = "reversekey", .len = 10},
    {             .bit = 0,      .name = nullptr,  .len = 0}
};

static void readhdr()
{
    char* ptr;

    flags = 0;
    while (fgets((char*)dbuf.mv_data, (int)dbuf.mv_size, stdin) != nullptr)
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
        else if (strncmp((char*)dbuf.mv_data, "mapsize=", STRLENOF("mapsize=")) == 0)
        {
            ptr = (char*)memchr(dbuf.mv_data, '\n', dbuf.mv_size);
            if (ptr != nullptr)
                *ptr = '\0';
#ifdef _WIN32
            const int mapsize_result =
                sscanf_s((char*)dbuf.mv_data + STRLENOF("mapsize="), "%" MDB_SCNy(u), &info.me_mapsize);
#else
            const int mapsize_result =
                sscanf((char*)dbuf.mv_data + STRLENOF("mapsize="), "%" MDB_SCNy(u), &info.me_mapsize);
#endif
            if (mapsize_result != 1)
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
            ptr = (char*)memchr(dbuf.mv_data, '\n', dbuf.mv_size);
            if (ptr != nullptr)
                *ptr = '\0';
#ifdef _WIN32
            const int maxreaders_result =
                sscanf_s((char*)dbuf.mv_data + STRLENOF("maxreaders="), "%u", &info.me_maxreaders);
#else
            const int maxreaders_result =
                sscanf((char*)dbuf.mv_data + STRLENOF("maxreaders="), "%u", &info.me_maxreaders);
#endif
            if (maxreaders_result != 1)
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
            int flag_idx;
            for (flag_idx = 0; dbflags[flag_idx].bit != 0; flag_idx++)
            {
                if ((strncmp((char*)dbuf.mv_data, dbflags[flag_idx].name, dbflags[flag_idx].len) == 0) &&
                    ((char*)dbuf.mv_data)[dbflags[flag_idx].len] == '=')
                {
                    flags |= dbflags[flag_idx].bit;
                    break;
                }
            }
            if (dbflags[flag_idx].bit == 0)
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

static void badend()
{
    fprintf(stderr, "%s: line %" Yu ": unexpected end of input\n", prog, lineno);
}

static auto unhex(unsigned char* c2) -> int
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

static auto readline(MDB_val* out, MDB_val* buf) -> int
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
            if (fgets((char*)buf->mv_data, (int)buf->mv_size, stdin) == nullptr)
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
    if (fgets((char*)buf->mv_data, (int)buf->mv_size, stdin) == nullptr)
    {
        Eof = 1;
        return EOF;
    }
    lineno++;

    auto* const initial_data = (unsigned char*)buf->mv_data;
    const size_t initial_len = strlen((char*)initial_data);
    size_t total_len = initial_len;

    // Is buffer too short?
    unsigned char* current_pos = initial_data;
    while (current_pos[initial_len - 1] != '\n')
    {
        void* new_data = realloc(buf->mv_data, buf->mv_size * 2);
        if (new_data == nullptr)
        {
            Eof = 1;
            fprintf(stderr, "%s: line %" Yu ": out of memory, line too long\n", prog, lineno);
            return EOF;
        }
        buf->mv_data = new_data;
        auto* const buffer_base = (unsigned char*)buf->mv_data;
        unsigned char* const append_pos = buffer_base + total_len;
        if (fgets((char*)append_pos, (int)buf->mv_size + 1, stdin) == nullptr)
        {
            Eof = 1;
            badend();
            return EOF;
        }
        buf->mv_size *= 2;
        const size_t append_len = strlen((char*)append_pos);
        total_len += append_len;
        current_pos = buffer_base;
    }
    auto* const source_ptr = (unsigned char*)buf->mv_data;
    unsigned char* dest_ptr = source_ptr;
    const size_t final_len = total_len - 1;  // Remove newline
    source_ptr[final_len] = '\0';
    end = source_ptr + final_len;

    // TODO: This function has extensive variable re-purposing and should be refactored
    // Variables c1, c2, len are re-purposed multiple times for different contexts
    c1 = dest_ptr;
    c2 = source_ptr;
    len = final_len;

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
        if ((len & 1) != 0U)
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

static void usage()
{
    fprintf(stderr, "usage: %s [-V] [-a] [-f input] [-n] [-s name] [-N] [-T] dbpath\n", prog);
    exit(EXIT_FAILURE);
}

static auto greater(const MDB_val* a, const MDB_val* b) -> int
{
    return 1;
}

auto main(int argc, char* argv[]) -> int
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
            putflags = MDB_NOOVERWRITE;
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

    if (info.me_maxreaders != 0U)
        mdb_env_set_maxreaders(env, info.me_maxreaders);

    if (info.me_mapsize != 0U)
        mdb_env_set_mapsize(env, info.me_mapsize);

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

        rc = mdb_txn_begin(env, nullptr, 0, &txn);
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
                const int commit_rc = mdb_txn_commit(txn);
                if (commit_rc != 0)
                {
                    fprintf(stderr, "%s: line %" Yu ": txn_commit: %s\n", prog, lineno, mdb_strerror(commit_rc));
                    rc = commit_rc;
                    goto env_close;
                }
                const int begin_rc = mdb_txn_begin(env, nullptr, 0, &txn);
                if (begin_rc != 0)
                {
                    fprintf(stderr, "mdb_txn_begin failed, error %d %s\n", begin_rc, mdb_strerror(begin_rc));
                    rc = begin_rc;
                    goto env_close;
                }
                const int cursor_rc = mdb_cursor_open(txn, dbi, &mc);
                if (cursor_rc != 0)
                {
                    fprintf(stderr, "mdb_cursor_open failed, error %d %s\n", cursor_rc, mdb_strerror(cursor_rc));
                    rc = cursor_rc;
                    goto txn_abort;
                }
                if (append != 0)
                {
                    MDB_val last_key;
                    MDB_val last_data;
                    mdb_cursor_get(mc, &last_key, &last_data, MDB_LAST);
                    memcpy(prevk.mv_data, last_key.mv_data, last_key.mv_size);
                    prevk.mv_size = last_key.mv_size;
                }
                batch = 0;
            }
        }
        rc = mdb_txn_commit(txn);
        txn = nullptr;
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

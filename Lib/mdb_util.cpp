#include "mdb_util.h"

#include "lmdb.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

// Return the library version info.
const char* mdb_version(int* major, int* minor, int* patch)
{
    if (major)
        *major = MDB_VERSION_MAJOR;
    if (minor)
        *minor = MDB_VERSION_MINOR;
    if (patch)
        *patch = MDB_VERSION_PATCH;
    return MDB_VERSION_STRING;
}

// Table of descriptions for LMDB errors
static const char* const mdb_errstr[] = {
    "MDB_KEYEXIST: Key/data pair already exists",
    "MDB_NOTFOUND: No matching key/data pair found",
    "MDB_PAGE_NOTFOUND: Requested page not found",
    "MDB_CORRUPTED: Located page was wrong type",
    "MDB_PANIC: Update of meta page failed or environment had a fatal error",
    "MDB_VERSION_MISMATCH: DB file version mismatch with expected version",
    "MDB_INVALID: File is not an MDB file",
    "MDB_MAP_FULL: Environment mapsize limit reached",
    "MDB_DBS_FULL: Environment maxdbs limit reached",
    "MDB_READERS_FULL: Environment maxreaders limit reached",
    "MDB_TLS_FULL: Thread-local storage keys full - too many environments open",
    "MDB_TXN_FULL: Transaction has too many dirty pages",
    "MDB_CURSOR_FULL: Internal error - cursor stack limit reached",
    "MDB_PAGE_FULL: Internal error - page has no more space",
    "MDB_MAP_RESIZED: Database contents grew beyond environment mapsize",
    "MDB_INCOMPATIBLE: Operation and DB incompatible, or DB type changed",
    "MDB_BAD_RSLOT: Invalid reuse of reader locktable slot",
    "MDB_BAD_TXN: Transaction must abort, has a child, or is invalid",
    "MDB_BAD_VALSIZE: Unsupported size of key/data for target DB",
    "MDB_BAD_DBI: The specified DBI was changed unexpectedly",
    "MDB_PROBLEM: Unexpected problem - txn should abort",
    "MDB_LAST_ERRCODE: MDB_LAST_ERRCODE",
};

const char* mdb_strerror(int err)
{
#ifdef _WIN32
    // HACK: pad 4KB on stack over the buf. Return system msgs in buf.
    // This works as long as no function between the call to mdb_strerror
    // and the actual use of the message uses more than 4K of stack.
#define MSGSIZE 1024
#define PADSIZE 4096
    static char buf[MSGSIZE + PADSIZE], *ptr = buf;
#endif
    int i;
    if (!err)
        return ("Successful return: 0");

    if (err >= MDB_KEYEXIST && err <= MDB_LAST_ERRCODE)
    {
        i = err - MDB_KEYEXIST;
        return mdb_errstr[i];
    }

#ifdef _WIN32
    // These are the C-runtime error codes we use. The comment indicates
    // their numeric value, and the Win32 error they would correspond to
    // if the error actually came from a Win32 API. A major mess, we should
    // have used LMDB-specific error codes for everything.
    switch (err)
    {
    case ENOENT:  // 2, FILE_NOT_FOUND
    case EIO:     // 5, ACCESS_DENIED
    case ENOMEM:  // 12, INVALID_ACCESS
    case EACCES:  // 13, INVALID_DATA
    case EBUSY:   // 16, CURRENT_DIRECTORY
    case EINVAL:  // 22, BAD_COMMAND
    case ENOSPC:  // 28, OUT_OF_PAPER
        {
            static char errbuf[256];
            if (strerror_s(errbuf, sizeof(errbuf), err) == 0)
                return errbuf;
            return "Unknown error";
        }
    default:;
    }
    buf[0] = 0;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, ptr, MSGSIZE, NULL);
    return ptr;
#else
    if (err < 0)
        return "Invalid error code";
    return strerror(err);
#endif
}

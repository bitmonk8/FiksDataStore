#include "util.h"

#include "fds.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

// Return the library version info.
auto fds_version(int* major, int* minor, int* patch) -> const char*
{
    if (major != nullptr)
        *major = FDS_VERSION_MAJOR;
    if (minor != nullptr)
        *minor = FDS_VERSION_MINOR;
    if (patch != nullptr)
        *patch = FDS_VERSION_PATCH;
    return FDS_VERSION_STRING;
}

// Table of descriptions for LMDB errors
static const char* const fds_errstr[] = {
    "FDS_KEYEXIST: Key/data pair already exists",
    "FDS_NOTFOUND: No matching key/data pair found",
    "FDS_PAGE_NOTFOUND: Requested page not found",
    "FDS_CORRUPTED: Located page was wrong type",
    "FDS_PANIC: Update of meta page failed or environment had a fatal error",
    "FDS_VERSION_MISMATCH: DB file version mismatch with expected version",
    "FDS_INVALID: File is not an MDB file",
    "FDS_MAP_FULL: Environment mapsize limit reached",
    "FDS_DBS_FULL: Environment maxdbs limit reached",
    "FDS_READERS_FULL: Environment maxreaders limit reached",
    "FDS_TLS_FULL: Thread-local storage keys full - too many environments open",
    "FDS_TXN_FULL: Transaction has too many dirty pages",
    "FDS_CURSOR_FULL: Internal error - cursor stack limit reached",
    "FDS_PAGE_FULL: Internal error - page has no more space",
    "FDS_MAP_RESIZED: Database contents grew beyond environment mapsize",
    "FDS_INCOMPATIBLE: Operation and DB incompatible, or DB type changed",
    "FDS_BAD_RSLOT: Invalid reuse of reader locktable slot",
    "FDS_BAD_TXN: Transaction must abort, has a child, or is invalid",
    "FDS_BAD_VALSIZE: Unsupported size of key/data for target DB",
    "FDS_BAD_DBI: The specified DBI was changed unexpectedly",
    "FDS_PROBLEM: Unexpected problem - txn should abort",
    "FDS_LAST_ERRCODE: FDS_LAST_ERRCODE",
};

auto fds_strerror(int err) -> const char*
{
#ifdef _WIN32
    // HACK: pad 4KB on stack over the buf. Return system msgs in buf.
    // This works as long as no function between the call to fds_strerror
    // and the actual use of the message uses more than 4K of stack.
#define MSGSIZE 1024
#define PADSIZE 4096
    static char buf[MSGSIZE + PADSIZE];
    static char* ptr = buf;
#endif
    if (err == 0)
        return ("Successful return: 0");

    if (err >= FDS_KEYEXIST && err <= FDS_LAST_ERRCODE)
    {
        return fds_errstr[err - FDS_KEYEXIST];
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
        static char errbuf[256]{};
        if (strerror_s(errbuf, sizeof(errbuf), err) == 0)
            return errbuf;
        return "Unknown error";
    }
    default:;
    }
    buf[0] = 0;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, ptr, MSGSIZE, nullptr);
    return ptr;
#else
    if (err < 0)
        return "Invalid error code";
    return strerror(err);
#endif
}

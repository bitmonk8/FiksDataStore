#pragma once

#include "internal.h"

#if defined(FDS_WINDOWS)
enum Pidlock_op : int
{
    Pidset,
    Pidcheck
};
#else
enum Pidlock_op : int
{
    Pidset = F_SETLK,
    Pidcheck = F_GETLK
};
#endif

// The information we store in a single slot of the reader table.
// In addition to a transaction ID, we also record the process and
// thread ID that owns a slot, so that we can detect stale information,
// e.g. threads or processes that went away without cleaning up.
// We currently don't check for stale records. We simply re-init
// the table when we know that we're the only process opening the
// lock file.
struct FDS_rxbody
{
    // Current Transaction ID when this transaction began, or (txnid_t)-1.
    // Multiple readers that start at the same time will probably have the
    // same ID here. Again, it's not important to exclude them from
    // anything; all we need to know is which version of the DB they
    // started from so we can avoid overwriting any data used in that
    // particular version.
    volatile txnid_t mrb_txnid;
    // The process ID of the process owning this reader txn.
    volatile FDS_PID_T mrb_pid;
    // The thread ID of the thread owning this txn.
    volatile FDS_THR_T mrb_tid;
};

// The actual reader record, with cacheline padding.
struct FDS_reader
{
    union
    {
        FDS_rxbody mrx;
        // shorthand for mrb_txnid
#define mr_txnid mru.mrx.mrb_txnid
#define mr_pid mru.mrx.mrb_pid
#define mr_tid mru.mrx.mrb_tid
        // cache line alignment
        char pad[(sizeof(FDS_rxbody) + CACHELINE - 1) & ~(CACHELINE - 1)];
    } mru;
};

// Lock mutex, handle any error, set rc = result.
// Return 0 on success, nonzero (not rc) on error.
#define LOCK_MUTEX(rc, env, mutex) (((rc) = LOCK_MUTEX0(mutex)) && ((rc) = fds_mutex_failed(env, mutex, rc)))

auto fds_mutex_failed(FDS_env* env, fds_mutexref_t mutex, int rc) -> int;
auto fds_reader_pid(FDS_env* env, enum Pidlock_op op, FDS_PID_T pid) -> int;
auto fds_reader_check0(FDS_env* env, int rlocked, int* dead) -> int;

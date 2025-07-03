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
        char pad[(sizeof(FDS_rxbody) + CACHELINE - 1) & ~(CACHELINE - 1)]; // cache line alignment
    };
};


#if defined(FDS_WINDOWS)

#define LOCK_MUTEX0(mutex) WaitForSingleObject(mutex, INFINITE)

#define UNLOCK_MUTEX(mutex) ReleaseMutex(mutex)

#define fds_mutex_consistent(mutex) 0

#elif defined(FDS_MACOS)

int fds_sem_wait(fds_mutexref_t sem);

#define LOCK_MUTEX0(mutex) fds_sem_wait(mutex)

#define UNLOCK_MUTEX(mutex)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        struct sembuf sb = {0, 1, SEM_UNDO};                                                                           \
        sb.sem_num = (mutex)->semnum;                                                                                  \
        *(mutex)->locked = 0;                                                                                          \
        semop((mutex)->semid, &sb, 1);                                                                                 \
    } while (0)

#define fds_mutex_consistent(mutex) 0

#elif defined(FDS_LINUX)

// Lock the reader or writer mutex.
// Returns 0 or a code to give fds_mutex_failed(), as in LOCK_MUTEX().
#define LOCK_MUTEX0(mutex) pthread_mutex_lock(mutex)

// Unlock the reader or writer mutex.
#define UNLOCK_MUTEX(mutex) pthread_mutex_unlock(mutex)

// Mark mutex-protected data as repaired, after death of previous owner.
#define fds_mutex_consistent(mutex) pthread_mutex_consistent(mutex)

#else

#error "Unknown platform for mutex locking"

#endif


// Lock mutex, handle any error, set rc = result.
// Return 0 on success, nonzero (not rc) on error.
#define LOCK_MUTEX(rc, env, mutex) (((rc) = LOCK_MUTEX0(mutex)) && ((rc) = fds_mutex_failed(env, mutex, rc)))

auto fds_mutex_failed(FDS_env* env, fds_mutexref_t mutex, int rc) -> int;
auto fds_reader_pid(FDS_env* env, enum Pidlock_op op, FDS_PID_T pid) -> int;
auto fds_reader_check0(FDS_env* env, int rlocked, int* dead) -> int;

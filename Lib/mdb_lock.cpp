#include "mdb_lock.h"

#include <utility>

#include "mdb_debug.h"
#include "mdb_env.h"

#ifdef _WIN32
#define MDB_OWNERDEAD ((int)WAIT_ABANDONED)
#elif defined MDB_USE_SYSV_SEM
#define MDB_OWNERDEAD (MDB_LAST_ERRCODE + 11)
#elif defined(MDB_USE_POSIX_MUTEX)
#define MDB_OWNERDEAD EOWNERDEAD /* LOCK_MUTEX0() result if dead owner */
#endif

// Set or check a pid lock. Set returns 0 on success.
// Check returns 0 if the process is certainly dead, nonzero if it may
// be alive (the lock exists or an error happened so we do not know).
//
// On Windows Pidset is a no-op, we merely check for the existence
// of the process with the given pid. On POSIX we use a single byte
// lock on the lockfile, set at an offset equal to the pid.
auto mdb_reader_pid(MDB_env* env, enum Pidlock_op op, MDB_PID_T pid) -> int
{
#if !(MDB_PIDLOCK) /* Currently the same as defined(_WIN32) */
    if (op == Pidcheck)
    {
        HANDLE h{OpenProcess(env->me_pidquery, FALSE, pid)};
        // No documented "no such process" code, but other program use this:
        if (h == nullptr)
            return ErrCode() != ERROR_INVALID_PARAMETER;
        // A process exists until all handles to it close. Has it exited?
        int ret{static_cast<int>(WaitForSingleObject(h, 0) != 0)};
        CloseHandle(h);
        return ret;
    }
    return 0;
#else
    for (;;)
    {
        struct flock lock_info{};
        lock_info.l_type = F_WRLCK;
        lock_info.l_whence = SEEK_SET;
        lock_info.l_start = pid;
        lock_info.l_len = 1;
        int rc{fcntl(env->me_lfd, op, &lock_info)};
        if (rc == 0)
        {
            if (op == F_GETLK && lock_info.l_type != F_UNLCK)
                rc = -1;
        }
        else if ((rc = ErrCode()) == EINTR)
        {
            continue;
        }
        return rc;
    }
#endif
}

auto ESECT mdb_reader_list(MDB_env* env, MDB_msg_func func, void* ctx) -> int
{
    if ((env == nullptr) || (func == nullptr))
        return -1;
    if (env->me_txns == nullptr)
    {
        return func("(no reader locks)\n", ctx);
    }

    unsigned int rdrs{env->me_txns->mti_numreaders};
    MDB_reader* mr{env->me_txns->mti_readers};
    int rc{0};
    int first{1};

    for (unsigned int i{0}; i < rdrs; i++)
    {
        if (mr[i].mr_pid != 0)
        {
            txnid_t txnid{mr[i].mr_txnid};
            char buf[64]{};
            snprintf(buf,
                     sizeof(buf),
                     (txnid == -1) ? "%10d %" Z "x -\n" : "%10d %" Z "x %" Yu "\n",
                     (int)mr[i].mr_pid,
                     (size_t)mr[i].mr_tid,
                     txnid);
            if (first != 0)
            {
                first = 0;
                rc = func("    pid     thread     txnid\n", ctx);
                if (rc < 0)
                    break;
            }
            rc = func(buf, ctx);
            if (rc < 0)
                break;
        }
    }
    if (first != 0)
    {
        rc = func("(no active readers)\n", ctx);
    }
    return rc;
}

// Insert pid into list if not already present.
// return -1 if already present.
static auto ESECT mdb_pid_insert(MDB_PID_T* ids, MDB_PID_T pid) -> int
{
    // binary search of pid in list
    unsigned base{0};
    unsigned cursor{1};
    int val{0};
    unsigned n = ids[0];

    while (0 < n)
    {
        unsigned pivot{n >> 1};
        cursor = base + pivot + 1;
        val = pid - ids[cursor];

        if (val < 0)
        {
            n = pivot;
        }
        else if (val > 0)
        {
            base = cursor;
            n -= pivot + 1;
        }
        else
        {
            // found, so it's a duplicate
            return -1;
        }
    }

    if (val > 0)
    {
        ++cursor;
    }
    ids[0]++;
    for (n = ids[0]; n > cursor; n--)
        ids[n] = ids[n - 1];
    ids[n] = pid;
    return 0;
}

auto ESECT mdb_reader_check(MDB_env* env, int* dead) -> int
{
    if (env == nullptr)
        return EINVAL;
    if (dead != nullptr)
        *dead = 0;
    return (env->me_txns != nullptr) ? mdb_reader_check0(env, 0, dead) : MDB_SUCCESS;
}

// Handle #LOCK_MUTEX0() failure.
// Try to repair the lock file if the mutex owner died.
// env: the environment handle
// mutex: LOCK_MUTEX0() mutex
// rc: LOCK_MUTEX0() error (nonzero)
// Returns 0 on success with the mutex locked, or an error code on failure.
auto ESECT mdb_mutex_failed(MDB_env* env, mdb_mutexref_t mutex, int rc) -> int
{
    if (rc == MDB_OWNERDEAD)
    {
        // We own the mutex. Clean up after dead previous owner.
        int cleanup_result{MDB_SUCCESS};
        const int rlocked{static_cast<int>(mutex == env->me_rmutex)};
        if (rlocked == 0)
        {
            // Keep mti_txnid updated, otherwise next writer can
            // overwrite data which latest meta page refers to.
            MDB_meta* meta{mdb_env_pick_meta(env)};
            env->me_txns->mti_txnid = meta->mm_txnid;
            // env is hosed if the dead thread was ours
            if (env->me_txn != nullptr)
            {
                env->me_flags |= MDB_FATAL_ERROR;
                env->me_txn = nullptr;
                cleanup_result = MDB_PANIC;
            }
        }
        DPRINTF(("%cmutex owner died, %s",
                 (rlocked ? 'r' : 'w'),
                 (cleanup_result ? "this process' env is hosed" : "recovering")));
        const int reader_check_result{mdb_reader_check0(env, rlocked, nullptr)};
        int consistency_result = reader_check_result;
        if (reader_check_result == 0)
            consistency_result = mdb_mutex_consistent(mutex);
        if (cleanup_result == 0)
            cleanup_result = consistency_result;
        if (cleanup_result != 0)
        {
            DPRINTF(("LOCK_MUTEX recovery failed, %s", mdb_strerror(cleanup_result)));
            UNLOCK_MUTEX(mutex);
        }
        return cleanup_result;
    }

#ifdef _WIN32
    const int error_code = ErrCode();
    DPRINTF(("LOCK_MUTEX failed, %s", mdb_strerror(error_code)));
    return error_code;
#else
    DPRINTF(("LOCK_MUTEX failed, %s", mdb_strerror(rc)));
    return rc;
#endif
}

// As #mdb_reader_check(). rlocked is set if caller locked #me_rmutex.
auto ESECT mdb_reader_check0(MDB_env* env, int rlocked, int* dead) -> int
{
    mdb_mutexref_t rmutex{(rlocked != 0) ? nullptr : env->me_rmutex};
    unsigned int rdrs{env->me_txns->mti_numreaders};
    MDB_PID_T* pids{(MDB_PID_T*)malloc((rdrs + 1) * sizeof(MDB_PID_T))};
    if (pids == nullptr)
        return ENOMEM;
    pids[0] = 0;
    MDB_reader* mr{env->me_txns->mti_readers};
    int rc{MDB_SUCCESS};
    int count{0};

    for (unsigned int i{0}; i < rdrs; i++)
    {
        MDB_PID_T pid{mr[i].mr_pid};
        if ((pid != 0) && pid != env->me_pid)
        {
            if (mdb_pid_insert(pids, pid) == 0)
            {
                if (mdb_reader_pid(env, Pidcheck, pid) == 0)
                {
                    // Stale reader found
                    unsigned int j{i};
                    if (rmutex != nullptr)
                    {
                        rc = LOCK_MUTEX0(rmutex);
                        if (rc != 0)
                        {
                            rc = mdb_mutex_failed(env, rmutex, rc);
                            if (rc != 0)
                                break;
                            rdrs = 0;  // the above checked all readers
                        }
                        else
                        {
                            // Recheck, a new process may have reused pid
                            if (mdb_reader_pid(env, Pidcheck, pid) != 0)
                                j = rdrs;
                        }
                    }
                    for (; j < rdrs; j++)
                        if (mr[j].mr_pid == pid)
                        {
                            DPRINTF(("clear stale reader pid %u txn %" Yd, (unsigned)pid, mr[j].mr_txnid));
                            mr[j].mr_pid = 0;
                            count++;
                        }
                    if (rmutex != nullptr)
                        UNLOCK_MUTEX(rmutex);
                }
            }
        }
    }
    free(pids);
    if (dead != nullptr)
        *dead = count;
    return rc;
}

#ifndef _WIN32

#ifdef MDB_USE_POSIX_SEM

int mdb_sem_wait(sem_t* sem)
{
    int rc;
    while ((rc = sem_wait(sem)) && (rc = errno) == EINTR)
        ;
    return rc;
}

#elif defined MDB_USE_SYSV_SEM

int mdb_sem_wait(mdb_mutexref_t sem)
{
    int* locked{sem->locked};
    struct sembuf sb{0, -1, SEM_UNDO};
    sb.sem_num = sem->semnum;
    int rc{};
    do
    {
        if (!semop(sem->semid, &sb, 1))
        {
            rc = *locked ? MDB_OWNERDEAD : MDB_SUCCESS;
            *locked = 1;
            break;
        }
    } while ((rc = errno) == EINTR);
    return rc;
}

#endif

#endif

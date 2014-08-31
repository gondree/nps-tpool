/*! \file      thr_pool.c
**  \brief     The NPS Pthread Pool implementation
*/
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sched.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include "thr_pool.h"

//! debug level
#ifndef DEBUG_THR_POOL
#define DEBUG_THR_POOL 0
#endif

#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
#include <mach/clock.h>
#include <mach/mach.h>
#define clock_gettime(a,b)  clock_get_time(REALTIME_CLOCK,b);
#endif


//
// debug-related macros
//

//! Prints to stderr "TID: file:line  <string>\n"
#define dbugf(a, s) \
do { \
    if (a) { \
        fprintf(stderr, "%5ld: ", pthread_self()); \
        fprintf(stderr, "%s:%d ", __FUNCTION__, __LINE__); \
        fprintf(stderr, "%s\n", s); \
        fflush(stderr); \
    } \
} while(0)

//! Prints to stderr "TID: file:line  <string> <integer>\n"
#define dbugdf(a, s, d) \
do { \
    if (a) { \
        fprintf(stderr, "%5ld: ", pthread_self()); \
        fprintf(stderr, "%s:%d ", __FUNCTION__, __LINE__); \
        fprintf(stderr, "%s %d\n", s, d); \
        fflush(stderr); \
    } \
} while(0)

//! Prints to stderr "TID: file:line  <format string>\n"
#define dbugg(a, f, ...)\
do { \
    if (a) { \
        fprintf(stderr, "%5ld: ", pthread_self()); \
        fprintf(stderr, "%s:%d ", __FUNCTION__, __LINE__); \
        fprintf(stderr, f, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        fflush(stderr); \
    } \
} while(0)


//
// internal module constants
//
#define THR_POOL_COPY_MODE          0x01    //!< flag: job input is copied

#define THR_POOL_DESTROY_REQUEST    0x01    //!< destroying pool
#define THR_POOL_WAKE_WORKER        0x02    //!< idle worker has been signaled
#define THR_POOL_WORKER_WOKE        0x04    //!< ack from worker that woke


//! The "initialized" module state
#define THR_POOL_INITIALIZED             (THR_POOL_LAST - 1)
//! Internal error: a lock-related error
#define THR_POOL_LOCK_ERROR              (THR_POOL_LAST - 2)  
//! Internal error: a lock-related error that affects the pool
#define THR_POOL_LOCK_POOL_ERROR         (THR_POOL_LAST - 3)
//! Internal error: a pthread-related error
#define THR_POOL_PTHREAD_ERROR           (THR_POOL_LAST - 4)
//! Internal error: a pthread-related error that affects the pool
#define THR_POOL_PTHREAD_POOL_ERROR      (THR_POOL_LAST - 5)
//! Internal error: a memory-related error
#define THR_POOL_MEM_ERROR               (THR_POOL_LAST - 6)
//! Internal error: a memory-related error that affects the pool
#define THR_POOL_MEM_POOL_ERROR          (THR_POOL_LAST - 7)


typedef struct job job_t;            //!< Simplifies defining linked lists
typedef struct thr_pool thr_pool_t;  //!< Simplifies defining lists of pools
typedef void (*push_fn_t)(void *);   //!< Simplifies casting function pointers


//! Linked list of queued jobs
struct job {
    job_t *next;            //!< linked list holding each job
    void *(*func)(void *);  //!< function to call
    void *arg;              //!< argument for job::func()
    int mode;               //!< was the job::arg copied or not
};


//! State for a Thread Pool
struct thr_pool {
    thr_pool_handle_t handle;    //!< pool handle
    thr_pool_t        *next;     //!< list to other pools
    pthread_mutex_t   lock;      //!< protects all (the following) pool data
    pthread_cond_t    jobcv;     //!< for idle workers to wait on for jobs
    pthread_cond_t    emptycv;   //!< for caller to wait, while pool non-empty
    pthread_cond_t    wakecv;    //!< for caller to wait, for ack from worker
    pthread_t         *tid;      //!< array of all threads in the pool
    int               *active;   //!< array, if active[j] then tid[j] is active
    job_t             *job_head; //!< head of job queue
    job_t             *job_tail; //!< tail of job queue
    int               notice;    //!< notifications for the pool
    pthread_attr_t    attr;      //!< POSIX thread attributes for workers
    unsigned int      timeout;   //!< seconds before idle workers exit
    unsigned int      min;       //!< minimum number of worker threads
    unsigned int      max;       //!< maximum number of worker threads
    unsigned int      nthreads;  //!< current number of worker threads
    unsigned int      nidle;     //!< number of idle workers
#if (DEBUG_THR_POOL > 0)
    pid_t             *pid;      //!< all threads in the pool
#endif
};


//
// Module Data
//

//! \brief Module state. One of:
//!  THR_POOL_INITIALIZED, THR_POOL_NOT_INITIALIZED, THR_POOL_UNRECOV_ERROR
static int thr_pools_state = THR_POOL_NOT_INITIALIZED;

//! Module state: remembers if the atfork handler was installed
static int thr_pools_atfork = 0;

//! List of pools
static thr_pool_t *thr_pools = NULL;

//! Next valid handle
static thr_pool_handle_t next_pool_handle = -1;

//! A signal set filled with all signals
static sigset_t fillset;

//! \brief Module lock. Protects: 
//!   job::next pointers, ::thr_pools, ::thr_pools_state, ::next_pool_handle
static pthread_rwlock_t thr_pools_lock = PTHREAD_RWLOCK_INITIALIZER;


//
// Internal Function Prototypes
//

static int thrint_create_worker(thr_pool_t *pool);
static void *thrint_worker_function(void *arg);
static int thrint_error_check(int error);
static int thrint_pool_destroy(thr_pool_t *pool);
    // cleanup handlers 
static void thrint_job_free_handler(job_t *job);
static void thrint_job_cleanup_handler(thr_pool_t *pool);
static void thrint_worker_cleanup_handler(thr_pool_t *pool);
    // fork handlers
static void thrint_lock_all(void);
static void thrint_unlock_all(void); 
static void thrint_fork_child_handler(void);



#if (DEBUG_THR_POOL > 0)
//! For debugging. Prints state of module. (Requires lock for accuracy.)
//!
//! @return None
//!
static void thrint_module_state(void)
{
    int len, rows, idx;
    char *buf[4];
    int bufl[4] = {0, 0, 0, 0};
    thr_pool_t* next;

    rows = 4;
    len = (THR_POOL_MAX_THREADS*10 + 20)*sizeof(char);
    for(idx=0; idx<rows; idx++) {
        buf[idx] = malloc(len);
    }

    dbugf(1, "thr_pool state");
    dbugf(1, "==============");
    dbugdf(1, "thr_pools_state = ", thr_pools_state);
    dbugdf(1, "next_pool_handle = ", next_pool_handle);

    next = thr_pools;
    while (next != NULL) {
        for(idx=0; idx<rows; idx++) {
            memset(buf[idx], '\0', len);
        }

        dbugdf(1, "   Pool #", next->handle);
        dbugf(1,  "   -------");
        dbugdf(1, "   nthreads = ", next->nthreads);
        dbugdf(1, "   nidle = ", next->nidle);
        dbugg(1, "   notice = %s %s %s %s", 
                 (next->notice == 0) ? "none." : "",
                 (next->notice & THR_POOL_DESTROY_REQUEST) ? "destroy" : "",
                 (next->notice & THR_POOL_WAKE_WORKER) ? "wake" : "",
                 (next->notice & THR_POOL_WORKER_WOKE) ? "woke" : "");
        dbugdf(1, "   timeout = ", next->timeout);
        dbugdf(1, "   min = ", next->min);
        dbugdf(1, "   max = ", next->max);

        bufl[0] += snprintf(buf[0], len, "   No.   : ");
        bufl[1] += snprintf(buf[1], len, "   Active: ");
        bufl[2] += snprintf(buf[2], len, "   TID   : ");
        bufl[3] += snprintf(buf[3], len, "   PID   : ");
        for(idx=0; idx < next->max; idx++) {
            // only print non-empty thread data
            if (!((next->active[idx] == 0) && (next->tid[idx] == -1))) {
                bufl[0] += snprintf(buf[0]+bufl[0], len - bufl[0], 
                                    " %7d", idx);
                bufl[1] += snprintf(buf[1]+bufl[1], len - bufl[1],
                                    " %7d", next->active[idx]);
                bufl[2] += snprintf(buf[2]+bufl[2], len - bufl[2], 
                                    " %7d", next->tid[idx]);
                bufl[3] += snprintf(buf[3]+bufl[3], len - bufl[3], 
                                   " %7d", next->pid[idx]);
            }
        }
        for(idx=0; idx<rows; idx++) {
            dbugg(1, "%s", buf[idx]);
        }        
        next = next->next;
    } // while

    for(idx=0; idx<rows; idx++) {
        if (buf[idx]) {
            free(buf[idx]);
            buf[idx] = NULL;
        }
    }
} // thrint_module_state
#endif // DEBUG_THR_POOL



//! A post-fork handler for the parent.
//!    Returns all locks.
//!
//! @return None
//!
static void thrint_unlock_all(void)
{
    int result = 0;
    thr_pool_t *pool = thr_pools;

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "inside post-fork parent handler");
#endif // DEBUG_THR_POOL

    while (pool != NULL) {
        if (pthread_mutex_unlock(&(pool->lock)) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_mutex_unlock");
        }
        pool = pool->next;
    }
    if (pthread_rwlock_unlock(&thr_pools_lock) != 0) {
        result = THR_POOL_LOCK_ERROR;
        dbugf(1, "error: pthread_rwlock_unlock");
    }

    if (result != 0) {
        thrint_error_check(result);
    }
} // thrint_unlock_all



//! A pre-fork handler.
//!    Holds all locks, so they don't enter a bad state during a fork.
//!
//! @return None.
//!
static void thrint_lock_all(void)
{
    int result = 0;
    thr_pool_t *pool = thr_pools;

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "inside pre-fork handler");
#endif // DEBUG_THR_POOL

    if (pthread_rwlock_wrlock(&thr_pools_lock) != 0) {
        result = THR_POOL_LOCK_ERROR;
        dbugf(1, "error: pthread_rwlock_wrlock");
        
    }
    while (pool != NULL) {
        if (pthread_mutex_lock(&(pool->lock)) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_mutex_lock");
        }
        pool = pool->next;
    }
    
    if (result != 0) {
        thrint_unlock_all();
        thrint_error_check(result);
    }
} // thrint_lock_all



//! A post-fork handler for the child.
//!    Puts the child's copy of the module into a sensible state.
//     No matter who forked, the child's module gets reset.
//!
//! @return None.
//!
static void thrint_fork_child_handler(void)
{
    int result = 0;
    thr_pool_t *pool, *poolp;
    job_t *job, *jobp;

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "inside post-fork child handler");
#endif // DEBUG_THR_POOL

    // go through each pool
    pool = thr_pools;
    while (pool != NULL) {
        // free data malloc'd for queued jobs
        job = pool->job_head;
        while(job != NULL) {
            jobp = job;
            job = job->next;
            thrint_job_free_handler(jobp);
        }
        pool->job_head = NULL;
        pool->job_tail = NULL;
        
        // free list of threads
        free(pool->active);
        free(pool->tid);
#if (DEBUG_THR_POOL > 0)
        free(pool->pid);
#endif // DEBUG_THR_POOL

        // destroy attr
        if (pthread_attr_destroy(&(pool->attr)) != 0) {
            dbugdf(1, "error: attr_destroy, errno", errno);
        }
        // destroy each cv
        if ((pthread_cond_destroy(&(pool->jobcv)) != 0)   ||
            (pthread_cond_destroy(&(pool->emptycv)) != 0) ||
            (pthread_cond_destroy(&(pool->wakecv)) != 0)) {
#if (DEBUG_THR_POOL > 0)
            dbugdf(1, "error: cond_destroy, errno", errno);
#endif // DEBUG_THR_POOL
        }
        // destroy locks
        if ((pthread_mutex_init(&(pool->lock), NULL) != 0) ||
            (pthread_mutex_destroy(&(pool->lock)) != 0)) {
            dbugdf(1, "error: mutex_destroy, errno", errno);
        }

        // if any of the above fail, it doesn't really matter
        // because they are pool resources in a bad state,
        // and we are about to destroy the pool
        
        // snip pool out of list
        poolp = pool;
        pool = pool->next;
        // free pool
        free(poolp);
    } // while

    // reset module-wide data to INITIALIZED conditions
    thr_pools = NULL;
    next_pool_handle = 0; 
    if (pthread_rwlock_init(&thr_pools_lock, NULL) != 0) {
        result = THR_POOL_LOCK_ERROR;
        dbugdf(1, "error: pthread_rwlock_init, errno", errno);
    }
    
    if (result != 0) {
        thrint_error_check(result);
    }
} // thrint_fork_child_handler



//! A cleanup handler, executed by each worker thread upon completing a job.
//!   Frees the resources associated with the job.
//!
//! @param[in]  job
//!   A pointer to our job.
//! @return None
//!
static void thrint_job_free_handler(job_t *job)
{
    if (job != NULL) {
        if (job->mode == THR_POOL_COPY_MODE) {
            free(job->arg);
        }
        free(job);
    }
} // thrint_job_free_handler



//! A cleanup handler, executed by each worker thread upon completing a job.
//!  Takes the calling thread out of the list of active threads.
//!
//! \post Leaves pool->lock acquired.
//!
//! @param[in]   pool
//!   A pointer to our pool.
//! @return  None.
//!
static void thrint_job_cleanup_handler(thr_pool_t *pool)
{
    int idx, result = 0;
    pthread_t my_tid = pthread_self();

    if (pool == NULL) {
        result = THR_POOL_BAD_INPUT;
        dbugf(1, "bad input");
    } else {
        if (pthread_mutex_lock(&(pool->lock)) != 0) {
            result = THR_POOL_PTHREAD_POOL_ERROR;
            dbugf(1, "error: pthread_mutex_lock");
        }

        idx = 0;    
        // remove us from active list
        while (!result && (idx < pool->max)) {
            if (pthread_equal(pool->tid[idx], my_tid)) {
                result = THR_POOL_SUCCESS;
                if (pool->active[idx] != 0) {
                    pool->active[idx] = 0;
                    pool->nidle++;
                }
            } else {
                idx++;
            }
        } // while
    }

    if (result != 0) {
        thrint_error_check(result);
    }
} // thrint_job_cleanup_handler



//! A cleanup handler, executed by each worker thread upon exiting.
//!   Takes the calling thread out of the thread pool. If the caller
//!   exited prematurely, it creates a new worker to replace it. If
//!   the caller exited as a response to the pool being destroyed,
//!   it signals anyone waiting (ie, anyone who called
//!   thr_pool_destroy()) in the case the pool is now empty.
//!
//! \pre Caller must have acquired pool->lock.
//!
//! @param[in]  pool
//!   A pointer to our pool.
//! @return None
//!
static void thrint_worker_cleanup_handler(thr_pool_t *pool)
{
    int result = 0;
    int sys_result, found, idx;
    pthread_t my_tid = pthread_self();

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "started");
    //thrint_module_state();
#endif // DEBUG_THR_POOL

    if ((pool == NULL) || (pool->active == NULL)) {
        result = THR_POOL_BAD_INPUT;
        dbugf(1, "bad input");
    } else {

        // find us in pool
        found = 0;
        idx = 0;
        while (!found && (idx < pool->max)) {
            if (pthread_equal(pool->tid[idx], my_tid)) {
                found = 1;
            } else {
                idx++;
            }
        }

        // remove us from pool
        if (found) {
            pool->tid[idx] = -1;
#if (DEBUG_THR_POOL > 0)
            pool->pid[idx] = -1;
#endif // DEBUG_THR_POOL
            if (pool->active[idx] != 0) {
                pool->active[idx] = 0;
            } else {
                pool->nidle--;
            }
            pool->nthreads--;
        }

        // if pool being destroyed and we are the last thread:
        if ( (pool->notice & THR_POOL_DESTROY_REQUEST) &&
             (pool->nthreads == 0) ) {
            // signal that the pool is empty
#if (DEBUG_THR_POOL > 1)
            dbugf(1, "broadcast pool empty");
#endif // DEBUG_THR_POOL
            sys_result = pthread_cond_broadcast(&(pool->emptycv));
            if (sys_result != 0) {
                result = THR_POOL_PTHREAD_POOL_ERROR;
                dbugf(1, "error: pthread_cond_broadcast");
            }
        // or:
        } else if (!(pool->notice & THR_POOL_DESTROY_REQUEST) &&
                    (pool->job_head != NULL) &&
                    (pool->nthreads < pool->max) ) {
            // we are not destroying the pool, 
            // there are jobs , and the pool is not full -- 
            // (we probably hit an unexpected pthread_exit() in the job 
            // function, therefore: make a new worker to replace us, to 
            // keep the pool active
#if (DEBUG_THR_POOL > 1)
            dbugf(1, "thread exited early");
#endif // DEBUG_THR_POOL
            sys_result = thrint_create_worker(pool);
            if (sys_result != THR_POOL_SUCCESS) {
                dbugdf(1, "error creating replacement worker"
                          "thrint_create_worker returns", sys_result);
                result = sys_result;
            }
        }

#if (DEBUG_THR_POOL > 3)
        dbugf(1, "state:");
        thrint_module_state();
#endif // DEBUG_THR_POOL

        if (pthread_mutex_unlock(&(pool->lock)) != 0) {
            result = THR_POOL_LOCK_POOL_ERROR;
            dbugf(1, "error: pthread_mutex_unlock");
        }
    }
    
    if (result != 0) {
        thrint_error_check(result);
    }
#if (DEBUG_THR_POOL > 0)
    dbugf(1, "ended");
#endif // DEBUG_THR_POOL
} // thrint_worker_cleanup_handler



//! The main loop for a worker thread.
//!
//! @param[in] arg
//!   A pointer to our pool.
//! @return None.
//!
static void *thrint_worker_function(void *arg)
{
    int sys_result, result = 0;
    int found, timed_out, time_to_exit, idx;
    job_t *job;
    void *(*func)(void *);
    struct timespec ts;
    thr_pool_t *pool = (thr_pool_t *) arg;
    pthread_t my_tid = pthread_self();

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "started");
#endif //DEBUG_THR_POOL

    if (pool == NULL) {
        result = THR_POOL_BAD_INPUT;
        dbugf(1, "bad input");
    }

    // lock
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
        result = THR_POOL_LOCK_POOL_ERROR;
        dbugf(1, "error: pthread_mutex_lock");
    }

    pthread_cleanup_push((push_fn_t) thrint_worker_cleanup_handler, pool);

    // add ourselves to list of threads
    if (result == 0) {
        found = 0;
        for(idx = 0; !found && (idx < pool->max); idx++) {
            if (pthread_equal(pool->tid[idx], -1)) {
                pool->tid[idx] = my_tid;
                pool->active[idx] = 0;
                pool->nidle++;
                pool->nthreads++;
                found = 1;
#if (DEBUG_THR_POOL > 0)
                pool->pid[idx] = getpid();
#endif //DEBUG_THR_POOL
            }
        }
        if (!found) {
            // we are excess and were created for no reason
            result = THR_POOL_SUCCESS;
        }
    }

    // the worker's thread's main loop
    time_to_exit = 0;
    while(!time_to_exit && (result == 0)) {       
        // set worker to default state
        timed_out = 0;
        if ((pthread_sigmask(SIG_SETMASK, &fillset, NULL) != 0)         ||
            (pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL) != 0) ||
            (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) != 0)) {
            result = THR_POOL_PTHREAD_POOL_ERROR;
            dbugf(1, "error: pthread_sigmask/setcanceltype/setcancelstate()");
        }
        
        // wait
        // (if no work, no errors, no timeout, and not being killed)
        while ((result == 0) &&
                !timed_out && 
                !(pool->notice & THR_POOL_DESTROY_REQUEST) &&
                (pool->job_head == NULL)) {
            // wait option 1: normal wait
            if (pool->nthreads <= pool->min) {
#if (DEBUG_THR_POOL > 2)
                dbugf(1, "before cond_wait");
#endif //DEBUG_THR_POOL
                sys_result = pthread_cond_wait(&(pool->jobcv), 
                                               &(pool->lock));
                if (sys_result != 0) {
                    result = THR_POOL_LOCK_ERROR;
                    dbugf(1, "error: pthread_cond_wait()");
                }
            // wait option 2: we are surplus
            } else {
#if (DEBUG_THR_POOL > 2)
                dbugf(1, "before cond_timedwait");
#endif //DEBUG_THR_POOL
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += pool->timeout;
                sys_result = pthread_cond_timedwait(&(pool->jobcv), 
                                                    &(pool->lock), &ts);
                if (sys_result == ETIMEDOUT) {
                    timed_out = 1;
#if (DEBUG_THR_POOL > 2)
                    dbugf(1, "cond_timedwait timeout");
#endif //DEBUG_THR_POOL
                } else if (sys_result != 0) {
                    result = THR_POOL_LOCK_ERROR;
                    dbugdf(1, "error: pthread_cond_timedwait()", sys_result);
                }
            }

            // if we became active because someone signalled us, 
            // acknowledge to them that we we woke up
            if (pool->notice & THR_POOL_WAKE_WORKER) {
                pool->notice |= THR_POOL_WORKER_WOKE;
                if (pthread_cond_signal(&(pool->wakecv)) != 0) {
                    result = THR_POOL_LOCK_ERROR;
                    dbugf(1, "error: pthread_cond_signal");
                }
            }

#if (DEBUG_THR_POOL > 1)
            dbugf(1, "got past wait / idle");
#endif //DEBUG_THR_POOL
        } // while

        // We will get here in three cases:
        if (result == 0) {
        
        // (1) the pool is being destroyed: so exit
            if (pool->notice & THR_POOL_DESTROY_REQUEST) {
                time_to_exit = 1;
#if (DEBUG_THR_POOL > 1)
                dbugf(1, "being destroyed, so exit");
#endif // DEBUG_THR_POOL
        // (2) we hit a timeout, no work in queue, we are surplus: so exit
            } else if (timed_out && 
                       (pool->nthreads > pool->min) &&
                       (pool->job_head == NULL)) {
                time_to_exit = 1;
#if (DEBUG_THR_POOL > 1)
                dbugf(1, "surplus & timeout, so exit");
#endif // DEBUG_THR_POOL
        // (3) there is work to do: so do it
            } else if (pool->job_head != NULL) {
#if (DEBUG_THR_POOL > 1)
                dbugf(1, "doing work");
#endif // DEBUG_THR_POOL
                // remove a job from the queue
                job = pool->job_head;
                pool->job_head = job->next;
                if (job == pool->job_tail) {
                    pool->job_tail = NULL;
                }
                
                // no one can find the job now: so, remember to free it
                pthread_cleanup_push((push_fn_t) thrint_job_free_handler, job);

                // add ourselves as active
                found = 0;
                idx = 0;
                while (!found && (idx < pool->max)) {
                    if (pthread_equal(pool->tid[idx], my_tid)) {
                        found = 1;
                    } else {
                        idx++;
                    }
                }
                if (!found) {
                    result = THR_POOL_ERROR;
                    dbugf(1, "unexpected error: thread not in list");
                } else {
                    pool->active[idx] = 1;
                    pool->nidle--;
                }

                // unlock
                if (pthread_mutex_unlock(&(pool->lock)) != 0) {
                    result = THR_POOL_LOCK_ERROR;
                    dbugf(1, "error: pthread_mutex_unlock(&(pool->lock))");
                }
                pthread_cleanup_push((push_fn_t) thrint_job_cleanup_handler, pool);

                // call the job
                if (result == 0) {
#if (DEBUG_THR_POOL > 4)
                    dbugf(1, "calling job function");
                    thrint_module_state();
#endif // DEBUG_THR_POOL
                    func = job->func;
                    arg = job->arg;
                    (void) func(arg);
#if (DEBUG_THR_POOL > 4)
                    dbugf(1, "done with job function");
                    thrint_module_state();
#endif // DEBUG_THR_POOL
                }
                // we came back normally: good.

                // lock
                pthread_cleanup_pop(0);
                thrint_job_cleanup_handler(pool);

                // free(job)
                pthread_cleanup_pop(0);
                thrint_job_free_handler(job);

            } // (3)
        }
    } //while(!time_to_exit)

    // unlock
    pthread_cleanup_pop(0);
    thrint_worker_cleanup_handler(pool);
    if (result != 0) {
        thrint_error_check(result);
    }

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "ended");
#endif //DEBUG_THR_POOL

    return NULL;
} // thrint_worker_function



//! Creates a worker thread.
//! \pre Caller must acquire lock to pool.
//!
//! @param[in] pool
//!   A pointer to our pool
//! @return An indicator of whether or not the operation was successful.
//!   On success: THR_POOL_SUCCESS is returned.
//!   On error: THR_POOL_BAD_INPUT, or 
//!             THR_POOL_PTHREAD_POOL_ERROR is returned.
//!
static int thrint_create_worker(thr_pool_t *pool)
{
    int result = 0;
    int sys_result;
    sigset_t oset;
    pthread_t thr;

    if (pool == NULL) {
        result = THR_POOL_BAD_INPUT;
        dbugf(1, "error: bad input");
    }

    if (result == 0) {
        sys_result = pthread_sigmask(SIG_SETMASK, &fillset, &oset);
        if (sys_result != 0) {
            result = THR_POOL_PTHREAD_POOL_ERROR;
            dbugdf(1, "error: pthread_sigmask() ", sys_result);
        }
    }

    if (result == 0) {
        sys_result = pthread_create(&thr, &(pool->attr), 
                                    &thrint_worker_function, pool);
        if (sys_result == EAGAIN) {
            result = THR_POOL_MEM_POOL_ERROR;
            dbugdf(1, "error: pthread_create resource error ", sys_result);
        } else if (sys_result != 0) {
            result = THR_POOL_PTHREAD_POOL_ERROR;
            dbugdf(1, "error: pthread_create() ", sys_result);
        }
    }

    if (result == 0) {
        // put back caller's mask
        // ie, we don't want to mess up mask of caller from thr_pool_queue()
        sys_result = pthread_sigmask(SIG_SETMASK, &oset, NULL);
        if (sys_result != 0) {
            result = THR_POOL_PTHREAD_POOL_ERROR;
            dbugdf(1, "error: pthread_sigmask() ", sys_result);
        }
    }

    if (result == 0) {
        result = THR_POOL_SUCCESS;
    }

    return result;
} // thrint_create_worker



//! Create and initialize the thread pool.
//!
//! @param[in]  pool_min
//!  The minimum number of threads in the pool. If no jobs
//!  are present for the pool, these threads are present, but idle.
//! @param[in]  pool_max
//!  The maximum number of active threads allowed in the queue;
//!  it should be the case that pool_max should be non-zero, no
//!  less than pool_min, and no greater than THR_POOL_MAX_THREADS.
//! @param[in]  timeout
//!  A value, interpreted as seconds, at most THR_POOL_MAX_TIMEOUT;
//!  if the number of active threads in the thread pool is in excess 
//!  of pool_min, any thread that becomes idle will wait for timeout 
//!  seconds. If no tasks are present in the queue or entered in the 
//!  queue during this time, the thread will be removed from the pool 
//!  and the system resources for the thread will be reclaimed;
//! @param[in]  attr
//!  Pointer to the POSIX thread attribute used to specify the attributes
//!  of any thread created in the thread pool. If NULL, the default 
//!  attributes of pthread_create() are used.
//! @param[out] pool_handle
//!  A handle for the thread pool.
//! @return
//!   An indicator of whether or not the operation was successful.
//!   On success: THR_POOL_SUCCESS is returned.
//!   On error: THR_POOL_BAD_INPUT, THR_POOL_RESOURCE_ERROR,
//!             THR_POOL_NOT_INITIALIZED, THR_POOL_ERROR, or 
//!             THR_POOL_UNRECOV_ERROR is returned.
//!
int thr_pool_create(const unsigned int pool_min, const unsigned int pool_max,
                    const unsigned int timeout, const pthread_attr_t *attr,
                    thr_pool_handle_t *pool_handle)
{
    int result = 0;
    int sys_result, value, idx, locked;
    thr_pool_handle_t handle;
    thr_pool_t *pool = NULL;
    pthread_attr_t *new_attr;
    struct sched_param param;
    size_t size;

    // check inputs
    if ((pool_handle == NULL)   ||
        (pool_min > pool_max)   || 
        (pool_max < 1)          ||
        (pool_max > THR_POOL_MAX_THREADS)) {
        result = THR_POOL_BAD_INPUT;
        dbugf(1, "error: bad input");
    }

    // lock
    locked = 0;
    if (result == 0) {
        if (pthread_rwlock_wrlock(&thr_pools_lock) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_rwlock_wrlock(&thr_pools_lock)");
        } else {
            locked = 1;
        }
    }

    // check module state, and reserve handle
    if (result == 0) {
        if (thr_pools_state != THR_POOL_INITIALIZED) {
            result = THR_POOL_NOT_INITIALIZED;
            dbugf(1, "error: module not in initialized state");
        } else if (next_pool_handle > THR_POOL_MAX_POOLS) {
            result = THR_POOL_RESOURCE_ERROR;
            dbugf(1, "error: module pool limit reached");
        } else {
            handle = next_pool_handle;
            next_pool_handle++;
        }
    }

    // unlock
    if (locked) {
        if (pthread_rwlock_unlock(&thr_pools_lock) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_rwlock_unlock(&thr_pools_lock)");
        }
    }

    // make pool
    if (result == 0) {
        pool = malloc(sizeof(thr_pool_t));
        if (pool == NULL) {
            result = THR_POOL_MEM_POOL_ERROR;
            dbugdf(1, "error: malloc() errno", errno);
        }
    }

    // initialize pool locks and cv
    if (result == 0) {
        if ((pthread_mutex_init(&(pool->lock), NULL) != 0)   ||
            (pthread_cond_init(&(pool->emptycv), NULL) != 0) ||
            (pthread_cond_init(&(pool->jobcv), NULL) != 0)   ||
            (pthread_cond_init(&(pool->wakecv), NULL) != 0)) {
            result = THR_POOL_PTHREAD_POOL_ERROR;
            dbugdf(1, "error: pthread_mutex/cond_init(), errno", errno);
        }
    }

    // initialize pool data
    if (result == 0) {
        pool->job_head = NULL;
        pool->job_tail = NULL;
        pool->notice = 0;
        pool->timeout = timeout;
        pool->min = pool_min;
        pool->max = pool_max;
        pool->nthreads = 0;
        pool->nidle = 0;
        pool->handle = handle;
        pool->active = malloc(pool_max*sizeof(int));
        pool->tid = malloc(pool_max*sizeof(pthread_t));
#if (DEBUG_THR_POOL > 0)
        pool->pid = malloc(pool_max*sizeof(pid_t));
#endif //DEBUG_THR_POOL

        if ((pool->active == NULL) || (pool->tid == NULL)) {
            result = THR_POOL_MEM_POOL_ERROR;
            dbugdf(1, "error: malloc(), errno", errno);
        } else {
            for(idx=0; idx < pool->max; idx++) {
                pool->tid[idx] = -1;
                pool->active[idx] = 0;
            }
        }
    }

    // copy over all the thread attributes
    if (result == 0) {
        new_attr = &(pool->attr);
        if (pthread_attr_init(new_attr) != 0) {
            result = THR_POOL_MEM_POOL_ERROR;
            dbugdf(1, "error: pthread_attr_init(), errno", errno);
        }
        if (attr != NULL) {
            // these are the appropriate functions to access attr variables
            if ((pthread_attr_getstacksize(attr, &size) != 0)       ||
                (pthread_attr_setstacksize(new_attr, size) != 0)    ||
                (pthread_attr_getscope(attr, &value) != 0)          ||
                (pthread_attr_setscope(new_attr, value) != 0)       ||
                (pthread_attr_getinheritsched(attr, &value) != 0)   ||
                (pthread_attr_setinheritsched(new_attr, value) != 0) ||
                (pthread_attr_getschedpolicy(attr, &value) != 0)    ||
                (pthread_attr_setschedpolicy(new_attr, value) != 0) ||
                (pthread_attr_getschedparam(attr, &param) != 0)     ||
                (pthread_attr_setschedparam(new_attr, &param) != 0) ||
                (pthread_attr_getguardsize(attr, &size) != 0)       ||
                (pthread_attr_setguardsize(new_attr, size) != 0)) {
                result = THR_POOL_PTHREAD_POOL_ERROR;
                dbugdf(1, "error: pthread_attr_*(), errno", errno);
            }
        }
        if (pthread_attr_setdetachstate(new_attr,PTHREAD_CREATE_DETACHED)!=0) {
            result = THR_POOL_PTHREAD_POOL_ERROR;
            dbugdf(1, "error: pthread_attr_setdetachstate(), errno", errno);
        }
    }

    // lock
    locked = 0;
    if (result == 0) {
        if (pthread_mutex_lock(&(pool->lock)) != 0) {
            result = THR_POOL_LOCK_POOL_ERROR;
            dbugf(1, "error: pthread_mutex_lock(&(pool->lock))");
        } else {
            locked = 1;
        }
    }

    // populate pool
    for(idx = 0; (result == 0) && (idx < pool->min); idx++) {
        sys_result = thrint_create_worker(pool);
        if (sys_result != THR_POOL_SUCCESS) {
            result = sys_result;
            dbugdf(1, "error: thrint_create_worker returns", sys_result);
        }
    }

    // unlock
    if (locked) {
        if (pthread_mutex_unlock(&(pool->lock)) != 0) {
            result = THR_POOL_LOCK_POOL_ERROR;
            dbugf(1, "error: pthread_mutex_unlock(&(pool->lock))");
        }
    }

    if (result == 0) {
        if (pthread_rwlock_wrlock(&thr_pools_lock) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_rwlock_wrlock(&thr_pools_lock)");
        } else {
        // insert the pool into our pools list
            pool->next = thr_pools;
            thr_pools = pool;
        // unlock
            if (pthread_rwlock_unlock(&thr_pools_lock) != 0) {
                result = THR_POOL_LOCK_ERROR;
                dbugf(1, "error: pthread_rwlock_unlock(&thr_pools_lock)");
            }
        }
    }

    if (result == 0) {
        *pool_handle = handle;
        result = THR_POOL_SUCCESS;
    } else {
        result = thrint_error_check(result);
    }
    
    if (result != THR_POOL_SUCCESS) {
        if (pool_handle != NULL) {
            *pool_handle = -1;
        }
    }
    return result;
} // thr_pool_create



//! If there are idle threads in the pool, awaken one to perform the job.
//!   If there are no idle threads but the maximum number of threads in the 
//!   pool has not been reached, create a new thread to perform the job.
//!   If no idle threads exist and the pool already has the maximum number of 
//!   threads, then add the job to the queue and return success; some existing
//!   worker thread will perform the job when it becomes idle.
//!   If mode is 1, the len bytes at arg will be copied locally, and the job
//!   (when executed) will use the local copy of arg.
//!
//! @param[in] pool_handle
//!   The handle for the thread pool.
//! @param[in] mode
//!   Can take any value. If 1, it indicates arg should be (shallow) copied. 
//!   This is useful in many scenarios, e.g. when arg is a pointer
//!   to something on the caller's stack. If arguments are not copied,
//!   the caller must ensure arg persists until a thread is scheduled for the
//!   job, and throughout the duration of that job.
//! @param[in] len
//!   The length of arg.
//! @param[in] func
//!   A pointer to the job function.
//! @param[in,out] arg
//!   A pointer to the arguments for the job function. If argument is
//!   copied when enqueued, it cannot be used as an output argument.
//! @return 
//!   An indicator of whether or not the operation was successful
//!   On success: THR_POOL_SUCCESS is returned.
//!   On error: THR_POOL_BAD_INPUT, THR_POOL_RESOURCE_ERROR,
//!             THR_POOL_INVALID_HANDLE, THR_POOL_NOT_INITIALIZED,
//!             THR_POOL_ERROR, or THR_POOL_UNRECOV_ERROR is returned.
//!
int thr_pool_queue(const thr_pool_handle_t pool_handle, 
                   const int mode, const size_t len, 
                   void *(*func)(void *), void *arg)
{
    int result = 0;
    int sys_result;
    int found, locked, wake_worker;
    job_t *job = NULL;
    thr_pool_t *pool = NULL;

#if (DEBUG_THR_POOL > 0)
    dbugdf(1, "pool = ", pool_handle);
#endif

    // sanity check some inputs
    if ((mode == THR_POOL_COPY_MODE) && (arg == NULL) && (len > 0)) {
        result = THR_POOL_BAD_INPUT;
        dbugf(1, "error: bad input");
    }

    // initialize the job
    if (result == 0) {
        job = malloc(sizeof(job_t)); 
        if (job == NULL) {
            result = THR_POOL_RESOURCE_ERROR;
            dbugdf(1, "error: malloc(sizeof(job_t)), errno", errno);
        }
        // gets freed by worker thread in thrint_worker_function()
        // or, just to be sure, in thr_pool_destroy()
    }
    
    // initialize the job data
    if (result == 0) {
        job->next = NULL;
        job->func = func;
        job->mode = mode;
        
        if ((mode == THR_POOL_COPY_MODE) && (arg != NULL)) {
            // copy over arguments
            job->arg = malloc(len);
            if (job->arg == NULL) {
                result = THR_POOL_RESOURCE_ERROR;
                dbugdf(1, "error: malloc(len), errno", errno);
            } else {
                memcpy(job->arg, arg, len);
            }
        } else {
            job->arg = arg;
        }
    }

    // lock
    locked = 0;
    if (result == 0) {
        if (pthread_rwlock_rdlock(&thr_pools_lock) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_rwlock_rdlock(&thr_pools_lock)");
        } else {
            locked = 1;
        }
    }
   
    // check module state, and find handle
    if (result == 0) {
        if (thr_pools_state != THR_POOL_INITIALIZED) {
            result = THR_POOL_NOT_INITIALIZED;
            dbugf(1, "module not in initialized state");
        } else if (thr_pools_state == THR_POOL_INITIALIZED) {
            pool = thr_pools;
            found = 0;
            while (!found && (pool != NULL)) {
                if (pool->handle == pool_handle) {
                    found = 1;
                } else {
                    pool = pool->next;
                }
            }
            if (!found) {
                result = THR_POOL_INVALID_HANDLE;
                dbugf(1, "error: pool handle not found");
            }
        }
    }
    
    // unlock
    if (locked) {
        if (pthread_rwlock_unlock(&thr_pools_lock) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_rwlock_unlock(&thr_pools_lock)");
        }
    }

    // lock
    locked = 0;
    if (result == 0) {
        if (pthread_mutex_lock(&(pool->lock)) != 0) {
            result = THR_POOL_LOCK_POOL_ERROR;
            dbugf(1, "error: pthread_mutex_lock(&(pool->lock))");
        } else {
            locked = 1;
        }
    }
#if (DEBUG_THR_POOL > 2)
    dbugf(1, "got mutex");
#endif // DEBUG_THR_POOL
    // if there was an error up until this point, back out:
    if ((result != 0) && (job != NULL)) {
        dbugf(1, "hit error before, so free(job)");
        if ((job->arg != NULL) && (mode == THR_POOL_COPY_MODE)) {
            free(job->arg);
        }
        free(job);
    }

    // put job in queue
    if (result == 0) {
        // put job at end of queue
        if (pool->job_head == NULL) {
            pool->job_head = job;
        } else {
            (pool->job_tail)->next = job;
        }
        pool->job_tail = job;
    }
    
    // try to wake up an idle worker
    wake_worker = 0;
    if ((result == 0) && (pool->nidle > 0)) {
#if (DEBUG_THR_POOL > 1)
        dbugf(1, "waking worker");
#endif // DEBUG_THR_POOL
        //   this is complicated due to a race condition causing a possible
        //   "lost signal" error: consider two thr_pool_queue calls, 
        //   both signalling on jobcv before the worker wakes;
        //   so we coordinate through wakecv and notice to avoid this,
        //   though it serializes some of the queing logic.
        
        // someone is already waking a worker
        while(pool->notice & THR_POOL_WAKE_WORKER) {
#if (DEBUG_THR_POOL > 2)
            dbugf(1, "notice exists; signal and wait");
#endif // DEBUG_THR_POOL
            // they were waiting for this signal, so repeat it and wait
            if (pthread_cond_signal(&(pool->wakecv)) != 0) {
                result = THR_POOL_PTHREAD_POOL_ERROR;
                dbugdf(1, " error: pthread_cond_signal, errno", errno);
            } else if (pthread_cond_wait(&(pool->wakecv), &(pool->lock))!=0) {
                result = THR_POOL_PTHREAD_POOL_ERROR;
                dbugdf(1, "error: pthread_cond_wait, errno", errno);
            }
        }
        // our turn to wake a worker
        if (pool->nidle > 0) {
            pool->notice |= THR_POOL_WAKE_WORKER;
            if (pthread_cond_signal(&(pool->jobcv)) != 0) {
                result = THR_POOL_PTHREAD_POOL_ERROR;
                dbugdf(1, "error: pthread_cond_signal, errno", errno);
            }
            while (!(pool->notice & THR_POOL_WORKER_WOKE)) {
                if (pthread_cond_wait(&(pool->wakecv), &(pool->lock)) != 0) {
                    result = THR_POOL_PTHREAD_POOL_ERROR;
                    dbugdf(1, "error: pthread_cond_wait, errno", errno);
                }
            }

            // clear notices
            wake_worker = 1;
            pool->notice &= ~THR_POOL_WAKE_WORKER;
            pool->notice &= ~THR_POOL_WORKER_WOKE;

#if (DEBUG_THR_POOL > 2)
            dbugf(1, "got ack, cleared notices");
#endif // DEBUG_THR_POOL

            // send wake signal to other threads waiting to queue, if they exist
            if (pthread_cond_signal(&(pool->wakecv)) != 0) {
                result = THR_POOL_PTHREAD_POOL_ERROR;
                dbugdf(1, "error: pthread_cond_signal, errno", errno);
            }
        }
    }

    // or, try to make a new worker
    if ((result == 0) && !wake_worker && (pool->nthreads < pool->max)) {
#if (DEBUG_THR_POOL > 1)
        dbugf(1, "creating worker");
#endif // DEBUG_THR_POOL
        sys_result = thrint_create_worker(pool);
        if (sys_result != THR_POOL_SUCCESS) {
            result = sys_result;
        }
    }   

    // unlock
    if (locked) {
#if (DEBUG_THR_POOL > 2)
        dbugf(1, "releasing mutex");
#endif // DEBUG_THR_POOL
        if (pthread_mutex_unlock(&(pool->lock)) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_mutex_unlock(&(pool->lock))");
        }
    }
    if (result == 0) {
        result = THR_POOL_SUCCESS;
    } else {
        result = thrint_error_check(result);
    }
    return result;
} // thr_pool_queue



//! Cancels all queued jobs, kills all active threads, and frees 
//!   all internal module resources related to the thread.
//!
//! @param[in] pool
//!   The handle for the thread pool.
//! @return
//!   An indicator of whether or not the operation was successful
//!   On success: THR_POOL_SUCCESS is returned.
//!   On error: THR_POOL_LOCK_POOL_ERROR or 
//!             THR_POOL_PTHREAD_POOL_ERROR is returned.
//!
static int thrint_pool_destroy(thr_pool_t *pool)
{
    int result = 0;
    int found, idx, locked;
    int error = 0;
    int skip = 0;
    thr_pool_t *prev_pool;
    job_t *job;
    pthread_t my_tid;

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "started");
#endif // DEBUG_THR_POOL
    
    if (pool == NULL) {
        result = THR_POOL_SUCCESS;
        skip = 1;
    }

    // lock
    if (!skip) {
        if (pthread_mutex_lock(&(pool->lock)) != 0) {
            result = THR_POOL_LOCK_POOL_ERROR;
            dbugf(1, "error: pthread_mutex_lock");
        } else {
            // add unlock to cleanup stack (wait is a cancellation point)
            pthread_cleanup_push((push_fn_t) pthread_mutex_unlock,
                                 &(pool->lock));

            // Are we a thread in the pool that is trying to destroy it? 
            // Sounds weird to me, but I was told it might happen
            found = 0;
            idx = 0;
            my_tid = pthread_self();
            while (!found && (idx < pool->max)) {
                if (pthread_equal(pool->tid[idx], my_tid)) {
                    found = 1;
                } else {
                    idx++;
                }
            }
            if (found) {
                // remove us from the pool
                if (!(pool->active[idx])) {
                    pool->nidle--;
                }
                pool->active[idx] = 0;
                pool->tid[idx] = -1;
                pool->nthreads--;
            }

            // mark the pool as being destroyed; wakeup idle workers
            pool->notice |= THR_POOL_DESTROY_REQUEST;
            
            // loop through active workers, cancelling each
            for(idx = 0; !error && (idx < pool->max); idx++) {
                if (pool->active[idx]) {
#if (DEBUG_THR_POOL > 2)
                    dbugdf(1, "cancelling tid ", pool->tid[idx]);
#endif //DEBUG_THR_POOL
                    if (pthread_cancel(pool->tid[idx]) != 0) {
                         result = THR_POOL_PTHREAD_POOL_ERROR;
                         error = 1;
                         dbugdf(1, "error: pthread_cancel, errno", errno);
                    }
                }
            }
#if (DEBUG_THR_POOL > 2)
            dbugf(1, "broadcast to idle workers");
#endif //DEBUG_THR_POOL
            if (pthread_cond_broadcast(&(pool->jobcv)) != 0) {
                result = THR_POOL_PTHREAD_POOL_ERROR;
                error = 1;
                dbugdf(1, "error: pthread_cond_broadcast, errno", errno);
            }
            // there are still worker threads; the last one will wake us
            while (!error && (pool->nthreads != 0)) {
#if (DEBUG_THR_POOL > 2)
                dbugf(1, "workers not gone, need to wait ");
#endif //DEBUG_THR_POOL
                if (pthread_cond_wait(&(pool->emptycv), &(pool->lock)) != 0) {
                    result = THR_POOL_LOCK_POOL_ERROR;
                    error = 1;
                    dbugdf(1, "error: pthread_cond_wait, errno", errno);
                }
            }
    // unlock
            pthread_cleanup_pop(0);
            pthread_mutex_unlock(&(pool->lock));
        }
    }

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "removing pool from list");
#endif //DEBUG_THR_POOL

    // lock
    locked = 0;
    if (!skip) {
        if (pthread_rwlock_wrlock(&thr_pools_lock) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_rwlock_wrlock");
        } else {
            locked = 1;
        }
    }
  
    // Remove the pool from the list of pools.
    if (!skip) {
        if (thr_pools == pool) {
            thr_pools = pool->next;
        } else if (thr_pools != NULL) {
            for(prev_pool = thr_pools; prev_pool != NULL; 
                                       prev_pool = prev_pool->next) {
                if (prev_pool->next == pool) {
                    prev_pool->next = pool->next;
                }
            }
        }
    }
   
    // unlock
    if (locked) {
        if (pthread_rwlock_unlock(&thr_pools_lock) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_rwlock_unlock");
        } else {
            locked = 0;
        }
    }
 
#if (DEBUG_THR_POOL > 0)
    dbugf(1, "free pool resources");
#endif //DEBUG_THR_POOL
 
    // Release resources associated with the pool
    // (pool is no longer a part of the pool list and no one is active in it)
    if (!skip) {
        // these should all be gone, but just in case
        while(pool->job_head != NULL) {
#if (DEBUG_THR_POOL > 0)
            dbugf(1, "jobs are not all gone? strange.");
#endif // DEBUG_THR_POOL
            job = pool->job_head;
            pool->job_head = job->next;
            thrint_job_free_handler(job);
        }
        
        if ((pthread_attr_destroy(&(pool->attr)) != 0)      ||
            (pthread_cond_destroy(&(pool->jobcv)) != 0)     ||
            (pthread_cond_destroy(&(pool->emptycv)) != 0)   ||
            (pthread_cond_destroy(&(pool->wakecv)) != 0)    ||
            (pthread_mutex_unlock(&(pool->lock)) != 0)) {
            result = THR_POOL_PTHREAD_POOL_ERROR;
            dbugdf(1, "error: pthread_*_destroy, errno", idx);
        }
        pthread_mutex_destroy(&(pool->lock));
        free(pool);
    }
    
    if (result == 0) {
        result = THR_POOL_SUCCESS;
    }
    
#if (DEBUG_THR_POOL > 0)
    dbugf(1, "end");
#endif //DEBUG_THR_POOL
    return result;
} // thrint_pool_destroy



//! Cancels all queued jobs, kills all active threads, and frees 
//!   all internal module resources. All thread pools being managed
//!   are destroyed.
//!
//! @return
//!   An indicator of whether or not the operation was successful
//!   On success: THR_POOL_SUCCESS is returned.
//!   On error: THR_POOL_NOT_INITIALIZED, or 
//!             THR_POOL_UNRECOV_ERROR is returned.
//!
int thr_pool_free(void)
{
    int result = 0;
    int locked = 0;
    int skip = 0;
    int sys_result;

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "thr_pool_free started");
#endif // DEBUG_THR_POOL

    // check module state
    if ((result = pthread_rwlock_rdlock(&thr_pools_lock)) != 0) {
        dbugf(1, strerror(result));
        result = THR_POOL_LOCK_ERROR;
        dbugf(1, "error: pthread_rwlock_rdlock");
    } else {
        if (thr_pools_state == THR_POOL_NOT_INITIALIZED) {
            result = THR_POOL_NOT_INITIALIZED;
            skip = 1;
            dbugf(1, "module not init, no need to free");
        }
        if ((pthread_rwlock_unlock(&thr_pools_lock) != 0)) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_rwlock_unlock");
        }
    }

    // destroy each pool, one by one
    while (!skip && (thr_pools != NULL) && (result == 0)) {
        sys_result = thrint_pool_destroy(thr_pools);
        if (sys_result != THR_POOL_SUCCESS) {
            result = THR_POOL_ERROR;
            dbugdf(1, "error: failed to destroy all pools, "
                      "thrint_pool_destroy returns", sys_result);
        }
    }

    // lock
    locked = 0;
    if (pthread_rwlock_wrlock(&thr_pools_lock) != 0) {
        result = THR_POOL_LOCK_ERROR;
        dbugf(1, "error: pthread_rwlock_wrlock");
    } else {
        locked = 1;
    }

    // change module state
    if (result == 0) {
        thr_pools_state = THR_POOL_NOT_INITIALIZED;
        next_pool_handle = -1;
    } else {
        thr_pools_state = THR_POOL_UNRECOV_ERROR;
    }

#if (DEBUG_THR_POOL > 2)
    thrint_module_state();
#endif // DEBUG_THR_POOL

    // unlock
    if (locked) {
        if ((pthread_rwlock_unlock(&thr_pools_lock) != 0)) {
            result = THR_POOL_LOCK_ERROR;
            thr_pools_state = THR_POOL_UNRECOV_ERROR;
            dbugf(1, "error: pthread_rwlock_unlock");
        }
    }   

    if (result == 0) {
        result = THR_POOL_SUCCESS;
    } else if (result != THR_POOL_NOT_INITIALIZED) {
        result = THR_POOL_UNRECOV_ERROR;
    }

#if (DEBUG_THR_POOL > 0)
    dbugf(1, "ending");
#endif // DEBUG_THR_POOL

    return result;
} // thr_pool_free



//! Some modue errors require actions to be taken on their behalf, such as
//!  changing the module state to the URECOVERABLE_ERROR_STATE. This function 
//!  handles those conditions, and translates the internal errors into 
//!  sensible, external errors that may be returned to the caller.
//!
//! \pre Unlike other internal functions, this function acquires the 
//!      module lock; caller should not already hold a module lock.
//!
//! @param[in] error
//!   An error code
//! @return
//!   A constant that corresponds in meaning to error; if error is not a
//!   meaningful error, it is returned unchanged.
//!
static int thrint_error_check(int error)
{
    int result = 0;
    
    if ((error == THR_POOL_PTHREAD_ERROR)   ||
        (error == THR_POOL_LOCK_ERROR)) {
        result = THR_POOL_UNRECOV_ERROR;
        dbugf(1, "detected unrecoverable module error");
    } else if ((error == THR_POOL_PTHREAD_POOL_ERROR)   ||
               (error == THR_POOL_LOCK_POOL_ERROR)) {
        // these are pool errors ---  other pools are not affected
        result = THR_POOL_ERROR;
        dbugf(1, "detected pool error");
    } else if ((error == THR_POOL_MEM_POOL_ERROR) ||
               (error == THR_POOL_MEM_ERROR)) {
        // these should be interpreted as resource errors
        result = THR_POOL_RESOURCE_ERROR;
    } else {
        result = error;
    }
    
    if (result == THR_POOL_UNRECOV_ERROR) {
        // mark state with critical error
        // (no error checking: we've already hit a critical error)
        pthread_rwlock_wrlock(&thr_pools_lock);
        thr_pools_state = THR_POOL_UNRECOV_ERROR;
        pthread_rwlock_unlock(&thr_pools_lock);
    }
    
    return result;
} // thrint_error_check



//! Cancels all queued jobs, kills all active threads, and frees 
//!   all internal module resources related to the thread.
//!
//! @param[in] pool_handle
//!   The handle for the thread pool.
//! @return
//!   An indicator of whether or not the operation was successful
//!   On success: THR_POOL_SUCCESS is returned.
//!   On error: THR_POOL_INVALID_HANDLE, THR_POOL_NOT_INITIALIZED,
//!             THR_POOL_ERROR, or THR_POOL_UNRECOV_ERROR is returned.
//!
int thr_pool_destroy(const thr_pool_handle_t pool_handle)
{
    int result = 0;
    int found, locked = 0;
    thr_pool_t *pool;

    // lock
    if (pthread_rwlock_rdlock(&thr_pools_lock) != 0) {
        result = THR_POOL_ERROR;
        dbugf(1, "error: pthread_rwlock_rdlock");
    } else {
        locked = 1;
    }

    // check module state, and find handle  
    if (result == 0) {
        if (thr_pools_state != THR_POOL_INITIALIZED) {
            result = THR_POOL_NOT_INITIALIZED;
            dbugf(1, "module not in initialized state");
        } else {
            found = 0;
            pool = thr_pools;
            while (!found && (pool != NULL)) {
                if (pool->handle == pool_handle) {
                    found = 1;
                } else {
                    pool = pool->next;
                }
            }
            if (!found) {
                result = THR_POOL_INVALID_HANDLE;
                dbugf(1, "error: pool not found");
            }
        }
    }

    // unlock
    if (locked) {
        if (pthread_rwlock_unlock(&thr_pools_lock) != 0) {
            result = THR_POOL_ERROR;
            dbugf(1, "error: pthread_rwlock_unlock");
        } else {
            locked = 0;
        }
    }
    
    if (result == 0) {
        result = thrint_pool_destroy(pool);
    }
    result = thrint_error_check(result);
    
    return result;
} // thr_pool_destroy



//! Initialize the thread management module data.
//!
//! @return
//!   An indicator of whether or not the operation was successful
//!   On success: THR_POOL_SUCCESS is returned.
//!   On error: THR_POOL_RESOURCE_ERROR, THR_POOL_ERROR, or
//!             THR_POOL_UNRECOV_ERROR is returned.
//!
int thr_pool_init(void)
{
    int result = 0;
    int locked = 0;

    // lock
    if (pthread_rwlock_wrlock(&thr_pools_lock) != 0) {
        result = THR_POOL_LOCK_ERROR;
        dbugf(1, "error: pthread_rwlock_wrlock");
    } else {
        locked = 1;
    }

    if (thr_pools_state == THR_POOL_INITIALIZED) {
        result = THR_POOL_SUCCESS;
#if (DEBUG_THR_POOL > 0)
        dbugf(1, "already initialized");
#endif // DEBUG_THR_POOL
    }
    
    // initialize module data
    if (result == 0) {
        if (sigfillset(&fillset) != 0) {
            result = THR_POOL_ERROR;
            dbugdf(1, "error: sigfillset(), errno", errno);
        } else {
            thr_pools_state = THR_POOL_INITIALIZED;
            next_pool_handle = 0;
        }
    }

    // register atfork handler
    if ((result == 0) && !thr_pools_atfork) {
        if (pthread_atfork(&thrint_lock_all, 
                           &thrint_unlock_all, 
                           &thrint_fork_child_handler) != 0) {
            // ENOMEM is only error condition
            result = THR_POOL_RESOURCE_ERROR;
            dbugdf(1, "error: pthread_atfork(), errno", errno);
        } else {
            thr_pools_atfork = 1;
        }
    }

    // unlock
    if (locked) {
        if (pthread_rwlock_unlock(&thr_pools_lock) != 0) {
            result = THR_POOL_LOCK_ERROR;
            dbugf(1, "error: pthread_rwlock_unlock(&thr_pools_lock)");
        } else {
            locked = 0;
        }
    }

    if (result == 0) {
        result = THR_POOL_SUCCESS;
    } else {
        result = thrint_error_check(result);
    }

    return result;
} // thr_pool_init

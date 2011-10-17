#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/signal.h>
#include <limits.h>
#include <test-dept.h>
#include "test_dept_helper_functions.h"
#include "thr_pool.h"
#include "pthread_mock.h"
#include "stdlib_mock.h"


// Constants
#define default_mode   0x1
#define exit_early     0x2
#define fork_in_init   0x4
#define copy_mode      0x8

typedef void (*push_fn_t)(void *);


// Shared data
int *worker_fn_args = NULL;         // each job gets a separate arg, no need to protect
pthread_mutex_t worker_fn_mutex;
pthread_cond_t worker_fn_done_1;    // signal worker -> manager
int worker_fn_count_1 = 0;          // counter for workers completing Step 1
pthread_cond_t worker_fn_start_2;   // signal manager -> worker
pthread_cond_t worker_fn_done_2;    // signal worker -> manager
int worker_fn_count_2 = 0;          // counter for workers completing Step 2

// Module data (not used directly by threads)
int num_min_thr = 5;
int num_max_thr = 10;
int timeout = 10;
int num_jobs = 10;
int mode = default_mode;
int skip_teardown = 0;



// Function Prototypes
void *worker_fn(void *arg);
void *cancellation_handler(void *arg);
void check_job_free(void);
void check_job_init(void);
void prefork_handler(void);
void postfork_parent_handler(void);
void postfork_child_handler(void);


//////// support functions ////////////////////////////////////////////////////

void signal_handler(int signum)
{
    debug(1, "signal => %d", signum);
 }

void prefork_handler(void)
{
    debug(1, "(in prefork_handler)");
    pthread_mutex_lock(&worker_fn_mutex);
}

void postfork_parent_handler(void)
{
    debug(1, "(in postfork_parent_handler)");
    pthread_mutex_unlock(&worker_fn_mutex);
}

void postfork_child_handler(void)
{
    debug(1, "(in postfork_child_handler)");
    pthread_cond_init(&worker_fn_done_1, NULL);
    pthread_cond_init(&worker_fn_done_2, NULL);
    pthread_cond_init(&worker_fn_start_2, NULL);
    pthread_mutex_init(&worker_fn_mutex, NULL);
}

void setup(void)
{
    int result;
    struct sigaction act;
    sigset_t sig_set;

    sigfillset(&sig_set);
    act.sa_handler  = &signal_handler;
    act.sa_mask     = sig_set;
    act.sa_flags    = 0;
    act.sa_restorer = NULL;

    result = pthread_cond_init(&worker_fn_done_1, NULL);
    assert_equals(result, 0);
    result = pthread_cond_init(&worker_fn_done_2, NULL);
    assert_equals(result, 0);
    result = pthread_cond_init(&worker_fn_start_2, NULL);
    assert_equals(result, 0);
    result = pthread_mutex_init(&worker_fn_mutex, NULL);
    assert_equals(result, 0);

    result = thr_pool_init();
    assert_equals(result, THR_POOL_SUCCESS);
}

void teardown(void)
{
    int result;

    if (!skip_teardown) {
        result = thr_pool_free();
        assert_equals(result, THR_POOL_SUCCESS);
    } else {
        debug(1, "=> Teardown being skipped!");
    }
}


//////// related system tests /////////////////////////////////////////////////


void *very_sleepy_worker_fn(void *arg)
{
    while (worker_fn_count_1 == 0) {
        sleep(1);
    }
}

// tests to see if the system supports processes making THR_POOL_MAX threads
void test_pthread_max(void)
{
    // we use two less, since our test jig can be invoked in a way
    // that uses 2 threads, itself.
    const int test_max = THR_POOL_MAX_THREADS - 2;
    
    int idx, result;
    pthread_t thr;

    debug(1, "THR_POOL_MAX_THREADS = %d", THR_POOL_MAX_THREADS);
    for (idx = 0; idx < test_max; idx++) {
        result = pthread_create(&thr, NULL, &very_sleepy_worker_fn, NULL);
        if (result != 0) {
            worker_fn_count_1 = 1;
            debug(1, "pthread_create: thread %d of %d, error %d", 
                        idx+1, test_max, result);
        }
        assert_equals(result, 0);
    }
    worker_fn_count_1 = 1;
    sleep(1);
}


//////// test putting module into various states //////////////////////////////

// create after free fails
void test_state_1(void)
{
    int result;
    thr_pool_handle_t handle;
    
    result = thr_pool_free();
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_NOT_INITIALIZED);
    skip_teardown = 1;
}

// queue after free fails
void test_state_3(void)
{
    int result;
    thr_pool_handle_t handle;

    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_free();
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_queue(handle, 0, 0, &worker_fn, NULL);
    assert_equals(result, THR_POOL_NOT_INITIALIZED);
    skip_teardown = 1;
}

// destroy after free fails
void test_state_4(void)
{
    int result;
    thr_pool_handle_t handle;

    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_free();
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_destroy(handle);
    assert_equals(result, THR_POOL_NOT_INITIALIZED);
    skip_teardown = 1;
}

// double init works
void test_state_5(void)
{
    int result;
    result = thr_pool_init();
    assert_equals(result, THR_POOL_SUCCESS);
}


//////// test bad input for thr_pool_create ///////////////////////////////////

// null handle
void test_create_bad_input_1(void)
{
    int result;
    thr_pool_handle_t handle;

    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, NULL);
    assert_equals(result, THR_POOL_BAD_INPUT);
}

// bad max
void test_create_bad_input_2(void)
{
    int result;
    thr_pool_handle_t handle;

    result = thr_pool_create(num_min_thr, 0, 
                             timeout, NULL, &handle);
    assert_equals(result, THR_POOL_BAD_INPUT);
}

// bad max
void test_create_bad_input_3(void)
{
    int result;
    thr_pool_handle_t handle;

    result = thr_pool_create(num_min_thr, num_min_thr - 1,
                             timeout, NULL, &handle);
    assert_equals(result, THR_POOL_BAD_INPUT);
}

// bad max
void test_create_bad_input_4(void)
{
    int result;
    thr_pool_handle_t handle;

    result = thr_pool_create(num_min_thr, THR_POOL_MAX_THREADS+1, 
                             timeout, NULL, &handle);
    assert_equals(result, THR_POOL_BAD_INPUT);
}




//////// test bad input for thr_pool_queue ////////////////////////////////////

void test_queue_bad_handle_1(void)
{
    int result;
    thr_pool_handle_t handle;
    
    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_queue(-1, 0, 0, &worker_fn, NULL);
    assert_equals(result, THR_POOL_INVALID_HANDLE);
}


void test_queue_bad_handle_2(void)
{
    int result;
    thr_pool_handle_t handle;

    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_destroy(handle);
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_queue(handle, 0, 0, &worker_fn, NULL);
    assert_equals(result, THR_POOL_INVALID_HANDLE);
}




//////// test bad input for thr_pool_destroy //////////////////////////////////

void test_destroy_bad_handle_1(void)
{
    int result;
    thr_pool_handle_t handle;

    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_destroy(handle);
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_destroy(handle);
    assert_equals(result, THR_POOL_INVALID_HANDLE);
}



//////// test doing work //////////////////////////////////////////////////////

void check_job_init(void)
{
    int idx, result, pid, status;

    result = pthread_cond_init(&worker_fn_done_1, NULL);
    assert_equals(result, 0);
    result = pthread_cond_init(&worker_fn_done_2, NULL);
    assert_equals(result, 0);
    result = pthread_cond_init(&worker_fn_start_2, NULL);
    assert_equals(result, 0);
    result = pthread_mutex_init(&worker_fn_mutex, NULL);
    assert_equals(result, 0);
    
    worker_fn_args = NULL;  
    worker_fn_args = (int *)malloc(num_jobs*sizeof(int));
    assert_not_equals(worker_fn_args, NULL);

    for(idx=0; idx<num_jobs; idx++) {
        worker_fn_args[idx] = -1;
    }
    

    result = pthread_atfork(&prefork_handler,
                            &postfork_parent_handler,
                            &postfork_child_handler);
    assert_equals(result, 0);

    // if requested, we will fork (to test that the fork handlers
    //   work and still make the module usable)
    if (mode & fork_in_init) {
        debug(1, "forking");
        pid = fork();
        debug(1, "past fork");
    
        if (pid > 0) {
        // (if we do not do this, its possible for the test to register
        //  as "success" when there is really a failure in the child)
            assert_child_exited(pid, 0);
        }
    }
    
    result = pthread_mutex_lock(&worker_fn_mutex);
    assert_equals(result, 0);
}

void check_job_free(void)
{
    int result;
    
    result = pthread_mutex_unlock(&worker_fn_mutex);
    assert_equals(result, 0);
    result = pthread_cond_destroy(&worker_fn_start_2);
    assert_equals(result, 0);
    pthread_mutex_destroy(&worker_fn_mutex);
    free(worker_fn_args);
    debug(1, "=> done with free");
}

void *cancellation_handler(void *arg)
{
    debug(1, "--> (cancellation happened)");
    int *val = (int *) arg;
    *val = -200;
    return NULL;
}

void *worker_fn(void *arg)
{
    const int debug = 1;

    int result;
    int *val = (int *) arg;
    assert_not_equals(val, NULL);

    // step 1
    debug(debug, "--> waiting for lock");
    result = pthread_mutex_lock(&worker_fn_mutex);
    pthread_cleanup_push((push_fn_t) pthread_mutex_unlock, &worker_fn_mutex);
    assert_equals(result, 0);

    // step 1 work
    debug(debug, "--> got lock, doing work 1");
    assert_equals(*val, -1);
    *val = *val + 1;
    assert_equals(*val, 0);
    worker_fn_count_1++;
    
    // signal step 1 done
    debug(debug, "--> signalling done 1 & waiting");
    result = pthread_cond_signal(&worker_fn_done_1);
    assert_equals(result, 0);
    pthread_cleanup_push((push_fn_t) cancellation_handler, val);
    
    // release mutex, wait for step 2
    result = pthread_cond_wait(&worker_fn_start_2, &worker_fn_mutex);
    assert_equals(result, 0);

    // step 2 work
    debug(debug, "--> got lock, doing work 2");
    *val = *val + 1;
    assert_equals(*val, 1);
    worker_fn_count_2++;

    // signal step 2 done
    debug(debug, "--> signalling done 2 & unlocking");
    result = pthread_cond_signal(&worker_fn_done_2);
    assert_equals(result, 0);
    pthread_cleanup_pop(0); // cancellation_handler, do not run
    pthread_cleanup_pop(0); // pthread_mutex_unlock
    pthread_mutex_unlock(&worker_fn_mutex);

    if (mode & exit_early) {
        debug(debug, "--> exiting");
        exit(0);
    }
    debug(debug, "--> returning");
    return NULL;
}

void *sleepy_worker_fn(void *arg)
{
    int result;
    int *sec = (int *) arg;
    assert_not_equals(sec, NULL);

    sleep(*sec);
    *sec = 0;

    return NULL;
}


// test:
//     standard test; check to make sure they all do their jobs
void test_work_0(void)
{
    const int debug = 1;

    int result, idx, active, ans, q_type, q_len;
    thr_pool_handle_t handle;
    worker_fn_count_1 = worker_fn_count_2 = 0;

    // init & lock
    check_job_init();
    active = (num_max_thr < num_jobs) ? num_max_thr : num_jobs;
    q_type = (mode && copy_mode) ? 1 : 0;
    q_len = (mode && copy_mode) ? sizeof(worker_fn_args[idx]) : 0;

    // make pool, and jobs
    debug(debug, "=> create");
    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    debug(debug, "=> queue");
    for(idx=0; idx<num_jobs; idx++) {
        result = thr_pool_queue(handle, q_type, q_len, 
                                &worker_fn, &(worker_fn_args[idx]));
        assert_equals(result, THR_POOL_SUCCESS);
    }

    // check values -- all should be -1
    for(idx=0; idx<num_jobs; idx++) {
        assert_equals(worker_fn_args[idx], -1);
    }

    // step 1: workers grab mutex, do work, signal done_1, then wait on worker_fn_start_2
    while (worker_fn_count_1 < num_jobs) {
        debug(debug, "=> waiting on done_1");
        result = pthread_cond_wait(&worker_fn_done_1, &worker_fn_mutex);
        assert_equals(result, 0);
    }

    // check step 1 -- workers should set value to 0
    debug(debug, "=> check step 1");
    ans = (mode && copy_mode) ? -1 : 0;
    for(idx=0; idx<num_jobs; idx++) {
        assert_equals(worker_fn_args[idx], ans);
    }

    // signal step 2
    debug(debug, "=> broadcast to start_2");
    result = pthread_cond_broadcast(&worker_fn_start_2);
    assert_equals(result, 0);

    // wait for step 2 done
    while (worker_fn_count_2 < num_jobs) {
        debug(1, "=> waiting on done_2");
        result = pthread_cond_wait(&worker_fn_done_2, &worker_fn_mutex);
        assert_equals(result, 0);
    }

    // check step 2 -- workers should set value to 1
    debug(debug, "=> check step 2");
    ans = (mode && copy_mode) ? -1 : 1;
    for(idx=0; idx<num_jobs; idx++) {
        assert_equals(worker_fn_args[idx], ans);
    }

    // destroy pool
    debug(debug, "=> destroying pool");
    result = thr_pool_destroy(handle);
    assert_equals(result, THR_POOL_SUCCESS);

    // done
    debug(debug, "=> done");
    check_job_free();
}

// test:
//   (variant of test_0)
//      queue is in copy-mode
//      so, local values never change
// purpose: makes sure copy mode works
void test_work_1(void)
{
    mode |= copy_mode;
    test_work_0;
}

// test:
//     make as many threads as jobs
//     check to make sure they all do their jobs
void test_work_2(void)
{
    num_min_thr = num_max_thr = num_jobs;
    test_work_0();
}

// test:
//     make as many threads as jobs
//     check to make sure they all do their jobs
//     use a significant number of threads
void test_work_2b(void)
{
    int error;

    // should support THR_POOL_MAX_THREADS, but test system doesn't
    num_min_thr = num_max_thr = num_jobs = THR_POOL_MAX_THREADS - 3;
    test_work_0();
}

// test:
//      make less threads than jobs
//      check to make sure they all do their jobs
// purpose: checks thread re-use
void test_work_3(void)
{
    num_max_thr = num_min_thr = 1;
    test_work_0;
}

// test:
//      same as test_work_3, but worker_fn exits early via pthread_exit
// purpose: tests thread re-use in presence of 'exit early' condition
void test_work_4(void)
{
    mode |= exit_early;
    test_work_3();
}

// test:
//   (variant of test_0)
//      make threads do the first part of their job
//      destroy the pool
//      make sure they went through their cancellation handlers
//      broadcast; make sure nothing has changed 
//      (after a broadcast, they would have incremented the value)
// purpose: checks to make sure threads can be cancelled mid-job
void test_work_5(void)
{
    int result, idx, active;
    thr_pool_handle_t handle;
    worker_fn_count_1 = worker_fn_count_2 = 0;

    // lock
    check_job_init();
    active = (num_max_thr < num_jobs) ? num_max_thr : num_jobs;

    // make pool, and jobs
    debug(1, "=> create");
    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    debug(1, "=> queue");
    for(idx=0; idx<num_jobs; idx++) {
        result = thr_pool_queue(handle, 0, 0, 
                                &worker_fn, &(worker_fn_args[idx]));
        assert_equals(result, THR_POOL_SUCCESS);
    }
    
    // check values; lock being held, nothing can change
    for(idx=0; idx<num_jobs; idx++) {
        assert_equals(worker_fn_args[idx], -1);
    }

    // step 1: workers grab mutex, do work, signal done_1, then wait on worker_fn_start_2
    while (worker_fn_count_1 < active) {
        debug(1, "=> waiting on done_1");
        result = pthread_cond_wait(&worker_fn_done_1, &worker_fn_mutex);
        assert_equals(result, 0);
    }

    // check step 1
    for(idx=0; idx<num_jobs; idx++) {
        assert_equals(worker_fn_args[idx], 0);
    }

    // going to destroy pool (threads will get cancelled)
    // need to unlock mutex, because ...
    // if we hold mutex, threads will deadlock (they try to
    // re-acquire the mutex to get through the cancellation
    // point at pthread_cond_wait, before doing their cancel handlers)
    debug(1, "=> unlock");
    result = pthread_mutex_unlock(&worker_fn_mutex);
    assert_equals(result, 0);

    // destroy pool
    debug(1, "=> destroying pool");
    result = thr_pool_destroy(handle);
    assert_equals(result, THR_POOL_SUCCESS);

    debug(1, "=> locking");
    result = pthread_mutex_lock(&worker_fn_mutex);
    assert_equals(result, 0);

    // check cancellation handlers went
    for(idx=0; idx<num_jobs; idx++) {
        assert_equals(worker_fn_args[idx], -200);
    }

    // signal step 2
    debug(1, "=> broadcast on start_2 and unlocking");
    result = pthread_cond_broadcast(&worker_fn_start_2);
    assert_equals(result, 0);
    result = pthread_mutex_unlock(&worker_fn_mutex);
    assert_equals(result, 0);
    
    // check nothing has changed
    for(idx=0; idx<num_jobs; idx++) {
        assert_equals(worker_fn_args[idx], -200);
    }

    // done
    check_job_free();
}

// test:
//      make threads sleep, exit, and new ones made
// purpose: checks that idle threads leave and new threads are made
void test_work_6(void)
{
    int surplus = 2;
    int idx, result, expiration_1, expiration_2, wait_sec, wait_usec;
    thr_pool_handle_t handle;
    struct timeval t_then, t_now;

    assert_true(timeout > 0);
    assert_true((num_min_thr + surplus) <= num_max_thr);
    assert_true((num_min_thr + surplus) <= num_jobs);

    check_job_init();
    for(idx=0; idx < num_jobs; idx++) {
        worker_fn_args[idx] = 0;
    }
    
    debug(1, "our pool: min = %d, max = %d, timeout = %d\n"
             "\t we will create %d threads",
             num_min_thr, num_max_thr, timeout, num_min_thr+surplus);

    // make pool
    debug(1, "=> create");
    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    // add jobs
    debug(1, "=> queue");
    for(idx=0; idx < (num_min_thr + surplus); idx++) {
        worker_fn_args[idx] = (idx < surplus) ? timeout : 6*timeout;
        debug(1, "for job %d, worker thread will sleep for %d sec",
                 idx, worker_fn_args[idx]);
        result = thr_pool_queue(handle, 0, 0, 
                                &sleepy_worker_fn, &worker_fn_args[idx]);
        assert_equals(result, THR_POOL_SUCCESS);
    }
    gettimeofday(&t_then, NULL);

    // thread will wake after timeout, wait for timeout and then exit
    // so we sill sleep for > 2*timeout
    sleep(3*timeout);

    // check to make sure surplus finished
    for(idx=0; idx < surplus; idx++) {
        assert_equals(worker_fn_args[idx], 0);
    }

    // if new threads are not made to handle these requests
    // then this will take a long time, since we will have
    // to wait for a non-surplus thread to become available
    for(idx=0; idx < surplus; idx++) {
        debug(1, "previous work is done - queuing new job at %d", idx);
        worker_fn_args[idx] = timeout;
        result = thr_pool_queue(handle, 0, 0, 
                                &sleepy_worker_fn, &worker_fn_args[idx]);
        assert_equals(result, THR_POOL_SUCCESS);
    }
    gettimeofday(&t_now, NULL);

    // if we tried to queue and nidle wasn't decremented, we
    // might believe idle threads still exist to handle requests
    // and we would wait at queue() until an active thread finished.
    // if (now - then) is approx. 6*timeout, then this is a problem
    debug(1, "1st queue at %d, 2nd queue 2 at %d, "
             "difference (%d) should be in [%d, %d], "
             "based on how long we chose to sleep between queues",
             timeout, t_then.tv_sec, t_now.tv_sec,
             t_now.tv_sec - t_then.tv_sec,
             3*timeout, 4*timeout);
    assert_false((t_now.tv_sec - t_then.tv_sec) > 4*timeout);

    // we have two expirations; wait until they are both reached
    expiration_1 = (t_then.tv_sec + 6*timeout);
    expiration_2 = (t_now.tv_sec + timeout);
    if (expiration_1 < expiration_2) {
        wait_sec = expiration_2 - t_now.tv_sec;
        wait_usec = 500;
    } else {
        wait_sec = expiration_1 - t_now.tv_sec;
        wait_usec = 500 + (t_then.tv_usec > t_now.tv_usec) ? 
                           t_then.tv_usec: t_now.tv_usec;
    }
    debug(1, "at (%d) will wake (aka, in %d sec, %d usec), "
             "which should be after either timeout:"
             "\n            (%d) 1st queue + 6*timeout  or "
             "\n            (%d) 2nd queue + timeout ",
             t_now.tv_sec + wait_sec, wait_sec, wait_usec,
             expiration_1, expiration_2);
    sleep(wait_sec);
    usleep(wait_usec);
    sleep(5);

    // if we queued the job and it sat there for
    // an existing thread to service it, then
    // the jobs would not be complete now
    for(idx=0; idx < (num_min_thr + surplus); idx++) {
        debug(worker_fn_args[idx], "idx = %d, output = %d",
                                   idx, worker_fn_args[idx]);
        assert_equals(worker_fn_args[idx], 0);
    }

    // destroy pool
    debug(1, "=> destroying pool");
    result = thr_pool_destroy(handle);
    assert_equals(result, THR_POOL_SUCCESS);
}


// test:
// purpose: checks fork handlers of basic module data
void test_work_0f(void)
{
    mode |= fork_in_init;
    test_work_0();
}

// test:
// purpose: checks fork handlers of basic module data
void test_work_1f(void)
{
    mode |= fork_in_init;
    test_work_1();
}

// test:
// purpose: checks fork handlers of basic module data
void test_work_2f(void)
{
    mode |= fork_in_init;
    test_work_2();
}

// test:
// purpose: checks fork handlers of basic module data
void test_work_3f(void)
{
    mode |= fork_in_init;
    test_work_3();
}


// test:
//   (variant of test_0)
//      do step 1; fork
//      broadcast; check step 2
//      in chid: no one should be alive anymore.
//      in parent: they should exist and continue to do work
// purpose: checks fork in middle of using module
void test_work_0_fork(void)
{
    int result, idx, active, ans, q_type, q_len;
    int fork_result, status;
    thr_pool_handle_t handle;
    worker_fn_count_1 = worker_fn_count_2 = 0;

    // init
    check_job_init();
    active = (num_max_thr < num_jobs) ? num_max_thr : num_jobs;
    q_type = (mode && copy_mode) ? 1 : 0;
    q_len = (mode && copy_mode) ? sizeof(worker_fn_args[idx]) : 0;

    // make pool, and jobs
    debug(1, "=> create");
    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    debug(1, "=> queue");
    for(idx=0; idx<num_jobs; idx++) {
        result = thr_pool_queue(handle, q_type, q_len, 
                                &worker_fn, &(worker_fn_args[idx]));
        assert_equals(result, THR_POOL_SUCCESS);
    }

    // check values -- all should be -1
    for(idx=0; idx<num_jobs; idx++) {
        assert_equals(worker_fn_args[idx], -1);
    }

    // step 1: workers grab mutex, do work, signal done_1, then wait on worker_fn_start_2
    while (worker_fn_count_1 < num_jobs) {
        debug(1, "=> waiting on done_1");
        result = pthread_cond_wait(&worker_fn_done_1, &worker_fn_mutex);
        assert_equals(result, 0);
    }

    // check step 1 -- workers should set value to 0
    debug(1, "=> check step 1");
    ans = (mode && copy_mode) ? -1 : 0;
    for(idx=0; idx<num_jobs; idx++) {
        assert_equals(worker_fn_args[idx], ans);
    }

    // release lock and fork
    result = pthread_mutex_unlock(&worker_fn_mutex);
    assert_equals(result, 0);
    
    fork_result = fork();

    // acquire lock
    result = pthread_mutex_lock(&worker_fn_mutex);
    assert_equals(result, 0);

    // signal step 2
    debug(1, "=> broadcast to start_2");
    result = pthread_cond_broadcast(&worker_fn_start_2);
    assert_equals(result, 0);
    
    if (fork_result > 0) {
    //in parent

        // wait for step 2 done
        while (worker_fn_count_2 < num_jobs) {
            debug(1, "=> waiting on done_2");
            result = pthread_cond_wait(&worker_fn_done_2, &worker_fn_mutex);
            assert_equals(result, 0);
        }

        // check step 2 -- workers should set value to 1
        debug(1, "=> check step 2");
        ans = (mode && copy_mode) ? -1 : 1;
        for(idx=0; idx<num_jobs; idx++) {
            assert_equals(worker_fn_args[idx], ans);
        }
        // destroy pool
        debug(1, "=> destroying pool");
        result = thr_pool_destroy(handle);
        assert_equals(result, THR_POOL_SUCCESS);
        
        assert_child_exited(fork_result, 0);
    } else {
    // in child
    
        sleep(1);
        // after fork, all the jobs are removed, threads are killed,
        // and pools are destroyed, but the module is still initialized

        // check step 2 -- value should still be 0
        debug(1, "=> check step 2");
        ans = (mode && copy_mode) ? -1 : 0;
        for(idx=0; idx<num_jobs; idx++) {
            assert_equals(worker_fn_args[idx], ans);
        }

        debug(1, "=> destroying pool");
        result = thr_pool_destroy(handle);
        debug(1, "   in child, the pool should no longer exist");
        assert_equals(result, THR_POOL_INVALID_HANDLE);
    }
    // done
    check_job_free();
}


//////// test failure cases ///////////////////////////////////////////////////

// test:
//   call thr_pool_create, but getting locks fails
// purpose: checks that module is put into unrecoverable error state
void test_error_1(void)
{
    int result;
    thr_pool_handle_t handle;
    
    replace_function(&pthread_rwlock_wrlock, 
                     &pthread_rwlock_always_fail_EINVAL);
    replace_function(&pthread_rwlock_rdlock, 
                     &pthread_rwlock_always_fail_EINVAL);

    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_UNRECOV_ERROR);

    result = thr_pool_free();
    assert_equals(result, THR_POOL_UNRECOV_ERROR);

    restore_function(&pthread_rwlock_wrlock);
    restore_function(&pthread_rwlock_rdlock);
    skip_teardown = 1;
}


// test:
//   call thr_pool_create, but malloc fails
// purpose: 
//       checks that module returns error
//       that whatever handle is returned is invalid
//       and when resources are made available, the module continues to work
void test_error_2(void)
{
    int result;
    thr_pool_handle_t handle;

    replace_function(&malloc, 
                     &malloc_always_fail);

    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_RESOURCE_ERROR);

    result = thr_pool_destroy(handle);
    assert_equals(result, THR_POOL_INVALID_HANDLE);

    restore_function(&malloc);

    result = thr_pool_create(num_min_thr, num_max_thr, timeout, NULL, &handle);
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_destroy(handle);
    assert_equals(result, THR_POOL_SUCCESS);
}



//////// test_pathological_1 --- race condition with multiple queue-ers ///////

typedef struct {
    int counter;
    int fn_arg;
    void *(*fn)(void *);
    thr_pool_handle_t pool;
} jockey_args_t;

// horse does a very simple job, fast
void horse_worker_fn(void *arg)
{
    int *idx = (int *) arg;
    assert_not_equals(idx, NULL);

    debug(1, "horse carrying jockey %d", *idx);
    *idx = getpid();

    return NULL;
}

// racer will queue a job for the horse when counter hits 0
void *jockey_worker_fn(void *arg)
{
    int result;
    jockey_args_t *args = (jockey_args_t *) arg;
    assert_not_equals(args, NULL);

    // wait for the gate to open
    while (args->counter != 0) {
        usleep(300);
    }
    // ride a horse
    args->fn_arg = getpid();
    debug(1, "queuing");
    result = thr_pool_queue(args->pool, 0, 0, args->fn, &(args->fn_arg));
    assert_equals(result, THR_POOL_SUCCESS);

    args->counter = getpid();
    debug(1, "done");
    return NULL;
}

// A scenario in which multiple simultenaously queued jobs
//  cause the queue-ers to see an idle thread, and wait
//  to signal the idle worker during the queue
//
//	Two pools -- jockeys and horses -- where jockeys work a horse.
// Its pathological because all the jockeys try to use the same horse. 
// When the gate opens, they all race to get a horse. 
// with high probability they all see an idle horse available,
// and attempt to signal it, to get it to accept their queued jobs.
//
// This consistently simulates a pathological error in which:
//  * jockeys waiting to queue to the idle horse never get signaled
//    that its their turn to queue
//  * the horse does several jobs in a row without becoming idle
//    and fails to acknowledge a future jockey's signal re: a previously
//    queued job.
// Previously (before the 2011-01-31 change to thr_pool.c),
// both scenarios cause jockeys to get stuck in the queue routine
//
void test_pathological_1(void)
{
    int result, idx, done;
    thr_pool_handle_t horse_pool, jockey_pool;
    jockey_args_t *jockey_args;

    // this is the pathological scenario
    const int num_horses = 1;
    const int num_jockeys = num_horses * 10;

    // one horse
    result = thr_pool_create(num_horses, num_horses, timeout, NULL, &horse_pool);
    assert_equals(result, THR_POOL_SUCCESS);

    // many jockeys
    result = thr_pool_create(num_jockeys, num_jockeys, timeout, NULL, &jockey_pool);
    assert_equals(result, THR_POOL_SUCCESS);

    jockey_args = malloc(num_jockeys * sizeof(jockey_args_t));
    assert_not_equals(jockey_args, NULL);
    for(idx = 0; idx < num_jockeys; idx++) {
        jockey_args[idx].counter = -1;
        jockey_args[idx].fn = &horse_worker_fn;
        jockey_args[idx].fn_arg = 0;
        jockey_args[idx].pool = horse_pool;
    }

    // assemble the jockeys
    for(idx = 0; idx < num_jockeys; idx++) {
        result = thr_pool_queue(jockey_pool, 0, 0, &jockey_worker_fn, &(jockey_args[idx]));
        assert_equals(result, THR_POOL_SUCCESS);
    }

    // open the gates
    sleep(1);
    for(idx = 0; idx < num_jockeys; idx++) {
        jockey_args[idx].counter = 0;
    }

    do {
        done = 1;
        for(idx = 0; idx < num_jockeys; idx++) {
            if (jockey_args[idx].counter == 0) {
                done = 0;
            };
        }
    } while (!done);
    debug(1, "jockeys done");

    do {
        done = 1;
        for(idx = 0; idx < num_jockeys; idx++) {
            if (jockey_args[idx].fn_arg == -1) {
                done = 0;
            };
        }
    } while (!done);
    debug(1, "horse done");

    result = thr_pool_destroy(jockey_pool);
    assert_equals(result, THR_POOL_SUCCESS);

    result = thr_pool_destroy(horse_pool);
    assert_equals(result, THR_POOL_SUCCESS);

    free(jockey_args);
}



/*! \file      thr_pool.h
**  \brief     The NPS Pthread Pool module interface.
** 
**  \details   Inferface for a module to maniulate a simple POSIX
**     thread pool. Each pool is manger-less, and supports a min- and max-
**     thread population.
**
**    - To create a new pool, see thr_pool_create().
**      The module can manage multiple pools; the caller is given
**      a handle to identify the pool, upon pool creation.
**    - To enqueue a job in a pool, see thr_pool_queue().
**    - To destroy a pool, see thr_pool_destroy().
**
**  \pre       Initialize the module via thr_pool_init().
**  \post      Free the module via thr_pool_free().
*//*!
**  \mainpage
**    @copydoc thr_pool.h
*/

#ifndef THR_POOL_H_
#define THR_POOL_H_

#include <pthread.h>
#include <limits.h>

#ifdef PTHREAD_THREADS_MAX
  #define THR_POOL_MAX_NUM       (PTHREAD_THREADS_MAX)
#else
  #define THR_POOL_MAX_NUM       (1024)
#endif


//! The type defining the "handle" to a specific pool
typedef int thr_pool_handle_t;

//! max number of pools (= 2^15 - 1)
#define THR_POOL_MAX_POOLS       32767

//! \brief max number of threads that can be in any pool.
//! \details We use 2 less than THR_POOL_MAX_NUM since
//!  - LinuxThreads has a manager thread
//!  - the caller uses one thread
#define THR_POOL_MAX_THREADS     (THR_POOL_MAX_NUM - 2)


//
// Module return values
//

//! All return codes are in the interval [THR_POOL_BASE, THR_POOL_LAST].
#define THR_POOL_BASE                    (-100)

//! Return code: operation successful.
#define THR_POOL_SUCCESS                 (THR_POOL_BASE)
//! Return code: a soft error ocurred; the pool might still be usable.
#define THR_POOL_ERROR                   (THR_POOL_BASE - 1)
//! Return code: a hard error ocurred; the pool may not be usable further.
#define THR_POOL_UNRECOV_ERROR           (THR_POOL_BASE - 2)
//! Return code: operation failed because the pool was not initialized.
#define THR_POOL_NOT_INITIALIZED         (THR_POOL_BASE - 3)
//! Return code: operation failed because the input was bad.
#define THR_POOL_BAD_INPUT               (THR_POOL_BASE - 4)
//! Return code: operation failed because the input pool handle was invalid.
#define THR_POOL_INVALID_HANDLE          (THR_POOL_BASE - 5)
//! Return code: a memory- or resource-related error ocurred.
#define THR_POOL_RESOURCE_ERROR          (THR_POOL_BASE - 6)

//! A constant (ensures external / internal return codes don't overlap).
#define THR_POOL_LAST                    (THR_POOL_BASE - 6)


//
// Prototypes
//

int thr_pool_init(void);
int thr_pool_create(const unsigned int pool_min, const unsigned int pool_max,
                    const unsigned int timeout, const pthread_attr_t *attr,
                    thr_pool_handle_t *pool_handle);
int thr_pool_queue(const thr_pool_handle_t pool_handle, 
                   const int mode, const size_t len, 
                   void *(*func)(void *), void *arg);
int thr_pool_destroy(const thr_pool_handle_t pool_handle);
int thr_pool_free(void);


#endif // THR_POOL_H_

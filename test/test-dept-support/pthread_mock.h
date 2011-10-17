#ifndef PTHREAD_MOCK_H_
#define PTHREAD_MOCK_H_

#include <pthread.h>
#include <errno.h>


//  Mock for functions of type int foo(pthread_rwlock_t *)
//         which always returns the error EINVAL.
//
int pthread_rwlock_always_fail_ENOMEM(pthread_rwlock_t *rwlock);


// Mock for functions of type int foo(pthread_rwlock_t *)
// which always returns the error EINVAL.
//
// Mock for functions of type int foo(pthread_rwlock_t *)
//         which always returns the error EAGAIN.
int pthread_rwlock_always_fail_EINVAL(pthread_rwlock_t *rwlock);


// Mock for functions of type int foo(pthread_rwlock_t *)
// which always returns the error EAGAIN.
//
// Mock for functions of type int foo(pthread_rwlock_t *)
//         which always returns the error EAGAIN.
int pthread_rwlock_always_fail_EAGAIN(pthread_rwlock_t *rwlock);


// Mock for functions of type int foo(pthread_rwlock_t *)
// which always returns the error EBUSY.
//
// Mock for functions of type int foo(pthread_rwlock_t *)
//         which always returns the error EAGAIN.
int pthread_rwlock_always_fail_EBUSY(pthread_rwlock_t *rwlock);


// Mock for functions of type int foo(pthread_rwlock_t *)
// which always returns the error EDEADLK.
//
// Mock for functions of type int foo(pthread_rwlock_t *)
//         which always returns the error EAGAIN.
int pthread_rwlock_always_fail_EDEADLK(pthread_rwlock_t *rwlock);


// Mock for functions of type int foo(pthread_rwlock_t *)
// which always returns the error EPERM.
//
// Mock for functions of type int foo(pthread_rwlock_t *)
//         which always returns the error EAGAIN.
int pthread_rwlock_always_fail_EPERM(pthread_rwlock_t *rwlock);


#endif // PTHREAD_MOCK_H_


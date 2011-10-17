#include <pthread.h>
#include <errno.h>
#include "pthread_mock.h"

// Internal macro to implement pthread_rwlock_always_fail_ENOMEM()
//   and related functions
//
#define __mock_pthread_rwlock_always_fail(error)                              \
int pthread_rwlock_always_fail_ ## error (pthread_rwlock_t *rwlock)           \
{                                                                             \
    return error;                                                             \
}

__mock_pthread_rwlock_always_fail(ENOMEM);
__mock_pthread_rwlock_always_fail(EINVAL);
__mock_pthread_rwlock_always_fail(EAGAIN);
__mock_pthread_rwlock_always_fail(EBUSY);
__mock_pthread_rwlock_always_fail(EDEADLK);
__mock_pthread_rwlock_always_fail(EPERM);


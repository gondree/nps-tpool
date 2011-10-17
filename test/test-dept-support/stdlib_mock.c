#include <stdlib.h>
#include <errno.h>
#include "stdlib_mock.h"


// Mock interface for malloc() which always returns failure
void *malloc_always_fail(size_t size)
{
    errno = ENOMEM;
    return NULL;
}





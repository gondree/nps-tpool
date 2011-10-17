#ifndef STDLIB_MOCK_H_
#define STDLIB_MOCK_H_

#include <stdlib.h>


// Mock interface for malloc() which always returns failure
void *malloc_always_fail(size_t size);


#endif // STDLIB_MOCK_H_


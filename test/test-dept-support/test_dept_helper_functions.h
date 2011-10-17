#ifndef TEST_DEPT_HELPER_FUNCTIONS_H_
#define TEST_DEPT_HELPER_FUNCTIONS_H_

#include <stdio.h>


// light-weight debugging that works on any platform
#define debug(a, f, ...) do { \
                           if (a) { \
                              fprintf(stderr, "%5d: ", getpid()); \
                              fprintf(stderr, f, ## __VA_ARGS__); \
                              fprintf(stderr, "\n"); \
                              fflush(stderr); \
                           } \
                         } while (0)


// Do not use exit directly - use safe_exit instead
void safe_exit(int code);

// Asserts that a child exited with a certain status
void assert_child_exited(int pid, int status);


#endif // TEST_DEPT_HELPER_FUNCTIONS_H_


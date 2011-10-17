#
# NPS pthread pool Project Makefile
#

.PHONY: all clean tests

#
# Paths
#
VPATH  += src test test/test-dept-support
TEST_DEPT=test/test-dept/src
TEST_SUPPORT=test/test-dept-support
TEST_DEPT_BIN_PATH=$(TEST_DEPT)
TEST_DEPT_INCLUDE_PATH=$(TEST_DEPT)

#
# Flags
#
CC = gcc
LDFLAGS = -pthread -lrt
CFLAGS  = -g -w -Iinclude  -I$(TEST_DEPT) -I$(TEST_SUPPORT)
CFLAGS += -DDEBUG_THR_POOL=0   # leveled debug, between 0 ... 6


all: libtpool.a

clean:
	rm -f *.o libpool.a thr_pool_test

tests: thr_pool_test
	sh test/test-dept/src/test_dept thr_pool_test 2> /dev/null



%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

libtpool.a: thr_pool.o
	ar crv $@ $^


#
# Test-related
#
TEST_SRCS = thr_pool_test.c

thr_pool_test: pthread_mock.o stdlib_mock.o \
               test_dept_helper_functions.o

include $(TEST_DEPT)/test-dept.mk

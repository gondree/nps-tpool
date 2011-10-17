#include <test-dept.h>
#include <test_dept_helper_functions.h>
#include <stdlib.h>
#include <sys/wait.h>


extern int test_dept_test_failures;
extern void teardown(void);


// We want to be ensure teardown is performed and errors get reported
void safe_exit(int code)
{
	int combined_exit_code = code;
    teardown();
    combined_exit_code += test_dept_test_failures;
    debug(1, "exit(%d)", combined_exit_code);
   	exit(combined_exit_code);
} // safe_exit


// Asserts that a child exited with a certain status
void assert_child_exited(int pid, int status)
{
    int error, pid_status;

    error = waitpid(pid, &pid_status, 0);
    debug(1, "waitpid(%d) = %d", pid, pid_status);

    if (WIFSIGNALED(pid_status)) {
    	debug(1, "WIFSIGNALED %d", WTERMSIG(pid_status));
    }
    if (WCOREDUMP(pid_status)) {
    	debug(1, "WCOREDUMP");
    }
    if (WIFSTOPPED(pid_status)) {
    	debug(1, "WIFSTOPPED %d", WSTOPSIG(pid_status));
    }
    assert_true(WIFEXITED(pid_status));
    assert_equals(pid_status, status);
} // assert_child_exited


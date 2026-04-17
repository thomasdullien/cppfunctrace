/* Fork-safety smoke binary used by the fork integration gtest.
 *
 * Parent records ~100 calls, forks, child exits immediately (either
 * via SIGTERM or normal exit depending on the argv).  The parent
 * waits for the child, records a few more calls, then exits. The
 * test asserts that the parent's .ftrc is a valid, complete trace
 * containing events from both phases and no garbage from the child's
 * inherited handlers. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int before_fork (int x) { return x * 3 + 1; }
static int after_fork  (int x) { return x - 7; }
static int in_child    (int x) { return x ^ 0x5a; }

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "exit";

    volatile int acc = 0;
    for (int i = 0; i < 100; i++) acc += before_fork(i);

    pid_t pid = fork();
    if (pid == 0) {
        /* Default modes exercise the fork-safety guard — the child
         * must not leak into the parent's trace. "trace-both" is for
         * the opt-in CPPFUNCTRACE_TRACE_CHILDREN=1 path: the child
         * does some instrumented work before exiting so its own
         * .ftrc file is populated. */
        if      (strcmp(mode, "sigterm")    == 0) raise(SIGTERM);
        else if (strcmp(mode, "sigabrt")    == 0) raise(SIGABRT);
        else if (strcmp(mode, "trace-both") == 0) {
            volatile int c = 0;
            for (int i = 0; i < 30; i++) c += in_child(i);
            /* Normal exit() so atexit handlers fire → child's buffer
             * flushes into its own <child_pid>.ftrc. */
            exit(0);
        }
        else _exit(0);
        _exit(1);
    }
    if (pid < 0) { perror("fork"); return 1; }

    int status = 0;
    waitpid(pid, &status, 0);

    for (int i = 0; i < 50; i++) acc += after_fork(i);
    printf("acc=%d child_status=%d\n", acc, status);
    return 0;
}

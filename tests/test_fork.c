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

static int before_fork(int x) { return x * 3 + 1; }
static int after_fork (int x) { return x - 7; }

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "exit";

    volatile int acc = 0;
    for (int i = 0; i < 100; i++) acc += before_fork(i);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: do not call any instrumented function. Crash or exit
         * immediately so any inherited signal / atexit handlers fire
         * with collecting still COW-inherited as 1. */
        if (strcmp(mode, "sigterm") == 0) raise(SIGTERM);
        else if (strcmp(mode, "sigabrt") == 0) raise(SIGABRT);
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

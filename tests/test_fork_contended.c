/* test_fork_contended — fork() while another thread is inside the
 * tracer's cold path.
 *
 * The worker thread calls many distinct functions rapidly, forcing
 * repeated ft_intern_fp_slow (hence cold_mutex) calls. The main
 * thread forks at random, then joins the worker. With the
 * pthread_atfork handlers in place, the fork succeeds and the parent
 * finishes cleanly; without them, fork() could return in a child
 * that inherits cold_mutex in an inconsistent state — in practice
 * that still only deadlocks if the child tries to use the tracer,
 * which our PID guard prevents, so this test mostly asserts "no hang
 * and no crash" under contention. */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* Many tiny distinct functions so every call path generates a new
 * symbol-resolution cold miss. */
#define DEFN(i) static int f##i(int x) { return x + (i); }
DEFN(0)  DEFN(1)  DEFN(2)  DEFN(3)  DEFN(4)  DEFN(5)  DEFN(6)  DEFN(7)
DEFN(8)  DEFN(9)  DEFN(10) DEFN(11) DEFN(12) DEFN(13) DEFN(14) DEFN(15)
DEFN(16) DEFN(17) DEFN(18) DEFN(19) DEFN(20) DEFN(21) DEFN(22) DEFN(23)
DEFN(24) DEFN(25) DEFN(26) DEFN(27) DEFN(28) DEFN(29) DEFN(30) DEFN(31)

typedef int (*fn_t)(int);
static fn_t fns[32] = {
    f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,
    f8,  f9,  f10, f11, f12, f13, f14, f15,
    f16, f17, f18, f19, f20, f21, f22, f23,
    f24, f25, f26, f27, f28, f29, f30, f31,
};

static volatile int worker_done = 0;

static void* worker(void* arg) {
    (void)arg;
    int acc = 0;
    /* Hammer on the cold path: each call to a never-seen fn[] slot
     * forces symbol resolution under cold_mutex. After we cycle once
     * we still keep calling to keep the writer thread busy. */
    for (int round = 0; round < 500 && !worker_done; round++) {
        for (int i = 0; i < 32; i++) acc += fns[i](round);
    }
    return (void*)(long)acc;
}

int main(void) {
    pthread_t th;
    pthread_create(&th, NULL, worker, NULL);

    /* Let the worker run a bit so it's actively interning. */
    for (int i = 0; i < 50000; i++) {
        if (i % 1000 == 0) sched_yield();
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child does zero instrumented work — tests only that fork()
         * itself returned cleanly in the child. */
        _exit(0);
    }
    if (pid < 0) { perror("fork"); worker_done = 1; pthread_join(th, NULL); return 1; }
    int status = 0;
    waitpid(pid, &status, 0);

    worker_done = 1;
    void* ret = NULL;
    pthread_join(th, &ret);

    printf("worker_acc=%ld child_status=%d\n", (long)ret, status);
    return 0;
}

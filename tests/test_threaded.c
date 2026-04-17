#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compute(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += (i * 31) ^ (s >> 3);
    return s;
}

static int accumulate(int rounds) {
    int acc = 0;
    for (int i = 0; i < rounds; i++) acc += compute(100);
    return acc;
}

static void* worker(void* arg) {
    char name[16];
    snprintf(name, sizeof(name), "worker-%ld", (long)arg);
    pthread_setname_np(pthread_self(), name);
    volatile int r = accumulate(1000);
    (void)r;
    return NULL;
}

int main(int argc, char** argv) {
    int nthreads = argc > 1 ? atoi(argv[1]) : 4;
    pthread_t th[32];
    if (nthreads > 32) nthreads = 32;
    for (int i = 0; i < nthreads; i++) {
        pthread_create(&th[i], NULL, worker, (void*)(long)i);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    printf("all %d threads done\n", nthreads);
    return 0;
}

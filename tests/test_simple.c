#include <stdio.h>
#include <stdlib.h>

static int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

static int sum_squares(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += i * i;
    return s;
}

static void do_work(int rounds) {
    for (int i = 0; i < rounds; i++) {
        volatile int x = fib(10 + (i & 3));
        x += sum_squares(1000);
        (void)x;
    }
}

int main(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 10;
    do_work(rounds);
    printf("done after %d rounds\n", rounds);
    return 0;
}

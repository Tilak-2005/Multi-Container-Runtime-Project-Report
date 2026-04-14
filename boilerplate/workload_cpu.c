/*
 * workload_cpu.c — CPU-bound workload
 *
 * Runs a tight arithmetic loop for a configurable number of iterations,
 * then reports elapsed wall-clock time. Used for scheduling experiments.
 *
 * Usage: workload_cpu [iterations]
 *   Default: 500,000,000 iterations
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    long long iters = 500000000LL;
    if (argc >= 2)
        iters = atoll(argv[1]);

    printf("[cpu-workload] starting %lld iterations\n", iters);
    fflush(stdout);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* LCG — keeps the compiler from optimising the loop away */
    volatile long long x = 1;
    for (long long i = 0; i < iters; i++)
        x = x * 6364136223846793005LL + 1442695040888963407LL;

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed = (double)(t1.tv_sec  - t0.tv_sec)
                   + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("[cpu-workload] done in %.3f s (x=%lld)\n", elapsed, x);
    fflush(stdout);
    return 0;
}

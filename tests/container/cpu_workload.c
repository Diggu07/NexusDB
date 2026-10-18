/*
 * NexusDB — controlled CPU workload helper (P4, test-only).
 *
 * A finite, wall-clock-bounded CPU burner used by tests/container/
 * cpu_enforce_test.c to demonstrate real cgroup v2 CPU throttling. It spins on
 * integer arithmetic (kept live through a volatile accumulator so the compiler
 * cannot delete it) for `--milliseconds` and prints the iteration count, which
 * lets the test show that a throttled run completes far less work in the same
 * wall time.
 *
 * POSIX only (CLOCK_MONOTONIC). On Windows it exits 0 immediately; the enforce
 * test reports ENV-LIMITED there.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char** argv)
{
    long ms = 1000;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--milliseconds") == 0 && i + 1 < argc) {
            ms = strtol(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "usage: %s [--milliseconds N]\n", argv[0]);
            return 2;
        }
    }
    if (ms <= 0)
        ms = 1000;

#ifdef _WIN32
    (void)ms;
    return 0;
#else
    {
        struct timespec t0;
        struct timespec t1;
        volatile unsigned long long acc = 1;
        unsigned long long iterations = 0;
        long long elapsed;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            int k;
            for (k = 0; k < 4096; k++)
                acc = acc * 1103515245ULL + 12345ULL;
            iterations += 4096;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            elapsed = (long long)(t1.tv_sec - t0.tv_sec) * 1000LL +
                      (long long)(t1.tv_nsec - t0.tv_nsec) / 1000000LL;
            if (elapsed >= (long long)ms)
                break;
        }
        /* Consume `acc` so the loop is not dead code. */
        printf("cpu_workload: iterations=%llu elapsed_ms=%lld sink=%llu\n",
               iterations, elapsed, acc);
        return 0;
    }
#endif
}

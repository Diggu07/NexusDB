/*
 * NexusDB — controlled memory-pressure workload helper (P4, test-only).
 *
 * Allocates `--bytes` of memory in `--chunk`-sized pieces and touches one byte
 * per page so the pages are actually faulted in and charged to the caller's
 * cgroup. Used by tests/container/mem_enforce_test.c to trigger cgroup v2
 * memory.max enforcement (OOM) and to distinguish the three possible outcomes:
 *
 *   exit 0  the workload finished within the configured limit
 *   exit 4  a malloc() call returned NULL (process allocation failure — not
 *           cgroup enforcement); exercise with a single impossibly-large chunk
 *   SIGKILL the kernel OOM-killed the process while charging it to a cgroup
 *           whose memory.max was exceeded
 *
 * Memory is intentionally leaked (never freed) so pressure accumulates.
 * POSIX only. On Windows it exits 0 immediately.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
    unsigned long long total = 0;
    unsigned long long chunk = 4ULL * 1024ULL * 1024ULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bytes") == 0 && i + 1 < argc) {
            total = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            chunk = strtoull(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "usage: %s --bytes N [--chunk N]\n", argv[0]);
            return 2;
        }
    }
    if (total == 0 || chunk == 0) {
        fprintf(stderr, "usage: %s --bytes N [--chunk N]\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
    return 0;
#else
    {
        const size_t page = 4096;
        unsigned long long done = 0;

        while (done < total) {
            size_t want = (total - done < chunk) ? (size_t)(total - done)
                                                 : (size_t)chunk;
            unsigned char* p = (unsigned char*)malloc(want);
            size_t off;

            if (p == NULL) {
                printf("mem_workload: allocation failed after %llu bytes\n",
                       done);
                fflush(stdout);
                return 4;
            }
            for (off = 0; off < want; off += page)
                p[off] = (unsigned char)(off & 0xff);

            done += want;
            printf("mem_workload: allocated %llu/%llu bytes\n", done, total);
            fflush(stdout);
        }
        printf("mem_workload: done allocated=%llu\n", done);
        fflush(stdout);
        return 0;
    }
#endif
}

/*
 * workload_mem.c — Memory workload for soft/hard-limit experiments.
 *
 * Allocates and touches memory in chunks, printing RSS after each step
 * so the kernel monitor can observe the growth and enforce limits.
 *
 * Usage: workload_mem [total_mib] [chunk_mib] [sleep_ms]
 *   total_mib  : total MiB to allocate          (default 80)
 *   chunk_mib  : MiB allocated per step         (default 10)
 *   sleep_ms   : milliseconds to sleep per step (default 500)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static long read_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, " %ld", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

int main(int argc, char **argv)
{
    int total_mib = (argc >= 2) ? atoi(argv[1]) : 80;
    int chunk_mib = (argc >= 3) ? atoi(argv[2]) : 10;
    int sleep_ms  = (argc >= 4) ? atoi(argv[3]) : 500;

    if (total_mib <= 0 || chunk_mib <= 0) {
        fprintf(stderr, "Error: total_mib and chunk_mib must be positive\n");
        return 1;
    }

    printf("[mem-workload] will allocate %d MiB in %d MiB chunks "
           "(sleep %d ms between chunks)\n",
           total_mib, chunk_mib, sleep_ms);
    fflush(stdout);

    int    chunks  = total_mib / chunk_mib;
    char **ptrs    = calloc((size_t)chunks, sizeof(char *));
    size_t chunk_b = (size_t)chunk_mib * 1024 * 1024;

    if (!ptrs) {
        perror("calloc");
        return 1;
    }

    for (int i = 0; i < chunks; i++) {
        ptrs[i] = malloc(chunk_b);
        if (!ptrs[i]) {
            fprintf(stderr, "[mem-workload] malloc failed at chunk %d\n", i);
            break;
        }
        /* Touch every page to make it resident (defeats CoW / lazy alloc) */
        memset(ptrs[i], (char)i, chunk_b);

        long rss = read_rss_kb();
        printf("[mem-workload] chunk %d/%d allocated, RSS ~ %ld MiB\n",
               i + 1, chunks, rss / 1024);
        fflush(stdout);

        struct timespec ts = {
            .tv_sec  = sleep_ms / 1000,
            .tv_nsec = (long)(sleep_ms % 1000) * 1000000L
        };
        nanosleep(&ts, NULL);
    }

    printf("[mem-workload] holding memory for 5 seconds...\n");
    fflush(stdout);
    sleep(5);

    for (int i = 0; i < chunks; i++)
        free(ptrs[i]);
    free(ptrs);

    printf("[mem-workload] done\n");
    fflush(stdout);
    return 0;
}

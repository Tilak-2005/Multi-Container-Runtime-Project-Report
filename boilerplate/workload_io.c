/*
 * workload_io.c — I/O-bound workload for scheduling experiments.
 *
 * Repeatedly writes a 4 KiB buffer to a temp file and reads it back,
 * simulating a disk-I/O-bound workload. O_SYNC is used to ensure
 * writes actually hit the underlying storage rather than just the
 * page cache, making the workload genuinely I/O-bound.
 *
 * Usage: workload_io [iterations]
 *   Default: 50,000 iterations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define BUF_SIZE 4096

int main(int argc, char **argv)
{
    int iters = 50000;
    if (argc >= 2)
        iters = atoi(argv[1]);

    printf("[io-workload] starting %d iterations "
           "(write+sync+read %d bytes each)\n",
           iters, BUF_SIZE);
    fflush(stdout);

    char path[] = "/tmp/io_workload_XXXXXX";
    int  fd     = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }

    char wbuf[BUF_SIZE], rbuf[BUF_SIZE];
    memset(wbuf, 0xAB, BUF_SIZE);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < iters; i++) {
        if (lseek(fd, 0, SEEK_SET) < 0) { perror("lseek"); break; }
        ssize_t w = write(fd, wbuf, BUF_SIZE);
        if (w != BUF_SIZE) { perror("write"); break; }
        /* fdatasync makes this genuinely I/O bound */
        fdatasync(fd);
        if (lseek(fd, 0, SEEK_SET) < 0) { perror("lseek"); break; }
        ssize_t r = read(fd, rbuf, BUF_SIZE);
        if (r != BUF_SIZE) { perror("read"); break; }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    close(fd);
    unlink(path);

    double elapsed = (double)(t1.tv_sec  - t0.tv_sec)
                   + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("[io-workload] done in %.3f s\n", elapsed);
    fflush(stdout);
    return 0;
}

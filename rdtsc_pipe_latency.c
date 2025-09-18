#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/wait.h>
#include <x86intrin.h>  // __rdtsc, __rdtscp, _mm_lfence
#include <cpuid.h>      // __get_cpuid

static inline uint64_t rdtsc_begin(void) {
    _mm_lfence();                 // serialize before
    return __rdtsc();
}
static inline uint64_t rdtsc_end(void) {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);  // partially serializing; captures TSC_AUX
    _mm_lfence();                 // serialize after
    return t;
}
static void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        exit(1);
    }
}
static double timespec_diff_sec(struct timespec a, struct timespec b) {
    // return a - b in seconds
    return (a.tv_sec - b.tv_sec) + (a.tv_nsec - b.tv_nsec) / 1e9;
}
// Calibrate TSC Hz using CLOCK_MONOTONIC_RAW over ~300ms
static double calibrate_tsc_hz(void) {
    struct timespec t0, t1, req = { .tv_sec = 0, .tv_nsec = 300000000L }; // 300ms
    clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
    uint64_t c0 = rdtsc_begin();
    nanosleep(&req, NULL);
    uint64_t c1 = rdtsc_end();
    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    double dt = timespec_diff_sec(t1, t0);
    return (c1 - c0) / dt;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    int iters = 10000, warmup = 1000, cpu = 0;
    if (argc > 1) iters = atoi(argv[1]);       // optional: iterations to record
    if (argc > 2) warmup = atoi(argv[2]);      // optional: warmup iterations
    if (argc > 3) cpu    = atoi(argv[3]);      // optional: CPU to pin

    int p_ab[2], p_ba[2];
    if (pipe(p_ab) || pipe(p_ba)) { perror("pipe"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        // ---- Child: echo one byte back for each byte received ----
        pin_to_cpu(cpu);
        close(p_ab[1]); // close writer (parent->child)
        close(p_ba[0]); // close reader (child->parent)
        char byte;
        int total = warmup + iters;
        for (int i = 0; i < total; ++i) {
            if (read(p_ab[0], &byte, 1) != 1) { perror("child read"); exit(1); }
            if (write(p_ba[1], &byte, 1) != 1) { perror("child write"); exit(1); }
        }
        close(p_ab[0]);
        close(p_ba[1]);
        _exit(0);
    }

    // ---- Parent: measure round-trip (write->read) ----
    pin_to_cpu(cpu);
    close(p_ab[0]); // close reader
    close(p_ba[1]); // close writer

    double tsc_hz = calibrate_tsc_hz();
    fprintf(stderr, "Calibrated TSC: %.3f MHz\n", tsc_hz / 1e6);

    uint64_t *samples = malloc((size_t)iters * sizeof(uint64_t));
    if (!samples) { perror("malloc"); return 1; }

    char byte = 0xA5;
    // Warmup
    for (int i = 0; i < warmup; ++i) {
        if (write(p_ab[1], &byte, 1) != 1) { perror("warmup write"); return 1; }
        if (read(p_ba[0], &byte, 1) != 1)  { perror("warmup read");  return 1; }
    }
    // Timed iterations
    for (int i = 0; i < iters; ++i) {
        uint64_t t0 = rdtsc_begin();
        if (write(p_ab[1], &byte, 1) != 1) { perror("write"); return 1; }
        if (read(p_ba[0], &byte, 1) != 1)  { perror("read");  return 1; }
        uint64_t t1 = rdtsc_end();
        samples[i] = t1 - t0;  // round-trip cycles
    }

    close(p_ab[1]);
    close(p_ba[0]);

    // Child exit
    int status = 0;
    waitpid(pid, &status, 0);

    // Stats (min / median / mean)
    uint64_t min = UINT64_MAX, sum = 0;
    for (int i = 0; i < iters; ++i) { if (samples[i] < min) min = samples[i]; sum += samples[i]; }
    qsort(samples, iters, sizeof(uint64_t), cmp_u64);
    uint64_t median = samples[iters/2];
    double mean = (double)sum / iters;

    double cyc_to_ns = 1e9 / tsc_hz;
    printf("Pipe round-trip:\n");
    printf("  min    : %10llu cycles  (%.3f ns)\n",
           (unsigned long long)min,   min * cyc_to_ns);
    printf("  median : %10llu cycles  (%.3f ns)\n",
           (unsigned long long)median, median * cyc_to_ns);
    printf("  mean   : %10.1f cycles  (%.3f ns)\n",
           mean,  mean * cyc_to_ns);

    free(samples);
    return 0;
}


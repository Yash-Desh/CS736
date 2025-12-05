// cgt_min.c
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);   // steady; not affected by NTP/date changes
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void) {
    uint64_t t0 = now_ns();

    // --- code to measure ---
    for (volatile int i = 0; i < 1000000; ++i) {}
    // -----------------------

    uint64_t t1 = now_ns();
    uint64_t ns = t1 - t0;

    printf("time: %llu ns (%.6f ms, %.9f s)\n",
           (unsigned long long)ns, ns / 1e6, ns / 1e9);
    return 0;
}

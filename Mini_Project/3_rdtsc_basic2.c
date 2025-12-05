#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <x86intrin.h>
#include <cpuid.h>

static inline void cpuid_serialize(void){
    unsigned a,b,c,d; __get_cpuid(0, &a,&b,&c,&d);
}
static inline uint64_t rdtsc_begin(void){ cpuid_serialize(); return __rdtsc(); }
static inline uint64_t rdtsc_end(void){ unsigned aux; uint64_t t=__rdtscp(&aux); cpuid_serialize(); return t; }

static double tsc_hz(void) {
    unsigned den=0, num=0, crystal=0, d;
    if (__get_cpuid(0x15, &den, &num, &crystal, &d) && den && num && crystal) {
        // TSC Hz = crystal_Hz * (num / den)
        return (double)crystal * (double)num / (double)den;
    }
    // Fallback: calibrate against CLOCK_MONOTONIC_RAW for ~200 ms
    struct timespec a,b, req = { .tv_sec=0, .tv_nsec=200000000L };
    clock_gettime(CLOCK_MONOTONIC_RAW, &a);
    uint64_t c0 = rdtsc_begin();
    nanosleep(&req, NULL);
    uint64_t c1 = rdtsc_end();
    clock_gettime(CLOCK_MONOTONIC_RAW, &b);
    double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec)/1e9;
    return (c1 - c0) / dt;
}

int main(void){
    double hz = tsc_hz();

    uint64_t t0 = rdtsc_begin();
    // --- code to time ---
    for (volatile int i=0;i<1000000;++i) {}
    // --------------------
    uint64_t t1 = rdtsc_end();

    uint64_t cycles = t1 - t0;
    double secs = cycles / hz;
    double nsec = (1e9 * cycles) / hz;

    printf("cycles=%llu  hz=%.3f MHz  time=%.9f s  (%.3f ns)\n",
           (unsigned long long)cycles, hz/1e6, secs, nsec);
    return 0;
}

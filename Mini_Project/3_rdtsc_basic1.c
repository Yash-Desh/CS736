#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>   // __rdtsc, __rdtscp
#include <cpuid.h>       // __get_cpuid

// CPUID is a serializing instruction: it prevents reordering across it.
static inline void cpuid_serialize(void) {
    unsigned int a, b, c, d;
    __get_cpuid(0, &a, &b, &c, &d);
}

static inline uint64_t rdtsc_begin(void) {
    cpuid_serialize();          // serialize BEFORE starting
    return __rdtsc();           // read TSC (not serializing by itself)
}

static inline uint64_t rdtsc_end(void) {
    unsigned aux;
    uint64_t t = __rdtscp(&aux); // read TSC and serialize AFTER prior ops
    cpuid_serialize();           // full fence after the measurement
    return t;
}

int main(void) {
    // ---- code to measure (example: a simple loop) ----
    uint64_t t0 = rdtsc_begin();

    volatile int sink = 0;
    for (int i = 0; i < 1000000; ++i) sink += i;

    uint64_t t1 = rdtsc_end();
    // --------------------------------------------------

    printf("cycles: %llu\n", (unsigned long long)(t1 - t0));
    return 0;
}

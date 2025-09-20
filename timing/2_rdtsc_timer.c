#define _GNU_SOURCE
#include <stdio.h>
#include <x86intrin.h>   // __rdtsc, __rdtscp
#include <cpuid.h>       // __get_cpuid

static inline void cpuid_serialize(void) {
    unsigned int a, b, c, d;
    __get_cpuid(0, &a, &b, &c, &d);  // CPUID serializes
}

int main(void) {
    unsigned int aux;
    unsigned long long t0, t1;

    cpuid_serialize();
    t0 = __rdtsc();

    for (volatile int i = 0; i < 1000000; ++i) {}

    t1 = __rdtscp(&aux);
    cpuid_serialize();

    printf("cycles: %llu\n", (t1 - t0));
    return 0;
}


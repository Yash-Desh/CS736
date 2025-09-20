// #include <stdio.h>
// #include <sys/time.h> // Required for gettimeofday

// int main() {
//     struct timeval tv;
//     int result;

//     result = gettimeofday(&tv, NULL);

//     if (result == -1) {
//         perror("gettimeofday");
//         return 1; // Indicate an error
//     }

//     printf("Current time: %ld seconds, %ld microseconds since Epoch\n",
//            tv.tv_sec, tv.tv_usec);

//     return 0; // Indicate success
// }

// gtod_min.c
#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>

static inline uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);                 // timezone arg is obsolete; pass NULL
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

int main(void) {
    uint64_t t0 = now_us();

    // --- code to measure ---
    for (volatile int i = 0; i < 1000000; ++i) {}
    // -----------------------

    uint64_t t1 = now_us();

    uint64_t us = t1 - t0;
    printf("time: %llu microseconds (%.6f seconds)\n",
           (unsigned long long)us, us / 1e6);
    return 0;
}

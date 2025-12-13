#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void shuffle(size_t *a, size_t n) {
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        size_t tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
}

// diff between two timespecs, return seconds as double
double timespec_diff_sec(struct timespec start, struct timespec end) {
    long sec  = end.tv_sec  - start.tv_sec;
    long nsec = end.tv_nsec - start.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    return (double)sec + (double)nsec / 1e9;
}

int main() {
    size_t N = 1024 * 1024 * 1024;   // 50 million elements (~400 MB)
                                   // Pick any size > LLC for true latency

    double *A = malloc(N * sizeof(double));
    size_t *idx = malloc(N * sizeof(size_t));

    // initialize array
    for (size_t i = 0; i < N; i++) {
        A[i] = (double)i;
        idx[i] = i;
    }

    // randomize index order
    srand(time(NULL));
    shuffle(idx, N);

    // start timing
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    // random memory access loop
    double sum = 0.0;
    for (size_t k = 0; k < N; k++) {
        sum += A[idx[k]];
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed = timespec_diff_sec(t0, t1);

    printf("sum = %.2f\n", sum);
    printf("Random access time: %.6f seconds\n", elapsed);
    printf("Accesses per second: %.2f million\n", (N / 1e6) / elapsed);

    free(A);
    free(idx);
    return 0;
}

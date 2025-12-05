#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <numa.h>
#include <numaif.h>

typedef struct {
    double   *array;        // base pointer to the big array
    size_t    start;        // starting index (in doubles) for this thread
    size_t    length;       // number of elements this thread will touch
    int       compute_node; // NUMA node on which this thread should run
    int       thread_id;    // just for printing
    double    elapsed_sec;  // per-thread time
} thread_arg_t;

static pthread_barrier_t start_barrier;

// simple helper: nanoseconds diff → seconds
static double timespec_diff_sec(struct timespec a, struct timespec b) {
    long sec  = b.tv_sec  - a.tv_sec;
    long nsec = b.tv_nsec - a.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    return (double)sec + (double)nsec / 1e9;
}

void *worker(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;

    // Pin THIS thread's execution to the requested NUMA node.
    // This tells the scheduler "run me on CPUs belonging to this node".
    if (numa_run_on_node(targ->compute_node) != 0) {
        fprintf(stderr, "Thread %d: numa_run_on_node(%d) failed: %s\n",
                targ->thread_id, targ->compute_node, strerror(errno));
        // not fatal, but we warn
    }

    // Optional: also set this node as the preferred one for future allocations
    // from this thread (we don't really allocate inside the thread, but harmless)
    numa_set_preferred(targ->compute_node);

    // Wait until ALL threads are ready before starting the timed region
    pthread_barrier_wait(&start_barrier);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    // --- NUMA-sensitive work loop ---
    // Each thread repeatedly scans its own chunk of the array.
    volatile double sink = 0.0;   // prevent compiler from optimizing away work
    const size_t iters = 10;      // repeat to amplify DRAM traffic

    for (size_t r = 0; r < iters; r++) {
        double local_sum = 0.0;
        for (size_t i = 0; i < targ->length; i++) {
            local_sum += targ->array[targ->start + i];
        }
        sink += local_sum;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    targ->elapsed_sec = timespec_diff_sec(t_start, t_end);

    // use sink so compiler can't drop the loop
    if (targ->thread_id == 0) {
        printf("Ignore this value: %f\n", sink);
    }

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <array_size_MB> <num_threads> <mem_node> <compute_node>\n"
                "  Example (local):  %s 4096 8 0 0\n"
                "  Example (remote): %s 4096 8 1 0\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }

    long array_mb     = strtol(argv[1], NULL, 10);
    int  num_threads  = atoi(argv[2]);
    int  mem_node     = atoi(argv[3]);
    int  compute_node = atoi(argv[4]);

    if (array_mb <= 0 || num_threads <= 0) {
        fprintf(stderr, "array_size_MB and num_threads must be positive.\n");
        return 1;
    }

    if (numa_available() < 0) {
        fprintf(stderr, "libnuma: NUMA not available on this system.\n");
        return 1;
    }

    int max_node = numa_max_node();
    if (mem_node < 0 || mem_node > max_node ||
        compute_node < 0 || compute_node > max_node) {
        fprintf(stderr, "Invalid mem_node or compute_node (max_node = %d).\n", max_node);
        return 1;
    }

    printf("=== Experiment 1: Local vs Remote ===\n");
    printf("Array size : %ld MB\n", array_mb);
    printf("Threads    : %d\n", num_threads);
    printf("mem_node   : %d (where the array lives)\n", mem_node);
    printf("compute_node: %d (where threads run)\n\n", compute_node);

    size_t bytes  = (size_t)array_mb * 1024 * 1024;
    size_t nelems = bytes / sizeof(double);

    // -------- NUMA-CONTROLLED MEMORY ALLOCATION --------
    //
    // numa_alloc_onnode(bytes, node) asks the kernel to back this virtual
    // range with pages physically located on the given NUMA node.
    //
    // This is crucial: it lets us *force* "memory on node 0" vs
    // "memory on node 1" independent of who touches it first.
    //
    double *array = (double *)numa_alloc_onnode(bytes, mem_node);
    if (!array) {
        fprintf(stderr, "numa_alloc_onnode failed.\n");
        return 1;
    }

    // Initialize data (just so the pages get faulted in once).
    // The pages will still be placed on mem_node because of the bind above.
    for (size_t i = 0; i < nelems; i++) {
        array[i] = 1.0;
    }

    // Prepare thread arguments: split array into equal-sized chunks.
    pthread_t    *threads = calloc(num_threads, sizeof(pthread_t));
    thread_arg_t *targs   = calloc(num_threads, sizeof(thread_arg_t));

    size_t chunk = nelems / num_threads;

    pthread_barrier_init(&start_barrier, NULL, num_threads);

    for (int t = 0; t < num_threads; t++) {
        targs[t].array        = array;
        targs[t].start        = (size_t)t * chunk;
        targs[t].length       = chunk;
        targs[t].compute_node = compute_node;
        targs[t].thread_id    = t;
        targs[t].elapsed_sec  = 0.0;

        int rc = pthread_create(&threads[t], NULL, worker, &targs[t]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create(%d) failed: %s\n",
                    t, strerror(rc));
            return 1;
        }
    }

    // Wait for all threads to finish
    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    pthread_barrier_destroy(&start_barrier);

    // Summarize per-thread times
    double max_time = 0.0;
    double sum_time = 0.0;
    for (int t = 0; t < num_threads; t++) {
        printf("Thread %2d elapsed: %f sec\n", t, targs[t].elapsed_sec);
        if (targs[t].elapsed_sec > max_time)
            max_time = targs[t].elapsed_sec;
        sum_time += targs[t].elapsed_sec;
    }
    printf("\nApprox. total time (max over threads): %f sec\n", max_time);
    printf("Average per-thread time: %f sec\n", sum_time / num_threads);

    // Clean up
    numa_free(array, bytes);
    free(threads);
    free(targs);

    return 0;
}

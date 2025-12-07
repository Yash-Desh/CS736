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

typedef enum {
    SCEN_BIND_NODE0      = 0,
    SCEN_PREFERRED_NODE0 = 1,
    SCEN_INTERLEAVE_01   = 2
} scenario_t;

typedef struct {
    double   *array;
    size_t    start;
    size_t    length;
    int       compute_node;   // NUMA node for this thread (we use 0)
    int       thread_id;
    double    elapsed_sec;
} thread_arg_t;

static pthread_barrier_t start_barrier;

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

    // Pin to chosen node (always 0 here)
    if (numa_run_on_node(targ->compute_node) != 0) {
        fprintf(stderr, "Thread %d: numa_run_on_node(%d) failed: %s\n",
                targ->thread_id, targ->compute_node, strerror(errno));
    }
    numa_set_preferred(targ->compute_node);

    pthread_barrier_wait(&start_barrier);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    volatile double sink = 0.0;
    const size_t iters = 10;

    for (size_t r = 0; r < iters; r++) {
        double local_sum = 0.0;
        for (size_t i = 0; i < targ->length; i++) {
            local_sum += targ->array[targ->start + i];
        }
        sink += local_sum;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    targ->elapsed_sec = timespec_diff_sec(t_start, t_end);

    if (targ->thread_id == 0)
        printf("Ignore sink: %f\n", sink);

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr,
            "Usage: %s <array_MB> <num_threads> <scenario> <compute_node>\n"
            "\n"
            "  scenario 0: BIND_NODE0\n"
            "      Working array allocated with numa_alloc_onnode(node 0).\n"
            "      If node 0 is full, allocator may fail or the kernel may still place\n"
            "      some pages remotely (depending on kernel version).\n"
            "\n"
            "  scenario 1: PREFERRED_NODE0\n"
            "      Working array allocated with numa_set_preferred(0) + numa_alloc.\n"
            "      Kernel prefers node 0 but can spill pages to other nodes under pressure.\n"
            "\n"
            "  scenario 2: INTERLEAVE_01\n"
            "      Working array interleaved across nodes 0 and 1.\n"
            "      Placement is independent of node-0 pressure.\n"
            "\n"
            "Example (no pressure):\n"
            "  %s 4096 8 0 0   # 4GB, 8 threads, BIND to node0, threads on node0\n"
            "\n"
            "With pressure (in another terminal first):\n"
            "  ./pressure 0 60000\n"
            "  %s 4096 8 0 0   # run again under node-0 pressure\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    long array_mb    = strtol(argv[1], NULL, 10);
    int  num_threads = atoi(argv[2]);
    int  scenario    = atoi(argv[3]);
    int  compute_node = atoi(argv[4]);

    if (array_mb <= 0 || num_threads <= 0) {
        fprintf(stderr, "array_MB and num_threads must be positive.\n");
        return 1;
    }
    if (scenario < 0 || scenario > 2) {
        fprintf(stderr, "Invalid scenario (0,1,2).\n");
        return 1;
    }

    if (numa_available() < 0) {
        fprintf(stderr, "NUMA not available.\n");
        return 1;
    }

    int max_node = numa_max_node();
    if (compute_node < 0 || compute_node > max_node) {
        fprintf(stderr, "Invalid compute_node (0..%d).\n", max_node);
        return 1;
    }

    printf("=== Experiment 6: Node pressure & fallback ===\n");
    printf("Array size : %ld MB\n", array_mb);
    printf("Threads    : %d\n", num_threads);
    printf("Scenario   : %d\n", scenario);
    printf("Compute node (threads pinned here): %d\n\n", compute_node);

    size_t bytes  = (size_t)array_mb * 1024 * 1024;
    size_t nelems = bytes / sizeof(double);

    double *array = NULL;
    struct bitmask *mask = NULL;

    // --------- Allocate working array according to scenario ----------
    if (scenario == SCEN_BIND_NODE0) {
        printf("Policy: BIND_NODE0 (numa_alloc_onnode)\n");
        array = (double *)numa_alloc_onnode(bytes, 0);
        if (!array) {
            fprintf(stderr, "numa_alloc_onnode failed: %s\n", strerror(errno));
            fprintf(stderr, "This may happen if node 0 is out of memory under pressure.\n");
            return 1;
        }
    } else if (scenario == SCEN_PREFERRED_NODE0) {
        printf("Policy: PREFERRED_NODE0 (numa_set_preferred + numa_alloc)\n");
        numa_set_preferred(0);
        array = (double *)numa_alloc(bytes);
        numa_set_localalloc();  // reset to default for other allocations

        if (!array) {
            fprintf(stderr, "numa_alloc failed: %s\n", strerror(errno));
            return 1;
        }
    } else { // SCEN_INTERLEAVE_01
        printf("Policy: INTERLEAVE_01 (numa_alloc_interleaved_subset)\n");
        mask = numa_allocate_nodemask();
        numa_bitmask_clearall(mask);
        numa_bitmask_setbit(mask, 0);
        numa_bitmask_setbit(mask, 1);

        array = (double *)numa_alloc_interleaved_subset(bytes, mask);
        if (!array) {
            fprintf(stderr, "numa_alloc_interleaved_subset failed: %s\n", strerror(errno));
            numa_free_nodemask(mask);
            return 1;
        }
    }

    // Fault in pages once
    for (size_t i = 0; i < nelems; i++) {
        array[i] = 1.0;
    }

    // --------- Launch worker threads ----------
    pthread_t    *threads = calloc(num_threads, sizeof(pthread_t));
    thread_arg_t *targs   = calloc(num_threads, sizeof(thread_arg_t));
    size_t chunk = nelems / num_threads;

    pthread_barrier_init(&start_barrier, NULL, num_threads);

    for (int t = 0; t < num_threads; t++) {
        targs[t].array        = array;
        targs[t].start        = (size_t)t * chunk;
        targs[t].length       = chunk;
        targs[t].compute_node = compute_node; // all on same node (e.g., 0)
        targs[t].thread_id    = t;
        targs[t].elapsed_sec  = 0.0;

        printf("Thread %2d pinned to node %d (start=%zu, len=%zu)\n",
               t, compute_node, targs[t].start, targs[t].length);

        int rc = pthread_create(&threads[t], NULL, worker, &targs[t]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create(%d) failed: %s\n", t, strerror(rc));
            return 1;
        }
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    pthread_barrier_destroy(&start_barrier);

    // --------- Summarize timing ----------
    double max_time = 0.0, sum_time = 0.0;
    for (int t = 0; t < num_threads; t++) {
        printf("Thread %2d elapsed: %f sec (node %d)\n",
               t, targs[t].elapsed_sec, targs[t].compute_node);
        if (targs[t].elapsed_sec > max_time)
            max_time = targs[t].elapsed_sec;
        sum_time += targs[t].elapsed_sec;
    }
    printf("\nApprox. total time (max over threads): %f sec\n", max_time);
    printf("Average per-thread time: %f sec\n", sum_time / num_threads);

    // --------- Cleanup ----------
    if (scenario == SCEN_INTERLEAVE_01 && mask)
        numa_free_nodemask(mask);

    // numa_* allocs all freed with numa_free
    numa_free(array, bytes);
    free(threads);
    free(targs);

    return 0;
}

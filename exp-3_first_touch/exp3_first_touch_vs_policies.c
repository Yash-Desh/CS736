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
    SCENARIO_SEQ_INIT   = 0, // Case 1: sequential init, parallel work
    SCENARIO_PAR_INIT   = 1, // Case 2: parallel init aligned with work
    SCENARIO_INTERLEAVE = 2  // Case 3: explicit interleave policy
} scenario_t;

typedef struct {
    double   *array;
    size_t    start;
    size_t    length;
    int       compute_node;   // NUMA node this thread runs on
    int       thread_id;
    double    elapsed_sec;
} thread_arg_t;

static pthread_barrier_t start_barrier;
static int g_scenario = 0;       // which scenario we're running (0/1/2)

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

    // Pin this thread to its chosen NUMA node
    if (numa_run_on_node(targ->compute_node) != 0) {
        fprintf(stderr, "Thread %d: numa_run_on_node(%d) failed: %s\n",
                targ->thread_id, targ->compute_node, strerror(errno));
    }
    numa_set_preferred(targ->compute_node);

    // ---------- Phase 1: initialization (only for some scenarios) ----------
    //
    // For PAR_INIT and INTERLEAVE scenarios, each thread initializes
    // its own chunk BEFORE we start timing. For SEQ_INIT, the main
    // thread already did the init, so workers skip this step.
    //
    if (g_scenario == SCENARIO_PAR_INIT || g_scenario == SCENARIO_INTERLEAVE) {
        for (size_t i = 0; i < targ->length; i++) {
            targ->array[targ->start + i] = 1.0;
        }
    }

    // Wait until everyone finishes initialization
    pthread_barrier_wait(&start_barrier);

    // ---------- Phase 2: timed compute ----------
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    volatile double sink = 0.0;
    const size_t iters = 10; // repeat to amplify memory traffic

    for (size_t r = 0; r < iters; r++) {
        double local_sum = 0.0;
        for (size_t i = 0; i < targ->length; i++) {
            local_sum += targ->array[targ->start + i];
        }
        sink += local_sum;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    targ->elapsed_sec = timespec_diff_sec(t_start, t_end);

    if (targ->thread_id == 0) {
        printf("Ignore sink: %f\n", sink);
    }

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
            "Usage: %s <array_size_MB> <num_threads> <scenario>\n"
            "\n"
            "  scenario 0: SEQ_INIT (sequential init, parallel work)\n"
            "      - Allocation: malloc (no policy)\n"
            "      - Init: main thread, pinned to node 0, touches ALL pages\n"
            "      - Work: threads split across node 0 and node 1\n"
            "      => First-touch places ALL pages on node 0; node-1 threads see remote memory\n"
            "\n"
            "  scenario 1: PAR_INIT (parallel init aligned with work)\n"
            "      - Allocation: malloc (no policy)\n"
            "      - Init: each worker thread initializes its OWN chunk,\n"
            "               pinned to its node (half on node0, half on node1)\n"
            "      - Work: same threads, same placement\n"
            "      => First-touch tends to place each page on the node that uses it\n"
            "\n"
            "  scenario 2: INTERLEAVE (explicit interleaved policy)\n"
            "      - Allocation: numa_alloc_interleaved_subset(0+1)\n"
            "      - Init: per-thread, but placement is controlled by interleave policy\n"
            "      - Work: threads split across node 0 and 1\n"
            "      => Pages interleaved regardless of init; every thread gets mix of local/remote\n"
            "\nExample:\n"
            "  %s 4096 8 0   # Case 1: SEQ_INIT\n"
            "  %s 4096 8 1   # Case 2: PAR_INIT\n"
            "  %s 4096 8 2   # Case 3: INTERLEAVE\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    long array_mb    = strtol(argv[1], NULL, 10);
    int  num_threads = atoi(argv[2]);
    g_scenario       = atoi(argv[3]);

    if (array_mb <= 0 || num_threads <= 0) {
        fprintf(stderr, "array_size_MB and num_threads must be positive.\n");
        return 1;
    }
    if (g_scenario < 0 || g_scenario > 2) {
        fprintf(stderr, "Invalid scenario (must be 0,1,2).\n");
        return 1;
    }

    if (numa_available() < 0) {
        fprintf(stderr, "libnuma: NUMA not available on this system.\n");
        return 1;
    }

    int max_node = numa_max_node();
    if (max_node < 1) {
        fprintf(stderr, "Need at least 2 NUMA nodes for this experiment.\n");
        return 1;
    }

    printf("=== Experiment 3: First-touch vs explicit policies ===\n");
    printf("Array size : %ld MB\n", array_mb);
    printf("Threads    : %d\n", num_threads);
    printf("Scenario   : %d\n\n", g_scenario);

    size_t bytes  = (size_t)array_mb * 1024 * 1024;
    size_t nelems = bytes / sizeof(double);

    double *array = NULL;
    struct bitmask *mask = NULL;

    // ---------- Allocation: default vs explicit interleave ----------
    if (g_scenario == SCENARIO_INTERLEAVE) {
        // Explicit interleave across node 0 and node 1
        mask = numa_allocate_nodemask();
        numa_bitmask_clearall(mask);
        numa_bitmask_setbit(mask, 0);
        numa_bitmask_setbit(mask, 1);

        array = (double *)numa_alloc_interleaved_subset(bytes, mask);
        if (!array) {
            fprintf(stderr, "numa_alloc_interleaved_subset failed.\n");
            numa_free_nodemask(mask);
            return 1;
        }
        printf("Allocation: INTERLEAVED across nodes 0 and 1\n");
    } else {
        // Normal malloc: placement determined by first-touch
        array = (double *)malloc(bytes);
        if (!array) {
            fprintf(stderr, "malloc failed.\n");
            return 1;
        }
        printf("Allocation: malloc (default policy, first-touch decides placement)\n");
    }

    // ---------- Scenario-specific initialization BEFORE worker threads ----------
    if (g_scenario == SCENARIO_SEQ_INIT) {
        // SEQ_INIT: main thread (pinned to node 0) initializes all elements.
        //
        // This ensures first-touch on every page comes from node 0, so the
        // kernel places all pages on node 0.
        printf("Initialization: SEQUENTIAL by main thread on node 0\n");
        if (numa_run_on_node(0) != 0) {
            fprintf(stderr, "Main: numa_run_on_node(0) failed: %s\n", strerror(errno));
        }
        numa_set_preferred(0);

        for (size_t i = 0; i < nelems; i++) {
            array[i] = 1.0;
        }

        // After this, all pages are presumably placed on node 0.
        printf("Main init finished; now starting worker threads.\n\n");
    } else if (g_scenario == SCENARIO_PAR_INIT) {
        printf("Initialization: will be done in parallel by worker threads.\n\n");
    } else { // INTERLEAVE
        printf("Initialization: will be done in parallel but placement is interleaved by policy.\n\n");
    }

    // ---------- Prepare worker threads for compute phase ----------
    pthread_t    *threads = calloc(num_threads, sizeof(pthread_t));
    thread_arg_t *targs   = calloc(num_threads, sizeof(thread_arg_t));
    size_t chunk = nelems / num_threads;

    pthread_barrier_init(&start_barrier, NULL, num_threads);

    for (int t = 0; t < num_threads; t++) {
        targs[t].array  = array;
        targs[t].start  = (size_t)t * chunk;
        targs[t].length = chunk;
        targs[t].thread_id = t;

        // For ALL scenarios, we use the same compute placement:
        //   first half of threads pinned to node 0,
        //   second half pinned to node 1.
        int node_for_thread = (t < num_threads / 2) ? 0 : 1;
        targs[t].compute_node = node_for_thread;

        printf("Thread %2d will run on NUMA node %d (start=%zu, len=%zu)\n",
               t, node_for_thread, targs[t].start, targs[t].length);

        int rc = pthread_create(&threads[t], NULL, worker, &targs[t]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create(%d) failed: %s\n",
                    t, strerror(rc));
            return 1;
        }
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    pthread_barrier_destroy(&start_barrier);

    // ---------- Summarize timing ----------
    double max_time = 0.0;
    double sum_time = 0.0;
    for (int t = 0; t < num_threads; t++) {
        printf("Thread %2d elapsed: %f sec (node %d)\n",
               t, targs[t].elapsed_sec, targs[t].compute_node);
        if (targs[t].elapsed_sec > max_time)
            max_time = targs[t].elapsed_sec;
        sum_time += targs[t].elapsed_sec;
    }
    printf("\nApprox. total time (max over threads): %f sec\n", max_time);
    printf("Average per-thread time: %f sec\n", sum_time / num_threads);

    // ---------- Cleanup ----------
    if (g_scenario == SCENARIO_INTERLEAVE) {
        if (mask) numa_free_nodemask(mask);
        numa_free(array, bytes);
    } else {
        free(array);
    }

    free(threads);
    free(targs);

    return 0;
}

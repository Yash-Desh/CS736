#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <numa.h>
#include <numaif.h>

typedef enum {
    SCENARIO_ALL_LOCAL = 0,        // Case 1
    SCENARIO_SPLIT_THREADS = 1,    // Case 2
    SCENARIO_SPLIT_INTERLEAVE = 2, // Case 3
    SCENARIO_INTERLEAVE_LOCAL = 3  // Case 4 (NEW)
} scenario_t;

typedef struct {
    double   *array;
    size_t    start;
    size_t    length;
    int       compute_node;   // which NUMA node this thread runs on
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

    // Pin this thread to the chosen NUMA node.
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

    if (targ->thread_id == 0) {
        printf("Ignore sink: %f\n", sink);
    }

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
            "Usage: %s <array_size_MB> <num_threads> <scenario>\n"
            "  scenario 0: ALL_LOCAL\n"
            "      Memory: BIND to node 0 (all pages on node 0)\n"
            "      Threads: all on node 0\n"
            "\n"
            "  scenario 1: SPLIT_THREADS\n"
            "      Memory: BIND to node 0\n"
            "      Threads: half on node 0, half on node 1\n"
            "\n"
            "  scenario 2: SPLIT_INTERLEAVE\n"
            "      Memory: INTERLEAVE across node 0 and node 1\n"
            "      Threads: half on node 0, half on node 1\n"
            "\n"
            "  scenario 3: INTERLEAVE_LOCAL  (NEW)\n"
            "      Memory: INTERLEAVE across node 0 and node 1\n"
            "      Threads: all on node 0 (pure local+remote mix)\n"
            "\nExample:\n"
            "  %s 4096 8 0   # Case 1: all local\n"
            "  %s 4096 8 1   # Case 2: split threads, memory on node 0\n"
            "  %s 4096 8 2   # Case 3: split threads + interleaved memory\n"
            "  %s 4096 8 3   # Case 4: all threads on node 0 + interleaved memory\n",
            argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    long array_mb    = strtol(argv[1], NULL, 10);
    int  num_threads = atoi(argv[2]);
    int  scenario    = atoi(argv[3]);

    if (array_mb <= 0 || num_threads <= 0) {
        fprintf(stderr, "array_size_MB and num_threads must be positive.\n");
        return 1;
    }
    if (scenario < 0 || scenario > 3) {
        fprintf(stderr, "Invalid scenario (must be 0,1,2,3).\n");
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

    printf("=== Experiment 2: Thread placement vs Memory placement ===\n");
    printf("Array size : %ld MB\n", array_mb);
    printf("Threads    : %d\n", num_threads);
    printf("Scenario   : %d\n\n", scenario);

    size_t bytes  = (size_t)array_mb * 1024 * 1024;
    size_t nelems = bytes / sizeof(double);

    double *array = NULL;
    struct bitmask *mask = NULL;

    // ---------- Memory placement for each scenario ----------
    if (scenario == SCENARIO_ALL_LOCAL || scenario == SCENARIO_SPLIT_THREADS) {
        // Memory bound entirely to node 0.
        array = (double *)numa_alloc_onnode(bytes, 0);
        if (!array) {
            fprintf(stderr, "numa_alloc_onnode failed.\n");
            return 1;
        }
        printf("Memory placement: BIND to node 0 (all pages on node 0)\n");
    } else { // scenarios 2 and 3
        // Memory interleaved between node 0 and node 1.
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
        printf("Memory placement: INTERLEAVED across node 0 and node 1\n");
    }

    // Initialize array to fault in pages.
    for (size_t i = 0; i < nelems; i++) {
        array[i] = 1.0;
    }

    pthread_t    *threads = calloc(num_threads, sizeof(pthread_t));
    thread_arg_t *targs   = calloc(num_threads, sizeof(thread_arg_t));
    size_t chunk = nelems / num_threads;

    pthread_barrier_init(&start_barrier, NULL, num_threads);

    for (int t = 0; t < num_threads; t++) {
        targs[t].array  = array;
        targs[t].start  = (size_t)t * chunk;
        targs[t].length = chunk;
        targs[t].thread_id = t;

        // ---------- Thread placement for each scenario ----------
        int node_for_thread = 0;
        switch (scenario) {
            case SCENARIO_ALL_LOCAL:
                // All threads on node 0
                node_for_thread = 0;
                break;

            case SCENARIO_SPLIT_THREADS:
            case SCENARIO_SPLIT_INTERLEAVE:
                // First half of threads on node 0, second half on node 1.
                if (t < num_threads / 2)
                    node_for_thread = 0;
                else
                    node_for_thread = 1;
                break;

            case SCENARIO_INTERLEAVE_LOCAL:
                // NEW: all threads on node 0, but memory is interleaved 0+1
                node_for_thread = 0;
                break;
        }

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

    if ((scenario == SCENARIO_SPLIT_INTERLEAVE ||
         scenario == SCENARIO_INTERLEAVE_LOCAL) && mask)
        numa_free_nodemask(mask);

    numa_free(array, bytes);
    free(threads);
    free(targs);

    return 0;
}



// #define _GNU_SOURCE
// #include <stdio.h>
// #include <stdlib.h>
// #include <stdint.h>
// #include <errno.h>
// #include <string.h>
// #include <pthread.h>
// #include <time.h>
// #include <numa.h>
// #include <numaif.h>

// typedef enum {
//     SCENARIO_ALL_LOCAL = 0,      // Case 1
//     SCENARIO_SPLIT_THREADS = 1,  // Case 2
//     SCENARIO_SPLIT_INTERLEAVE = 2// Case 3
// } scenario_t;

// typedef struct {
//     double   *array;
//     size_t    start;
//     size_t    length;
//     int       compute_node;   // which NUMA node this thread runs on
//     int       thread_id;
//     double    elapsed_sec;
// } thread_arg_t;

// static pthread_barrier_t start_barrier;

// static double timespec_diff_sec(struct timespec a, struct timespec b) {
//     long sec  = b.tv_sec  - a.tv_sec;
//     long nsec = b.tv_nsec - a.tv_nsec;
//     if (nsec < 0) {
//         sec--;
//         nsec += 1000000000L;
//     }
//     return (double)sec + (double)nsec / 1e9;
// }

// void *worker(void *arg) {
//     thread_arg_t *targ = (thread_arg_t *)arg;

//     // Pin this thread's execution to the chosen NUMA node.
//     if (numa_run_on_node(targ->compute_node) != 0) {
//         fprintf(stderr, "Thread %d: numa_run_on_node(%d) failed: %s\n",
//                 targ->thread_id, targ->compute_node, strerror(errno));
//     }
//     numa_set_preferred(targ->compute_node);

//     pthread_barrier_wait(&start_barrier);

//     struct timespec t_start, t_end;
//     clock_gettime(CLOCK_MONOTONIC, &t_start);

//     volatile double sink = 0.0;
//     const size_t iters = 10;

//     for (size_t r = 0; r < iters; r++) {
//         double local_sum = 0.0;
//         for (size_t i = 0; i < targ->length; i++) {
//             local_sum += targ->array[targ->start + i];
//         }
//         sink += local_sum;
//     }

//     clock_gettime(CLOCK_MONOTONIC, &t_end);
//     targ->elapsed_sec = timespec_diff_sec(t_start, t_end);

//     if (targ->thread_id == 0) {
//         printf("Ignore sink: %f\n", sink);
//     }

//     return NULL;
// }

// int main(int argc, char **argv) {
//     if (argc != 4) {
//         fprintf(stderr,
//             "Usage: %s <array_size_MB> <num_threads> <scenario>\n"
//             "  scenario 0: ALL_LOCAL          (threads on node0, memory on node0)\n"
//             "  scenario 1: SPLIT_THREADS      (half threads node0, half node1, memory on node0)\n"
//             "  scenario 2: SPLIT_INTERLEAVE   (half threads node0, half node1, memory interleaved 0+1)\n"
//             "\nExample:\n"
//             "  %s 4096 8 0   # Case 1: all local\n"
//             "  %s 4096 8 1   # Case 2: split threads, memory on node 0\n"
//             "  %s 4096 8 2   # Case 3: split threads, memory interleaved\n",
//             argv[0], argv[0], argv[0], argv[0]);
//         return 1;
//     }

//     long array_mb    = strtol(argv[1], NULL, 10);
//     int  num_threads = atoi(argv[2]);
//     int  scenario    = atoi(argv[3]);

//     if (array_mb <= 0 || num_threads <= 0) {
//         fprintf(stderr, "array_size_MB and num_threads must be positive.\n");
//         return 1;
//     }
//     if (scenario < 0 || scenario > 2) {
//         fprintf(stderr, "Invalid scenario (must be 0,1,2).\n");
//         return 1;
//     }

//     if (numa_available() < 0) {
//         fprintf(stderr, "libnuma: NUMA not available on this system.\n");
//         return 1;
//     }

//     int max_node = numa_max_node();
//     if (max_node < 1) {
//         fprintf(stderr, "Need at least 2 NUMA nodes for this experiment.\n");
//         return 1;
//     }

//     printf("=== Experiment 2: Thread placement vs Memory placement ===\n");
//     printf("Array size : %ld MB\n", array_mb);
//     printf("Threads    : %d\n", num_threads);
//     printf("Scenario   : %d\n\n", scenario);

//     size_t bytes  = (size_t)array_mb * 1024 * 1024;
//     size_t nelems = bytes / sizeof(double);

//     double *array = NULL;
//     struct bitmask *mask = NULL;

//     // ---------- Memory placement for each scenario ----------
//     if (scenario == SCENARIO_ALL_LOCAL || scenario == SCENARIO_SPLIT_THREADS) {
//         // Memory bound entirely to node 0.
//         array = (double *)numa_alloc_onnode(bytes, 0);
//         if (!array) {
//             fprintf(stderr, "numa_alloc_onnode failed.\n");
//             return 1;
//         }
//         printf("Memory placement: BIND to node 0 (all pages on node 0)\n");
//     } else if (scenario == SCENARIO_SPLIT_INTERLEAVE) {
//         // Memory interleaved between node 0 and node 1.
//         mask = numa_allocate_nodemask();
//         numa_bitmask_clearall(mask);
//         numa_bitmask_setbit(mask, 0);
//         numa_bitmask_setbit(mask, 1);

//         array = (double *)numa_alloc_interleaved_subset(bytes, mask);
//         if (!array) {
//             fprintf(stderr, "numa_alloc_interleaved_subset failed.\n");
//             numa_free_nodemask(mask);
//             return 1;
//         }
//         printf("Memory placement: INTERLEAVED across node 0 and node 1\n");
//     }

//     // Initialize array just to fault in pages.
//     for (size_t i = 0; i < nelems; i++) {
//         array[i] = 1.0;
//     }

//     pthread_t    *threads = calloc(num_threads, sizeof(pthread_t));
//     thread_arg_t *targs   = calloc(num_threads, sizeof(thread_arg_t));
//     size_t chunk = nelems / num_threads;

//     pthread_barrier_init(&start_barrier, NULL, num_threads);

//     for (int t = 0; t < num_threads; t++) {
//         targs[t].array  = array;
//         targs[t].start  = (size_t)t * chunk;
//         targs[t].length = chunk;
//         targs[t].thread_id = t;

//         // ---------- Thread placement for each scenario ----------
//         int node_for_thread = 0;
//         switch (scenario) {
//             case SCENARIO_ALL_LOCAL:
//                 // All threads on node 0
//                 node_for_thread = 0;
//                 break;

//             case SCENARIO_SPLIT_THREADS:
//             case SCENARIO_SPLIT_INTERLEAVE:
//                 // First half of threads on node 0, second half on node 1.
//                 // If num_threads is odd, extra thread stays on node 0.
//                 if (t < num_threads / 2)
//                     node_for_thread = 0;
//                 else
//                     node_for_thread = 1;
//                 break;
//         }

//         targs[t].compute_node = node_for_thread;

//         printf("Thread %2d will run on NUMA node %d (start=%zu, len=%zu)\n",
//                t, node_for_thread, targs[t].start, targs[t].length);

//         int rc = pthread_create(&threads[t], NULL, worker, &targs[t]);
//         if (rc != 0) {
//             fprintf(stderr, "pthread_create(%d) failed: %s\n",
//                     t, strerror(rc));
//             return 1;
//         }
//     }

//     for (int t = 0; t < num_threads; t++) {
//         pthread_join(threads[t], NULL);
//     }

//     pthread_barrier_destroy(&start_barrier);

//     double max_time = 0.0;
//     double sum_time = 0.0;
//     for (int t = 0; t < num_threads; t++) {
//         printf("Thread %2d elapsed: %f sec (node %d)\n",
//                t, targs[t].elapsed_sec, targs[t].compute_node);
//         if (targs[t].elapsed_sec > max_time)
//             max_time = targs[t].elapsed_sec;
//         sum_time += targs[t].elapsed_sec;
//     }
//     printf("\nApprox. total time (max over threads): %f sec\n", max_time);
//     printf("Average per-thread time: %f sec\n", sum_time / num_threads);

//     if (scenario == SCENARIO_SPLIT_INTERLEAVE && mask)
//         numa_free_nodemask(mask);
//     numa_free(array, bytes);
//     free(threads);
//     free(targs);

//     return 0;
// }

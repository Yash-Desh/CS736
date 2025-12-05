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
    SCEN_BIND_NODE0     = 0, // shared table bound to node 0
    SCEN_INTERLEAVE_01  = 1, // shared table interleaved across 0+1
    SCEN_PREFERRED_NODE0= 2  // shared table with "preferred" node 0
} scenario_t;

typedef struct {
    double  *shared;          // pointer to shared read-mostly table
    size_t   shared_nelems;   // elements in shared table

    double  *priv;            // per-thread private buffer (local writes)
    size_t   priv_nelems;

    int      compute_node;    // NUMA node this thread runs on
    int      thread_id;
    double   elapsed_sec;
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

    // Pin to chosen NUMA node
    if (numa_run_on_node(targ->compute_node) != 0) {
        fprintf(stderr, "Thread %d: numa_run_on_node(%d) failed: %s\n",
                targ->thread_id, targ->compute_node, strerror(errno));
    }
    numa_set_preferred(targ->compute_node);

    // Wait so that all threads start compute phase together
    pthread_barrier_wait(&start_barrier);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    const size_t iters = 10;
    volatile double sink = 0.0;

    // Each thread:
    //  - repeatedly reads from the *shared* table (read-mostly)
    //  - accumulates into its own *private* local buffer
    //
    // Access pattern: for each element of priv, read one element from shared
    // using a stride so we walk over the whole shared table.
    size_t sN = targ->shared_nelems;
    size_t pN = targ->priv_nelems;

    for (size_t r = 0; r < iters; r++) {
        for (size_t i = 0; i < pN; i++) {
            size_t idx = (i * 31) % sN;      // pseudo-random-ish stride
            double val = targ->shared[idx]; // READ shared
            targ->priv[i] += val;           // WRITE private (local)
        }
    }

    // prevent compiler from dropping loop
    for (size_t i = 0; i < 8 && i < pN; i++)
        sink += targ->priv[i];

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    targ->elapsed_sec = timespec_diff_sec(t_start, t_end);

    if (targ->thread_id == 0)
        printf("Ignore sink: %f\n", sink);

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr,
            "Usage: %s <shared_MB> <private_MB_per_thread> <num_threads> <scenario>\n"
            "\n"
            "  scenario 0: BIND_NODE0\n"
            "      Shared table bound to NUMA node 0 (numa_alloc_onnode).\n"
            "      All threads read same table; threads split across nodes.\n"
            "\n"
            "  scenario 1: INTERLEAVE_01\n"
            "      Shared table interleaved across nodes 0 and 1.\n"
            "      Threads split across nodes.\n"
            "\n"
            "  scenario 2: PREFERRED_NODE0\n"
            "      Shared table allocated with preferred node 0 policy.\n"
            "      (numa_set_preferred(0) + numa_alloc).\n"
            "      Threads split across nodes.\n"
            "\nExample:\n"
            "  %s 2048 64 8 0   # shared=2GB, private=64MB, 8 threads, bind shared to node0\n",
            argv[0], argv[0]);
        return 1;
    }

    long shared_mb   = strtol(argv[1], NULL, 10);
    long priv_mb     = strtol(argv[2], NULL, 10);
    int  num_threads = atoi(argv[3]);
    int  scenario    = atoi(argv[4]);

    if (shared_mb <= 0 || priv_mb <= 0 || num_threads <= 0) {
        fprintf(stderr, "Sizes and num_threads must be positive.\n");
        return 1;
    }
    if (scenario < 0 || scenario > 2) {
        fprintf(stderr, "Invalid scenario (0,1,2).\n");
        return 1;
    }

    if (numa_available() < 0) {
        fprintf(stderr, "libnuma: NUMA not available.\n");
        return 1;
    }
    if (numa_max_node() < 1) {
        fprintf(stderr, "Need at least 2 NUMA nodes.\n");
        return 1;
    }

    printf("=== Experiment 4: Shared read-mostly data ===\n");
    printf("Shared table : %ld MB\n", shared_mb);
    printf("Private per thread: %ld MB\n", priv_mb);
    printf("Threads      : %d\n", num_threads);
    printf("Scenario     : %d\n\n", scenario);

    size_t shared_bytes = (size_t)shared_mb * 1024 * 1024;
    size_t priv_bytes   = (size_t)priv_mb   * 1024 * 1024;

    size_t shared_nelems = shared_bytes / sizeof(double);
    size_t priv_nelems   = priv_bytes   / sizeof(double);

    double *shared = NULL;
    struct bitmask *mask = NULL;

    // ----------- Allocate SHARED table according to scenario -----------
    if (scenario == SCEN_BIND_NODE0) {
        printf("Shared placement: BIND to node 0 (numa_alloc_onnode)\n");
        shared = (double *)numa_alloc_onnode(shared_bytes, 0);
        if (!shared) {
            fprintf(stderr, "numa_alloc_onnode for shared failed.\n");
            return 1;
        }
    } else if (scenario == SCEN_INTERLEAVE_01) {
        printf("Shared placement: INTERLEAVED across nodes 0 and 1\n");
        mask = numa_allocate_nodemask();
        numa_bitmask_clearall(mask);
        numa_bitmask_setbit(mask, 0);
        numa_bitmask_setbit(mask, 1);

        shared = (double *)numa_alloc_interleaved_subset(shared_bytes, mask);
        if (!shared) {
            fprintf(stderr, "numa_alloc_interleaved_subset for shared failed.\n");
            numa_free_nodemask(mask);
            return 1;
        }
    } else { // SCEN_PREFERRED_NODE0
        printf("Shared placement: PREFERRED node 0 (numa_set_preferred + numa_alloc)\n");
        numa_set_preferred(0);    // tell kernel to *prefer* node 0 for new pages
        shared = (double *)numa_alloc(shared_bytes);
        if (!shared) {
            fprintf(stderr, "numa_alloc for shared failed.\n");
            return 1;
        }
        // reset to localalloc for later allocations
        numa_set_localalloc();
    }

    // Initialize shared once (read-mostly, so only here we write to it)
    for (size_t i = 0; i < shared_nelems; i++) {
        shared[i] = (double)(i % 1024) * 0.5;
    }

    // ----------- Prepare threads and their PRIVATE buffers -----------
    pthread_t    *threads = calloc(num_threads, sizeof(pthread_t));
    thread_arg_t *targs   = calloc(num_threads, sizeof(thread_arg_t));

    pthread_barrier_init(&start_barrier, NULL, num_threads);

    for (int t = 0; t < num_threads; t++) {
        // split threads across nodes: first half on node 0, second half on node 1
        int node_for_thread = (t < num_threads / 2) ? 0 : 1;

        // private buffer for this thread, local to its node
        double *priv_buf = (double *)numa_alloc_onnode(priv_bytes, node_for_thread);
        if (!priv_buf) {
            fprintf(stderr, "numa_alloc_onnode for priv (thread %d) failed.\n", t);
            return 1;
        }
        for (size_t i = 0; i < priv_nelems; i++) {
            priv_buf[i] = 0.0;
        }

        targs[t].shared        = shared;
        targs[t].shared_nelems = shared_nelems;
        targs[t].priv          = priv_buf;
        targs[t].priv_nelems   = priv_nelems;
        targs[t].compute_node  = node_for_thread;
        targs[t].thread_id     = t;
        targs[t].elapsed_sec   = 0.0;

        printf("Thread %2d will run on node %d (priv buffer local to node %d)\n",
               t, node_for_thread, node_for_thread);

        int rc = pthread_create(&threads[t], NULL, worker, &targs[t]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create(%d) failed: %s\n", t, strerror(rc));
            return 1;
        }
    }

    // ----------- Collect results -----------
    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    pthread_barrier_destroy(&start_barrier);

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

    // ----------- Cleanup -----------
    for (int t = 0; t < num_threads; t++) {
        numa_free(targs[t].priv, priv_bytes);
    }

    if (scenario == SCEN_INTERLEAVE_01 && mask)
        numa_free_nodemask(mask);

    if (scenario == SCEN_BIND_NODE0)
        numa_free(shared, shared_bytes);
    else if (scenario == SCEN_INTERLEAVE_01 || scenario == SCEN_PREFERRED_NODE0)
        numa_free(shared, shared_bytes); // numa_alloc used in both

    free(threads);
    free(targs);

    return 0;
}

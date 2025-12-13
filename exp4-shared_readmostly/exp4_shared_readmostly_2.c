#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <numa.h>
#include <numaif.h>

typedef enum {
    SCEN_BIND_NODE0        = 0, // shared table bound to node 0
    SCEN_INTERLEAVE_01     = 1, // shared table interleaved across 0+1 (50/50)
    SCEN_PREFERRED_NODE0   = 2, // shared table with "preferred" node 0
    SCEN_WEIGHTED_25_75    = 3  // shared table weighted 25/75 across two allowed nodes
} scenario_t;

typedef struct {
    double  *shared;
    size_t   shared_nelems;

    double  *priv;
    size_t   priv_nelems;

    int      compute_node;
    int      thread_id;
    double   elapsed_sec;
} thread_arg_t;

static pthread_barrier_t start_barrier;

static double timespec_diff_sec(struct timespec a, struct timespec b) {
    long sec  = b.tv_sec  - a.tv_sec;
    long nsec = b.tv_nsec - a.tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    return (double)sec + (double)nsec / 1e9;
}

static void print_allowed_mems(void) {
    struct bitmask *allowed = numa_get_mems_allowed();
    if (!allowed) return;
    printf("Mems allowed (by cpuset/cgroup): ");
    int first = 1;
    for (int n = 0; n <= numa_max_node(); n++) {
        if (numa_bitmask_isbitset(allowed, n)) {
            if (!first) printf(",");
            printf("%d", n);
            first = 0;
        }
    }
    printf("\n");
}

static int pick_two_allowed_mems(int *a, int *b) {
    struct bitmask *allowed = numa_get_mems_allowed();
    if (!allowed) return -1;

    int first = -1, second = -1;
    for (int n = 0; n <= numa_max_node(); n++) {
        if (numa_bitmask_isbitset(allowed, n)) {
            if (first < 0) first = n;
            else { second = n; break; }
        }
    }
    if (first < 0 || second < 0) return -1;
    *a = first; *b = second;
    return 0;
}

// Bind pages in a repeating pattern: 1 page -> nodeA, 3 pages -> nodeB (≈25/75)
static int bind_weighted_25_75_pages(void *addr, size_t len, int nodeA, int nodeB) {
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;

    struct bitmask *mA = numa_allocate_nodemask();
    struct bitmask *mB = numa_allocate_nodemask();
    if (!mA || !mB) return -1;

    numa_bitmask_clearall(mA);
    numa_bitmask_clearall(mB);
    numa_bitmask_setbit(mA, nodeA);
    numa_bitmask_setbit(mB, nodeB);

    // IMPORTANT: mbind expects "maxnode" as number-of-bits in the nodemask
    unsigned long maxnode_bits = mA->size;

    char *p = (char *)addr;
    size_t off = 0;

    while (off < len) {
        // 1 page on nodeA
        size_t cA = (size_t)pagesz;
        if (off + cA > len) cA = len - off;
        if (mbind(p + off, cA, MPOL_BIND, mA->maskp, maxnode_bits, 0) != 0) {
            fprintf(stderr, "mbind(node%d) failed at off=%zu: %s\n", nodeA, off, strerror(errno));
            goto fail;
        }
        off += cA;
        if (off >= len) break;

        // 3 pages on nodeB
        size_t cB = (size_t)pagesz * 3;
        if (off + cB > len) cB = len - off;
        if (mbind(p + off, cB, MPOL_BIND, mB->maskp, maxnode_bits, 0) != 0) {
            fprintf(stderr, "mbind(node%d) failed at off=%zu: %s\n", nodeB, off, strerror(errno));
            goto fail;
        }
        off += cB;
    }

    numa_free_nodemask(mA);
    numa_free_nodemask(mB);
    return 0;

fail:
    numa_free_nodemask(mA);
    numa_free_nodemask(mB);
    return -1;
}

void *worker(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;

    if (numa_run_on_node(targ->compute_node) != 0) {
        fprintf(stderr, "Thread %d: numa_run_on_node(%d) failed: %s\n",
                targ->thread_id, targ->compute_node, strerror(errno));
    }
    numa_set_preferred(targ->compute_node);

    pthread_barrier_wait(&start_barrier);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    const size_t iters = 10;
    volatile double sink = 0.0;

    size_t sN = targ->shared_nelems;
    size_t pN = targ->priv_nelems;

    for (size_t r = 0; r < iters; r++) {
        for (size_t i = 0; i < pN; i++) {
            size_t idx = (i * 31) % sN;
            double val = targ->shared[idx];
            targ->priv[i] += val;
        }
    }

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
            "  scenario 1: INTERLEAVE_01 (50/50 across nodes 0,1)\n"
            "  scenario 2: PREFERRED_NODE0\n"
            "  scenario 3: WEIGHTED_25_75 across two allowed nodes (1 page : 3 pages)\n",
            argv[0]);
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
    if (scenario < 0 || scenario > 3) {
        fprintf(stderr, "Invalid scenario (0..3).\n");
        return 1;
    }

    if (numa_available() < 0) {
        fprintf(stderr, "libnuma: NUMA not available.\n");
        return 1;
    }

    printf("=== Experiment 4: Shared read-mostly data ===\n");
    printf("Shared table : %ld MB\n", shared_mb);
    printf("Private per thread: %ld MB\n", priv_mb);
    printf("Threads      : %d\n", num_threads);
    printf("Scenario     : %d\n", scenario);
    print_allowed_mems();
    printf("\n");

    size_t shared_bytes = (size_t)shared_mb * 1024 * 1024;
    size_t priv_bytes   = (size_t)priv_mb   * 1024 * 1024;

    size_t shared_nelems = shared_bytes / sizeof(double);
    size_t priv_nelems   = priv_bytes   / sizeof(double);

    double *shared = NULL;
    struct bitmask *mask = NULL;
    int shared_is_mmap = 0;

    // For scenarios that assume two nodes, we’ll still choose nodes from allowed set
    int nodeA = 0, nodeB = 1;
    int have_two_allowed = (pick_two_allowed_mems(&nodeA, &nodeB) == 0);

    if ((scenario == SCEN_WEIGHTED_25_75) && !have_two_allowed) {
        fprintf(stderr, "Scenario 3 needs at least 2 allowed memory nodes.\n");
        fprintf(stderr, "Try running with: numactl --cpunodebind=0,1 --membind=0,1 ...\n");
        return 1;
    }

    // ----------- Allocate SHARED table according to scenario -----------
    if (scenario == SCEN_BIND_NODE0) {
        printf("Shared placement: BIND to node 0 (numa_alloc_onnode)\n");
        shared = (double *)numa_alloc_onnode(shared_bytes, 0);
        if (!shared) { fprintf(stderr, "numa_alloc_onnode(shared) failed.\n"); return 1; }

    } else if (scenario == SCEN_INTERLEAVE_01) {
        printf("Shared placement: INTERLEAVED across nodes 0 and 1 (50/50)\n");
        mask = numa_allocate_nodemask();
        numa_bitmask_clearall(mask);
        numa_bitmask_setbit(mask, 0);
        numa_bitmask_setbit(mask, 1);

        shared = (double *)numa_alloc_interleaved_subset(shared_bytes, mask);
        if (!shared) {
            fprintf(stderr, "numa_alloc_interleaved_subset(shared) failed.\n");
            numa_free_nodemask(mask);
            return 1;
        }

    } else if (scenario == SCEN_PREFERRED_NODE0) {
        printf("Shared placement: PREFERRED node 0 (numa_set_preferred + numa_alloc)\n");
        numa_set_preferred(0);
        shared = (double *)numa_alloc(shared_bytes);
        if (!shared) { fprintf(stderr, "numa_alloc(shared) failed.\n"); return 1; }
        numa_set_localalloc();

    } else { // SCEN_WEIGHTED_25_75
        printf("Shared placement: WEIGHTED 25/75 across allowed nodes %d/%d (pattern 1:3)\n",
               nodeA, nodeB);

        void *p = mmap(NULL, shared_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "mmap(shared) failed: %s\n", strerror(errno));
            return 1;
        }
        shared_is_mmap = 1;
        shared = (double *)p;

        if (bind_weighted_25_75_pages(shared, shared_bytes, nodeA, nodeB) != 0) {
            fprintf(stderr, "Weighted mbind setup failed.\n");
            munmap(shared, shared_bytes);
            return 1;
        }
    }

    // Initialize shared once (first-touch allocates according to the set policy)
    for (size_t i = 0; i < shared_nelems; i++) {
        shared[i] = (double)(i % 1024) * 0.5;
    }

    // ----------- Prepare threads and their PRIVATE buffers -----------
    pthread_t    *threads = calloc(num_threads, sizeof(pthread_t));
    thread_arg_t *targs   = calloc(num_threads, sizeof(thread_arg_t));
    if (!threads || !targs) {
        fprintf(stderr, "calloc failed.\n");
        return 1;
    }

    pthread_barrier_init(&start_barrier, NULL, num_threads);

    // If we have two allowed nodes, split threads across them; otherwise everything on node 0.
    int n0 = have_two_allowed ? nodeA : 0;
    int n1 = have_two_allowed ? nodeB : 0;

    for (int t = 0; t < num_threads; t++) {
        int node_for_thread = (t < num_threads / 2) ? n0 : n1;

        double *priv_buf = (double *)numa_alloc_onnode(priv_bytes, node_for_thread);
        if (!priv_buf) {
            fprintf(stderr, "numa_alloc_onnode(priv) failed for thread %d.\n", t);
            return 1;
        }
        for (size_t i = 0; i < priv_nelems; i++) priv_buf[i] = 0.0;

        targs[t].shared        = shared;
        targs[t].shared_nelems = shared_nelems;
        targs[t].priv          = priv_buf;
        targs[t].priv_nelems   = priv_nelems;
        targs[t].compute_node  = node_for_thread;
        targs[t].thread_id     = t;
        targs[t].elapsed_sec   = 0.0;

        printf("Thread %2d will run on node %d (priv local to node %d)\n",
               t, node_for_thread, node_for_thread);

        int rc = pthread_create(&threads[t], NULL, worker, &targs[t]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create(%d) failed: %s\n", t, strerror(rc));
            return 1;
        }
    }

    // ----------- Collect results -----------
    for (int t = 0; t < num_threads; t++) pthread_join(threads[t], NULL);

    pthread_barrier_destroy(&start_barrier);

    double max_time = 0.0, sum_time = 0.0;
    for (int t = 0; t < num_threads; t++) {
        printf("Thread %2d elapsed: %f sec (node %d)\n",
               t, targs[t].elapsed_sec, targs[t].compute_node);
        if (targs[t].elapsed_sec > max_time) max_time = targs[t].elapsed_sec;
        sum_time += targs[t].elapsed_sec;
    }
    printf("\nApprox. total time (max over threads): %f sec\n", max_time);
    printf("Average per-thread time: %f sec\n", sum_time / num_threads);

    // ----------- Cleanup -----------
    for (int t = 0; t < num_threads; t++) {
        numa_free(targs[t].priv, priv_bytes);
    }

    if (mask) numa_free_nodemask(mask);

    if (shared_is_mmap) munmap(shared, shared_bytes);
    else numa_free(shared, shared_bytes);

    free(threads);
    free(targs);
    return 0;
}

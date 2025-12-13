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
    SCEN_BIND_NODE0        = 0,
    SCEN_INTERLEAVE_01     = 1,
    SCEN_PREFERRED_NODE0   = 2,
    SCEN_WEIGHTED_RATIO    = 3
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

// Pick the first two allowed memory nodes (works under cpuset restrictions)
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

// Parse ratio "A:B" into (a,b)
static int parse_ratio(const char *s, int *a, int *b) {
    if (!s || !*s) return -1;
    char *dup = strdup(s);
    if (!dup) return -1;

    char *colon = strchr(dup, ':');
    if (!colon) { free(dup); return -1; }
    *colon = '\0';

    char *end1 = NULL, *end2 = NULL;
    long aa = strtol(dup, &end1, 10);
    long bb = strtol(colon + 1, &end2, 10);

    int ok = (end1 && *end1 == '\0' && end2 && *end2 == '\0' && aa > 0 && bb > 0);
    free(dup);
    if (!ok) return -1;

    *a = (int)aa;
    *b = (int)bb;
    return 0;
}

// Weighted page binding with ratio A:B using chunked page pattern.
// Example A=1,B=3 => bind 1 page to nodeA, then 3 pages to nodeB, repeat.
static int bind_weighted_pages(void *addr, size_t len, int nodeA, int nodeB, int ratioA, int ratioB) {
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;

    struct bitmask *mA = numa_allocate_nodemask();
    struct bitmask *mB = numa_allocate_nodemask();
    if (!mA || !mB) return -1;

    numa_bitmask_clearall(mA);
    numa_bitmask_clearall(mB);
    numa_bitmask_setbit(mA, nodeA);
    numa_bitmask_setbit(mB, nodeB);

    unsigned long maxnode_bits = mA->size; // mbind expects number-of-bits in nodemask

    char *p = (char *)addr;
    size_t off = 0;

    while (off < len) {
        // ratioA pages on nodeA
        size_t bytesA = (size_t)pagesz * (size_t)ratioA;
        if (off + bytesA > len) bytesA = len - off;
        if (bytesA > 0) {
            if (mbind(p + off, bytesA, MPOL_BIND, mA->maskp, maxnode_bits, 0) != 0) {
                fprintf(stderr, "mbind(node%d) failed at off=%zu: %s\n", nodeA, off, strerror(errno));
                goto fail;
            }
            off += bytesA;
            if (off >= len) break;
        }

        // ratioB pages on nodeB
        size_t bytesB = (size_t)pagesz * (size_t)ratioB;
        if (off + bytesB > len) bytesB = len - off;
        if (bytesB > 0) {
            if (mbind(p + off, bytesB, MPOL_BIND, mB->maskp, maxnode_bits, 0) != 0) {
                fprintf(stderr, "mbind(node%d) failed at off=%zu: %s\n", nodeB, off, strerror(errno));
                goto fail;
            }
            off += bytesB;
        }
    }

    numa_free_nodemask(mA);
    numa_free_nodemask(mB);
    return 0;

fail:
    numa_free_nodemask(mA);
    numa_free_nodemask(mB);
    return -1;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [shared_MB] [private_MB_per_thread] [num_threads] [scenario] [ratio_A:ratio_B]\n"
        "\n"
        "All args are optional. Defaults:\n"
        "  shared_MB = 2048\n"
        "  private_MB_per_thread = 64\n"
        "  num_threads = 8\n"
        "  scenario = 3\n"
        "  ratio = 1:3   (only used for scenario 3)\n"
        "\n"
        "Scenarios:\n"
        "  0: Shared BIND to node 0\n"
        "  1: Shared INTERLEAVE across nodes 0 and 1 (50/50)\n"
        "  2: Shared PREFERRED node 0\n"
        "  3: Shared WEIGHTED across first two *allowed* nodes using ratio A:B\n"
        "\n"
        "Examples:\n"
        "  %s                          # defaults (weighted 1:3)\n"
        "  %s 1024 32 8 3 2:6           # weighted 2:6 (~25/75)\n"
        "  %s 4096 64 16 3 3:1          # weighted 3:1 (~75/25)\n"
        "  %s 2048 64 8 1               # scenario 1 (ignore ratio)\n",
        prog, prog, prog, prog, prog);
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
    // ---- Defaults if user doesn't provide args ----
    long shared_mb   = 2048;
    long priv_mb     = 64;
    int  num_threads = 8;
    int  scenario    = 3;
    int  ratioA      = 1;
    int  ratioB      = 3;

    if (argc > 1) shared_mb = strtol(argv[1], NULL, 10);
    if (argc > 2) priv_mb   = strtol(argv[2], NULL, 10);
    if (argc > 3) num_threads = atoi(argv[3]);
    if (argc > 4) scenario  = atoi(argv[4]);
    if (argc > 5) {
        if (parse_ratio(argv[5], &ratioA, &ratioB) != 0) {
            fprintf(stderr, "Invalid ratio '%s' (expected A:B, both > 0)\n", argv[5]);
            usage(argv[0]);
            return 1;
        }
    }
    if (argc > 6) {
        fprintf(stderr, "Too many arguments.\n");
        usage(argv[0]);
        return 1;
    }

    if (shared_mb <= 0 || priv_mb <= 0 || num_threads <= 0) {
        fprintf(stderr, "shared_MB, private_MB_per_thread, and num_threads must be positive.\n");
        return 1;
    }
    if (scenario < 0 || scenario > 3) {
        fprintf(stderr, "Invalid scenario (0..3).\n");
        usage(argv[0]);
        return 1;
    }
    if (ratioA <= 0 || ratioB <= 0) {
        fprintf(stderr, "Ratio parts must be positive.\n");
        return 1;
    }

    if (numa_available() < 0) {
        fprintf(stderr, "libnuma: NUMA not available.\n");
        return 1;
    }

    printf("=== Experiment 4: Shared read-mostly data ===\n");
    printf("Shared table         : %ld MB\n", shared_mb);
    printf("Private per thread   : %ld MB\n", priv_mb);
    printf("Threads              : %d\n", num_threads);
    printf("Scenario             : %d\n", scenario);
    if (scenario == SCEN_WEIGHTED_RATIO)
        printf("Weighted ratio (A:B) : %d:%d\n", ratioA, ratioB);
    print_allowed_mems();
    printf("\n");

    size_t shared_bytes = (size_t)shared_mb * 1024 * 1024;
    size_t priv_bytes   = (size_t)priv_mb   * 1024 * 1024;

    size_t shared_nelems = shared_bytes / sizeof(double);
    size_t priv_nelems   = priv_bytes   / sizeof(double);

    double *shared = NULL;
    struct bitmask *mask = NULL;
    int shared_is_mmap = 0;

    int nodeA = 0, nodeB = 1;
    int have_two_allowed = (pick_two_allowed_mems(&nodeA, &nodeB) == 0);

    if (scenario == SCEN_WEIGHTED_RATIO && !have_two_allowed) {
        fprintf(stderr, "Scenario 3 needs at least 2 allowed memory nodes.\n");
        fprintf(stderr, "Try: numactl --cpunodebind=0,1 --membind=0,1 %s ...\n", argv[0]);
        return 1;
    }

    // ----------- Allocate SHARED table according to scenario -----------
    if (scenario == SCEN_BIND_NODE0) {
        printf("Shared placement: BIND to node 0\n");
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
        printf("Shared placement: PREFERRED node 0\n");
        numa_set_preferred(0);
        shared = (double *)numa_alloc(shared_bytes);
        if (!shared) { fprintf(stderr, "numa_alloc(shared) failed.\n"); return 1; }
        numa_set_localalloc();

    } else { // SCEN_WEIGHTED_RATIO
        printf("Shared placement: WEIGHTED across allowed nodes %d/%d using ratio %d:%d\n",
               nodeA, nodeB, ratioA, ratioB);

        void *p = mmap(NULL, shared_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "mmap(shared) failed: %s\n", strerror(errno));
            return 1;
        }
        shared_is_mmap = 1;
        shared = (double *)p;

        if (bind_weighted_pages(shared, shared_bytes, nodeA, nodeB, ratioA, ratioB) != 0) {
            fprintf(stderr, "Weighted mbind setup failed.\n");
            munmap(shared, shared_bytes);
            return 1;
        }
    }

    // First-touch initialization (allocates pages under policy)
    for (size_t i = 0; i < shared_nelems; i++)
        shared[i] = (double)(i % 1024) * 0.5;

    // ----------- Prepare threads and PRIVATE buffers -----------
    pthread_t    *threads = calloc(num_threads, sizeof(pthread_t));
    thread_arg_t *targs   = calloc(num_threads, sizeof(thread_arg_t));
    if (!threads || !targs) {
        fprintf(stderr, "calloc failed.\n");
        return 1;
    }

    pthread_barrier_init(&start_barrier, NULL, num_threads);

    // Split threads across two allowed nodes if possible, else all on node0
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <numa.h>
#include <numaif.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
            "Usage: %s <node> <size_MB>\n"
            "Example: %s 0 60000   # allocate ~60GB on node 0\n",
            argv[0], argv[0]);
        return 1;
    }

    int node = atoi(argv[1]);
    long mb  = strtol(argv[2], NULL, 10);

    if (mb <= 0) {
        fprintf(stderr, "size_MB must be positive.\n");
        return 1;
    }

    if (numa_available() < 0) {
        fprintf(stderr, "NUMA not available.\n");
        return 1;
    }

    int max_node = numa_max_node();
    if (node < 0 || node > max_node) {
        fprintf(stderr, "Invalid node (0..%d).\n", max_node);
        return 1;
    }

    size_t bytes = (size_t)mb * 1024 * 1024;
    printf("Pressure: allocating %ld MB on node %d\n", mb, node);

    // Allocate on the chosen node
    char *buf = (char *)numa_alloc_onnode(bytes, node);
    if (!buf) {
        fprintf(stderr, "numa_alloc_onnode failed: %s\n", strerror(errno));
        return 1;
    }

    // Touch all pages so they are actually backed by physical memory
    for (size_t i = 0; i < bytes; i += 4096) {
        buf[i] = (char)(i & 0xff);
    }

    printf("Pressure process PID = %d. Holding memory... (Ctrl+C to stop)\n", getpid());

    // Keep process alive so the memory stays allocated
    while (1) {
        // Very occasional touches so kernel doesn’t think it’s cold
        for (size_t i = 0; i < bytes; i += 4096 * 1024) {
            buf[i] ^= 1;
        }
        sleep(1);
    }

    // (never reached)
    numa_free(buf, bytes);
    return 0;
}

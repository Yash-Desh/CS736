// Steady-state IPC benchmark for a UNIX pipe: latency or throughput.
// Build: gcc -O2 -g -fno-omit-frame-pointer -std=c11 ipc_bench.c -o ipc_bench
// Usage examples are below the code.
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);   // steady, not NTP-adjusted
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void pin_to_cpu(int cpu) {
    if (cpu < 0) return; // negative = don't pin
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        exit(1);
    }
}

static int write_full(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t n = 0;
    while (n < len) {
        ssize_t w = write(fd, p + n, len - n);
        if (w < 0) { if (errno == EINTR) continue; perror("write"); return -1; }
        n += (size_t)w;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t n = 0;
    while (n < len) {
        ssize_t r = read(fd, p + n, len - n);
        if (r < 0) { if (errno == EINTR) continue; perror("read"); return -1; }
        if (r == 0) { fprintf(stderr, "EOF on pipe\n"); return -1; }
        n += (size_t)r;
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
      "Usage:\n"
      "  %s -m {lat|thr} -s <msg_bytes> [options]\n"
      "Options:\n"
      "  -w <warmup_iters>      (lat: unmeasured ping-pongs; thr: unmeasured chunks) [default 2000]\n"
      "  -n <iters>             (latency mode) measured round trips [default 50000]\n"
      "  -T <MiB>               (throughput mode) total MiB to send [default 256]\n"
      "  -p <parent_cpu>        pin parent to CPU id (negative = no pin) [default 0]\n"
      "  -c <child_cpu>         pin child to CPU id (negative = no pin) [default 1]\n"
      "  -P <pipe_size_bytes>   try to set pipe capacity (both dirs) [default 0 = leave]\n"
      , prog);
}

int main(int argc, char **argv) {
    // Defaults
    enum { LAT, THR } mode = LAT;
    size_t msg = 64;
    int warmup = 2000;
    int iters = 50000;      // latency
    double total_mib = 256; // throughput
    int parent_cpu = 0, child_cpu = 1;
    long pipe_sz = 0;

    // Parse args
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-m") && i+1 < argc) {
            if (!strcmp(argv[i+1], "lat")) mode = LAT;
            else if (!strcmp(argv[i+1], "thr")) mode = THR;
            else { usage(argv[0]); return 1; }
            ++i;
        } else if (!strcmp(argv[i], "-s") && i+1 < argc) {
            msg = (size_t)strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-w") && i+1 < argc) {
            warmup = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-n") && i+1 < argc) {
            iters = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-T") && i+1 < argc) {
            total_mib = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-p") && i+1 < argc) {
            parent_cpu = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-c") && i+1 < argc) {
            child_cpu = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-P") && i+1 < argc) {
            pipe_sz = strtol(argv[++i], NULL, 0);
        } else {
            usage(argv[0]); return 1;
        }
    }
    if (msg == 0) { fprintf(stderr, "msg size must be > 0\n"); return 1; }

    // Two pipes: A->B data, B->A reply/ack
    int p_ab[2], p_ba[2];
    if (pipe(p_ab) || pipe(p_ba)) { perror("pipe"); return 1; }
    if (pipe_sz > 0) {
        (void)fcntl(p_ab[1], F_SETPIPE_SZ, pipe_sz);
        (void)fcntl(p_ba[1], F_SETPIPE_SZ, pipe_sz);
    }

    // Allocate aligned buffers
    void *snd, *rcv;
    if (posix_memalign(&snd, 64, msg) || posix_memalign(&rcv, 64, msg)) {
        perror("posix_memalign"); return 1;
    }
    memset(snd, 0xA5, msg);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        // ---- Child ----
        pin_to_cpu(child_cpu);

        if (mode == LAT) {
            // Child echoes back exactly msg bytes each time
            close(p_ab[1]); // child reads from A->B
            close(p_ba[0]); // child writes to B->A
            for (int i = 0; i < warmup + iters; ++i) {
                if (read_full(p_ab[0], rcv, msg) != 0) _exit(2);
                if (write_full(p_ba[1], rcv, msg) != 0) _exit(3);
            }
            close(p_ab[0]); close(p_ba[1]);
            _exit(0);
        } else {
            // Throughput: child reads total bytes then sends 1-byte ACK
            close(p_ab[1]); // read from A->B
            close(p_ba[0]); // write ACK on B->A
            size_t total_bytes = (size_t)(total_mib * 1024.0 * 1024.0);
            // warm-up reads
            for (int i = 0; i < warmup; ++i) {
                if (read_full(p_ab[0], rcv, msg) != 0) _exit(4);
            }
            size_t left = total_bytes;
            while (left) {
                size_t chunk = left < msg ? left : msg;
                if (read_full(p_ab[0], rcv, chunk) != 0) _exit(5);
                left -= chunk;
            }
            char ack = 0;
            if (write_full(p_ba[1], &ack, 1) != 0) _exit(6);
            close(p_ab[0]); close(p_ba[1]);
            _exit(0);
        }
    }

    // ---- Parent ----
    pin_to_cpu(parent_cpu);

    if (mode == LAT) {
        // Parent initiates ping-pong
        close(p_ab[0]); // parent writes on A->B
        close(p_ba[1]); // parent reads on B->A

        // warm-up
        for (int i = 0; i < warmup; ++i) {
            if (write_full(p_ab[1], snd, msg) != 0) return 1;
            if (read_full (p_ba[0], rcv, msg) != 0) return 1;
        }

        // measured loop
        uint64_t t0 = ns_now();
        for (int i = 0; i < iters; ++i) {
            if (write_full(p_ab[1], snd, msg) != 0) return 1;
            if (read_full (p_ba[0], rcv, msg) != 0) return 1;
        }
        uint64_t t1 = ns_now();

        close(p_ab[1]); close(p_ba[0]);
        int status=0; waitpid(pid, &status, 0);

        double rt_avg_ns = (t1 - t0) / (double)iters;
        double ow_avg_ns = rt_avg_ns / 2.0;
        printf("MODE  : latency (ping-pong)\n");
        printf("MSG   : %zu bytes   warmup=%d  iters=%d\n", msg, warmup, iters);
        printf("CPUs  : parent=%d child=%d  pipe_sz=%ld\n", parent_cpu, child_cpu, pipe_sz);
        printf("RTavg : %.3f ns   ONE-WAY avg: %.3f ns\n", rt_avg_ns, ow_avg_ns);
    } else {
        // Throughput: stream total bytes, single ACK back
        close(p_ab[0]); // parent writes A->B
        close(p_ba[1]); // parent reads ACK on B->A

        // warm-up writes
        for (int i = 0; i < warmup; ++i) {
            if (write_full(p_ab[1], snd, msg) != 0) return 1;
        }
        // drain warmup from child before timing to avoid intermix
        // (child consumed warm-up already before measured phase)

        size_t total_bytes = (size_t)(total_mib * 1024.0 * 1024.0);

        uint64_t t0 = ns_now();
        size_t left = total_bytes;
        while (left) {
            size_t chunk = left < msg ? left : msg;
            if (write_full(p_ab[1], snd, chunk) != 0) return 1;
            left -= chunk;
        }
        char ack;
        if (read_full(p_ba[0], &ack, 1) != 0) return 1;
        uint64_t t1 = ns_now();

        close(p_ab[1]); close(p_ba[0]);
        int status=0; waitpid(pid, &status, 0);

        double secs = (t1 - t0) / 1e9;
        double mib  = (double)total_bytes / (1024.0 * 1024.0);
        double mibps = mib / secs;
        double gibps = mibps / 1024.0;
        printf("MODE  : throughput (one-way stream + final ACK)\n");
        printf("MSG   : %zu bytes   warmup=%d  total=%.1f MiB\n", msg, warmup, total_mib);
        printf("CPUs  : parent=%d child=%d  pipe_sz=%ld\n", parent_cpu, child_cpu, pipe_sz);
        printf("TIME  : %.6f s   THROUGHPUT: %.2f MiB/s  (%.3f GiB/s)\n",
               secs, mibps, gibps);
    }

    free(snd); free(rcv);
    return 0;
}

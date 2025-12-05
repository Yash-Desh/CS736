#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cpuid.h>
#include <x86intrin.h>   // __rdtsc, __rdtscp, _mm_lfence

// --- RDTSC helpers (serialize to avoid reordering) ---
static inline void cpuid_serialize(void){ unsigned a,b,c,d; __get_cpuid(0,&a,&b,&c,&d); }
static inline uint64_t rdtsc_begin(void){ cpuid_serialize(); return __rdtsc(); }
static inline uint64_t rdtsc_end(void){ unsigned aux; uint64_t t=__rdtscp(&aux); cpuid_serialize(); return t; }

// --- Pin to a CPU ---
static void pin_to_cpu(int cpu){
  cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu,&set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0) { perror("sched_setaffinity"); exit(1); }
}

// --- Calibrate TSC Hz against CLOCK_MONOTONIC_RAW (~300 ms) ---
static double tsc_hz_calibrate(void){
  struct timespec a,b,req = { .tv_sec=0, .tv_nsec=300000000L };
  clock_gettime(CLOCK_MONOTONIC_RAW, &a);
  uint64_t c0 = rdtsc_begin(); nanosleep(&req, NULL); uint64_t c1 = rdtsc_end();
  clock_gettime(CLOCK_MONOTONIC_RAW, &b);
  double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec)/1e9;
  return (c1 - c0) / dt;
}

// Robust I/O (handle partial transfers)
static int write_full(int fd, const void *buf, size_t len){
  const uint8_t *p = (const uint8_t*)buf; size_t n=0;
  while (n < len) {
    ssize_t w = write(fd, p+n, len-n);
    if (w < 0) { if (errno==EINTR) continue; perror("write"); return -1; }
    n += (size_t)w;
  }
  return 0;
}
static int read_full(int fd, void *buf, size_t len){
  uint8_t *p = (uint8_t*)buf; size_t n=0;
  while (n < len) {
    ssize_t r = read(fd, p+n, len-n);
    if (r < 0) { if (errno==EINTR) continue; perror("read"); return -1; }
    if (r == 0) { fprintf(stderr, "EOF on pipe\n"); return -1; }
    n += (size_t)r;
  }
  return 0;
}

// Run a single message size; returns 0 on success
static int run_size(size_t msg, size_t total_bytes, int parent_cpu, int child_cpu, long pipe_sz, double tsc_hz){
  int p_ab[2], p_ba[2];
  if (pipe(p_ab) || pipe(p_ba)) { perror("pipe"); return -1; }

  // Try to ensure pipe buffers are at least msg (or user-specified pipe_sz)
  long want = (long)msg;
  if (pipe_sz > want) want = pipe_sz;          // user override
  if (want > 0) { (void)fcntl(p_ab[1], F_SETPIPE_SZ, want); (void)fcntl(p_ba[1], F_SETPIPE_SZ, want); }

  // Buffers
  void *snd_buf;
  if (posix_memalign(&snd_buf, 64, msg)) { perror("posix_memalign"); return -1; }
  memset(snd_buf, 0xA5, msg);
  char ack = 0;

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return -1; }

  if (pid == 0) {
    // --- Child: read total_bytes, then send single 1-byte ACK ---
    pin_to_cpu(child_cpu);
    close(p_ab[1]); close(p_ba[0]);
    size_t remaining = total_bytes;
    uint8_t *tmp = malloc(msg);
    if (!tmp) { perror("malloc"); _exit(2); }

    while (remaining) {
      size_t chunk = remaining < msg ? remaining : msg;
      if (read_full(p_ab[0], tmp, chunk) != 0) { _exit(3); }
      remaining -= chunk;
    }
    free(tmp);
    if (write_full(p_ba[1], &ack, 1) != 0) { _exit(4); }
    close(p_ab[0]); close(p_ba[1]); _exit(0);
  }

  // --- Parent: send total_bytes, then wait for ACK; measure whole transfer ---
  pin_to_cpu(parent_cpu);
  close(p_ab[0]); close(p_ba[1]);

  uint64_t t0 = rdtsc_begin();
  size_t remaining = total_bytes;
  while (remaining) {
    size_t chunk = remaining < msg ? remaining : msg;
    if (write_full(p_ab[1], snd_buf, chunk) != 0) return -1;
    remaining -= chunk;
  }
  if (read_full(p_ba[0], &ack, 1) != 0) return -1;   // negligible vs total
  uint64_t t1 = rdtsc_end();

  close(p_ab[1]); close(p_ba[0]);
  int status=0; waitpid(pid, &status, 0);

  double secs = (t1 - t0) / tsc_hz;
  double mib  = (double)total_bytes / (1024.0*1024.0);
  double gib  = (double)total_bytes / (1024.0*1024.0*1024.0);
  double mibps = mib / secs;
  double gibps = gib / secs;

  printf("%7zu | %10.2f MiB | %8.3f s | %10.2f MiB/s | %6.3f GiB/s\n",
         msg, mib, secs, mibps, gibps);

  free(snd_buf);
  return 0;
}

int main(int argc, char **argv){
  // Default sizes (bytes)
  const size_t sizes[] = {4,16,64,256,1024,4096,16384,65536,262144,524288};
  const int ns = (int)(sizeof(sizes)/sizeof(sizes[0]));

  // Defaults (tweak via args):
  // argv[1]=total MiB per size (default 256), argv[2]=parent CPU, argv[3]=child CPU, argv[4]=pipe size bytes (0=auto)
  double total_mib = 256.0;
  int parent_cpu = 0, child_cpu = 1;
  long pipe_sz = 0;
  if (argc > 1) total_mib  = atof(argv[1]);
  if (argc > 2) parent_cpu = atoi(argv[2]);
  if (argc > 3) child_cpu  = atoi(argv[3]);
  if (argc > 4) pipe_sz    = strtol(argv[4], NULL, 0);

  double hz = tsc_hz_calibrate();
  fprintf(stderr, "Calibrated TSC: %.3f MHz  | parent_cpu=%d child_cpu=%d total=%.1f MiB/size\n",
          hz/1e6, parent_cpu, child_cpu, total_mib);

  printf("Message |   Total     |  Elapsed  |     Throughput     | Throughput\n");
  printf("  bytes |   sent      |   time    |     (MiB/s)        |  (GiB/s)\n");
  printf("--------+-------------+-----------+---------------------+-----------\n");

  size_t total_bytes = (size_t)(total_mib * 1024.0 * 1024.0);
  for (int i=0; i<ns; ++i) {
    if (run_size(sizes[i], total_bytes, parent_cpu, child_cpu, pipe_sz, hz) != 0) return 1;
  }
  return 0;
}

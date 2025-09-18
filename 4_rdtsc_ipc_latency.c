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

static inline void cpuid_serialize(void){ unsigned a,b,c,d; __get_cpuid(0,&a,&b,&c,&d); }
static inline uint64_t rdtsc_begin(void){ cpuid_serialize(); return __rdtsc(); }
static inline uint64_t rdtsc_end(void){ unsigned aux; uint64_t t=__rdtscp(&aux); cpuid_serialize(); return t; }

static void pin_to_cpu(int cpu){
  cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu,&set);
  if (sched_setaffinity(0,sizeof(set),&set)!=0){ perror("sched_setaffinity"); exit(1); }
}

static int write_full(int fd, const void *buf, size_t len){
  const uint8_t *p=(const uint8_t*)buf; size_t n=0;
  while(n<len){ ssize_t w=write(fd,p+n,len-n); if(w<0){ if(errno==EINTR) continue; perror("write"); return -1; } n+=(size_t)w; }
  return 0;
}
static int read_full(int fd, void *buf, size_t len){
  uint8_t *p=(uint8_t*)buf; size_t n=0;
  while(n<len){ ssize_t r=read(fd,p+n,len-n); if(r<0){ if(errno==EINTR) continue; perror("read"); return -1; } if(r==0){ fprintf(stderr,"EOF\n"); return -1; } n+=(size_t)r; }
  return 0;
}

static double tsc_hz_calibrate(void){
  struct timespec a,b,req={.tv_sec=0,.tv_nsec=300000000L}; // ~300ms
  clock_gettime(CLOCK_MONOTONIC_RAW,&a);
  uint64_t c0=rdtsc_begin(); nanosleep(&req,NULL); uint64_t c1=rdtsc_end();
  clock_gettime(CLOCK_MONOTONIC_RAW,&b);
  double dt=(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9;
  return (c1-c0)/dt;
}

static int cmp_u64(const void *x,const void *y){
  uint64_t a=*(const uint64_t*)x,b=*(const uint64_t*)y;
  return (a>b)-(a<b);
}

// run one size: returns 0 on success
static int run_size(size_t msg, int warmup, int iters, int cpu, double tsc_hz){
  int p_ab[2], p_ba[2];
  if (pipe(p_ab) || pipe(p_ba)) { perror("pipe"); return -1; }

  // Try to enlarge pipe to at least msg (ok if it fails)
  long want = (long)msg;
  if (want > 0) {
    (void)fcntl(p_ab[1], F_SETPIPE_SZ, want);
    (void)fcntl(p_ba[1], F_SETPIPE_SZ, want);
  }

  void *snd_buf, *rcv_buf;
  if (posix_memalign(&snd_buf, 64, msg) || posix_memalign(&rcv_buf, 64, msg)) {
    perror("posix_memalign"); return -1;
  }
  memset(snd_buf, 0xA5, msg);

  pid_t pid=fork();
  if (pid<0){ perror("fork"); return -1; }

  if (pid==0){
    // child: echo msg bytes back each time
    pin_to_cpu(cpu);
    close(p_ab[1]); close(p_ba[0]);
    int total = warmup + iters;
    for (int i=0;i<total;i++){
      if (read_full(p_ab[0], rcv_buf, msg)!=0) _exit(2);
      if (write_full(p_ba[1], rcv_buf, msg)!=0) _exit(3);
    }
    close(p_ab[0]); close(p_ba[1]); _exit(0);
  }

  // parent
  pin_to_cpu(cpu);
  close(p_ab[0]); close(p_ba[1]);

  // warmup
  for (int i=0;i<warmup;i++){
    if (write_full(p_ab[1], snd_buf, msg)!=0) return -1;
    if (read_full (p_ba[0], rcv_buf, msg)!=0) return -1;
  }

  uint64_t *samples = (uint64_t*)malloc((size_t)iters*sizeof(uint64_t));
  if (!samples){ perror("malloc"); return -1; }

  for (int i=0;i<iters;i++){
    uint64_t t0=rdtsc_begin();
    if (write_full(p_ab[1], snd_buf, msg)!=0) return -1;
    if (read_full (p_ba[0], rcv_buf, msg)!=0) return -1;
    uint64_t t1=rdtsc_end();
    samples[i]=t1-t0; // round-trip in cycles
  }

  close(p_ab[1]); close(p_ba[0]);
  int status=0; waitpid(pid,&status,0);

  // stats
  uint64_t min=~0ULL, sum=0;
  for (int i=0;i<iters;i++){ if (samples[i]<min) min=samples[i]; sum+=samples[i]; }
  qsort(samples, iters, sizeof(uint64_t), cmp_u64);
  uint64_t med = samples[iters/2];
  double cyc2ns = 1e9/tsc_hz;

  // report ONE-WAY (= round-trip/2)
  double one_min_ns = (min * cyc2ns) / 2.0;
  double one_med_ns = (med * cyc2ns) / 2.0;
  double one_avg_ns = ((double)sum/iters * cyc2ns) / 2.0;

  printf("%7zu  | %12.0f | %12.0f | %12.0f | %9.3f | %9.3f | %9.3f\n",
         msg,
         (double)min/2.0, (double)med/2.0, (double)sum/(2.0*iters),
         one_min_ns, one_med_ns, one_avg_ns);

  free(samples); free(snd_buf); free(rcv_buf);
  return 0;
}

int main(int argc, char **argv){
  // sizes to test
  const size_t sizes[] = {4,16,64,256,1024,4096,16384,65536,262144,524288};
  const int nsizes = (int)(sizeof(sizes)/sizeof(sizes[0]));

  // defaults; you can override via CLI
  int cpu = 0;
  double target_bytes = 128.0*1024*1024; // ~128MiB per size total xfer (round-trip)
  int min_iters = 200, warmup = 100;

  if (argc > 1) target_bytes = atof(argv[1]);  // bytes per size
  if (argc > 2) warmup       = atoi(argv[2]);
  if (argc > 3) cpu          = atoi(argv[3]);

  pin_to_cpu(cpu); // pin parent (and each child will pin too)

  double hz = tsc_hz_calibrate();
  fprintf(stderr, "Calibrated TSC: %.3f MHz (cpu=%d)\n", hz/1e6, cpu);

  printf("Message |   one-way cycles (min/med/avg)   |   one-way nanoseconds (min/med/avg)\n");
  printf("  bytes |        min      |       med      |       avg      |     min   |    med   |    avg   \n");
  printf("--------+-----------------+----------------+----------------+-----------+----------+----------\n");

  for (int i=0;i<nsizes;i++){
    size_t msg = sizes[i];
    // choose iterations so total bytes per size ≈ target_bytes
    int iters = (int)(target_bytes / (double)msg);
    if (iters < min_iters) iters = min_iters;
    if (run_size(msg, warmup, iters, cpu, hz) != 0) return 1;
  }
  return 0;
}

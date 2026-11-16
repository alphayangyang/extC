// memhier.c - 本机内存层级探针：延迟梯（64 B line 随机置换环、依赖链）、TLB 梯（4 KiB 与 2 MiB 页）、顺序带宽。
// 读法见 docs/analysis/CALIBRATION.md 第 11 节。
// 用法：cc -O2 -o /tmp/memhier tools/probe/memhier.c && taskset -c 6 /tmp/memhier 6
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sched.h>

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static uint64_t rng(uint64_t *s){uint64_t x=*s;x^=x<<13;x^=x>>7;x^=x<<17;*s=x;return x;}

static void build_cycle(size_t n, size_t stride, char *base) {
  uint64_t s = 88172645463325252ull;
  size_t *perm = malloc(n*sizeof(size_t));
  for (size_t i=0;i<n;i++) perm[i]=i;
  for (size_t i=n;i>1;i--){size_t j=rng(&s)%i; size_t t=perm[i-1];perm[i-1]=perm[j];perm[j]=t;}
  for (size_t i=0;i<n;i++){ size_t nxt = perm[(i+1)%n]; *(size_t*)(base + perm[i]*stride) = nxt*stride; }
  free(perm);
}

static uint64_t sink;
static double chase_ns(char *base, long iters) {
  size_t off = 0; double t0=now();
  for (long i=0;i<iters;i++) off = *(size_t*)(base+off);
  double t1=now();
  __asm__ __volatile__("" : "+r"(off));
  sink += off;
  return (t1-t0)*1e9/(double)iters;
}

// 自适应：先 20k 次估 ns/load，再定 iters 使单次约 budget_ns，取 5 次 min。
static double measure(char *base, size_t stride, double budget_ns, int reps) {
  double est = chase_ns(base, 20000);
  if (est < 0.05) est = 0.05;
  long iters = (long)(budget_ns/est);
  if (iters < 20000) iters = 20000;
  if (iters > 40000000) iters = 40000000;
  (void)stride;
  double best = 1e18;
  for (int r=0;r<reps;r++){ double v = chase_ns(base, iters); if (v<best) best=v; }
  return best;
}

/* Upper bound on the footprint the ladder walks, settable as argv[2]: the calibration precheck only needs one
 * DRAM point and should not pay for the 256 MiB rungs (see tests/gc-p0/calib_cseg.py). */
static size_t FP_MAX = (size_t)-1;

static void latency_ladder(void) {
  size_t fp[] = {16u<<10,32u<<10,64u<<10,128u<<10,256u<<10,512u<<10,1u<<20,2u<<20,4u<<20,8u<<20,16u<<20,24u<<20,32u<<20,36u<<20,40u<<20,48u<<20,64u<<20,128u<<20,256u<<20};
  printf("## latency ladder (random cycle over 64B lines), ns/load, min of 5\n");
  printf("%12s %10s %10s\n","footprint","lines","ns/load");
  for (unsigned k=0;k<sizeof(fp)/sizeof(fp[0]);k++){
    size_t bytes = fp[k]; size_t n = bytes/64;
    if (bytes > FP_MAX) continue;
    char *b = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
    if (b==MAP_FAILED){printf("%12zu mmap FAIL\n",bytes); fflush(stdout); continue;}
    build_cycle(n,64,b);
    double v = measure(b,64,2.0e8,5);
    printf("%12zu %10zu %10.2f\n",bytes,n,v); fflush(stdout);
    munmap(b,bytes);
  }
}

static void tlb_ladder(int huge) {
  unsigned p4[] = {8,16,32,64,128,256,512,1024,2048,4096,8192,16384};
  unsigned p2[] = {4,8,16,24,32,48,64,96,128};
  size_t pg = huge ? (2u<<20) : (4u<<10);
  unsigned *np = huge ? p2 : p4;
  unsigned cnt = huge ? (unsigned)(sizeof(p2)/sizeof(p2[0])) : (unsigned)(sizeof(p4)/sizeof(p4[0]));
  printf("## TLB ladder (%s pages), ns/load, min of 3\n", huge?"2MiB":"4KiB");
  printf("%10s %12s %10s\n","pages","bytes","ns/load");
  fflush(stdout);
  for (unsigned k=0;k<cnt;k++){
    size_t n = np[k]; size_t bytes = n*pg;
    char *b = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
    if (b==MAP_FAILED){printf("%10zu mmap FAIL\n",n); fflush(stdout); continue;}
    if (huge) madvise(b,bytes,MADV_HUGEPAGE);
    memset(b,0,bytes);
    build_cycle(n,pg,b);
    double v = measure(b,pg,1.5e8,3);
    printf("%10zu %12zu %10.2f\n",n,bytes,v); fflush(stdout);
    munmap(b,bytes);
  }
}

static void bandwidth(void) {
  size_t bytes = 192u<<20; char *a = mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
  char *b = mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
  memset(a,1,bytes); memset(b,2,bytes);
  printf("## bandwidth (192 MiB), GB/s\n");
  for (int r=0;r<3;r++){
    double t0=now(); uint64_t s=0; for(size_t i=0;i<bytes;i+=64) s+=*(volatile uint64_t*)(a+i); double t1=now();
    printf("read   %6.2f  (guard %llu)\n", bytes/(t1-t0)/1e9, (unsigned long long)s);
    t0=now(); for(size_t i=0;i<bytes;i+=64) *(volatile uint64_t*)(a+i)=1; t1=now();
    printf("write  %6.2f\n", bytes/(t1-t0)/1e9);
    t0=now(); memcpy(b,a,bytes); t1=now();
    printf("copy   %6.2f (r+w as 2x)\n", 2.0*bytes/(t1-t0)/1e9);
    fflush(stdout);
  }
  munmap(a,bytes); munmap(b,bytes);
}

int main(int argc,char**argv){
  setvbuf(stdout,NULL,_IOLBF,0);
  if(argc>1 && strcmp(argv[1],"none")!=0){cpu_set_t s;CPU_ZERO(&s);CPU_SET(atoi(argv[1]),&s);sched_setaffinity(0,sizeof(s),&s);}
  if(argc>2){ FP_MAX = (size_t)strtoull(argv[2],NULL,10); latency_ladder(); return 0; }
  latency_ladder();
  tlb_ladder(0);
  tlb_ladder(1);
  bandwidth();
  return 0;
}
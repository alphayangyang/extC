// 三条路拿一块"全零内存"，**每条都真的逐页读一遍**，所以编译器抹不掉 memset。
// 这是唯一可信的口径 —— 之前一版把 memset 优化掉了，测出 4600 倍那种假数。
//
// 问题：`malloc+memset` 在多大以上就不如 `calloc` / `mmap`？
// 目的：如果 `calloc`（纯标准 C）够好，就不必引 mmap（平台依赖）。
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
volatile long sink; static long acc = 0;

/* 真的逐页读（写进 volatile 累加器）*/
static long touch(unsigned char *p, size_t n){ long s=0; for(size_t j=0;j<n;j+=64) s+=p[j]; return s; }

int main(void){
    size_t sizes[] = {4096, 65536, 262144, 1UL<<20, 4UL<<20, 16UL<<20, 64UL<<20};
    printf("%-10s %16s %16s %16s   %s\n", "块", "malloc+memset", "calloc", "mmap", "省多少");
    for (int k = 0; k < 7; k++) {
        size_t SZ = sizes[k];
        long N = (long)(256UL*1024*1024 / SZ); if (N > 20000) N = 20000; if (N < 30) N = 30;

        double t0 = now();
        for (long i = 0; i < N; i++) {
            unsigned char *p = malloc(SZ); memset(p, 0, SZ); p[0]=1; acc += touch(p, SZ); free(p);
        }
        double t1 = now();
        for (long i = 0; i < N; i++) {
            unsigned char *p = calloc(1, SZ);                  p[0]=1; acc += touch(p, SZ); free(p);
        }
        double t2 = now();
        for (long i = 0; i < N; i++) {
            unsigned char *p = mmap(NULL,SZ,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            p[0]=1; acc += touch(p, SZ); munmap(p, SZ);
        }
        double t3 = now();

        double a=(t1-t0)/N*1e6, b=(t2-t1)/N*1e6, c=(t3-t2)/N*1e6;
        double best = a < b ? (a < c ? a : c) : (b < c ? b : c);
        printf("%-10zu %13.1f us %13.1f us %13.1f us   %+.1fx\n", SZ, a, b, c, (best - a) / a * 100.0);
    }
    sink = acc;
    return 0;
}

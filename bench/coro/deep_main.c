#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#ifndef K
#define K 2000
#endif
#ifndef M
#define M 5000
#endif
#define main __extc_generated_main
#include "deep_gen.c"
#undef main
static int64_t now_ns(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec*1000000000LL+ts.tv_nsec;}
int main(void){
    volatile int64_t sink = 0;
    int (*volatile fnp)(void) = __extc_generated_main;   /* 挡掉"提到循环外" ✓ */
    int64_t t0 = now_ns();
    for (int64_t i = 0; i < K; i++) sink += fnp();
    int64_t dt = now_ns() - t0;
    printf("extC 深 %-4d 层 : %8.3f ns/次挂起+恢复   (%d×%d, sink=%lld)\n",
           DEPTH_N, (double)dt/((double)K*M), K, M, (long long)sink);
    return 0;
}

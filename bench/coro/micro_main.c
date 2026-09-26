#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#define main __extc_generated_main
#include "micro_gen.c"
#undef main
static int64_t now_ns(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec*1000000000LL+ts.tv_nsec;}
int main(void){
    const int64_t K = 2000;
    volatile int64_t sink = 0;
    /* volatile 函数指针：否则 gcc 会把这个纯调用提到循环外，时间就成 0 了 */
    int (*volatile fnp)(void) = __extc_generated_main;
    int64_t t0 = now_ns();
    for (int64_t i = 0; i < K; i++) sink += fnp();
    int64_t dt = now_ns() - t0;
    printf("spawn+drive : %7.2f ns/个   (1000×%lld, sink=%lld)\n", (double)dt/(K*1000), (long long)K, (long long)sink);
    printf("switch      : %7.2f ns/次   (10000×%lld)\n", (double)dt/(K*10000), (long long)K);
    printf("deep1       : %7.2f ns/次   (5000×%lld, 每层 0)\n", (double)dt/(K*5000), (long long)K);
    printf("deep3       : %7.2f ns/次   (5000×%lld, 每层 2 次转发)\n", (double)dt/(K*5000), (long long)K);
    printf("  => 每层转发 %7.2f ns\n", (double)dt/(K*5000*2));
    return 0;
}

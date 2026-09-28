#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
typedef struct { float *data; int64_t len; } slice_f32;
static inline float *slice_f32_index(slice_f32 v, int64_t i, const char *f, int l) {
    if (!v.data) { fprintf(stderr, "%s:%d: no storage\n", f, l); exit(1); }
    if (i < 0 || i >= v.len) { fprintf(stderr, "%s:%d: oob %lld %lld\n", f, l, (long long)i, (long long)v.len); exit(1); }
    return &v.data[i];
}
static int32_t mul(int64_t n, slice_f32 a, slice_f32 b, slice_f32 c) {
    int64_t i = 0;
    while (i < n) { int64_t k = 0;
        while (k < n) {
            float aik = (*slice_f32_index(a, (int64_t)(((i * n) + k)), "m", 7));
            int64_t j = 0;
            while (j < n) {
                (*slice_f32_index(c, (int64_t)(((i * n) + j)), "m", 10)) = ((*slice_f32_index(c, (int64_t)(((i * n) + j)), "m", 10)) + (aik * (*slice_f32_index(b, (int64_t)(((k * n) + j)), "m", 10))));
                j = (j + ((int64_t)(1)));
            }
            k = (k + ((int64_t)(1)));
        }
        i = (i + ((int64_t)(1)));
    }
    return 0;
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+1e-9*t.tv_nsec; }
int main(int argc,char**argv){
    int64_t n = 768;
    float *A=malloc(n*n*4),*B=malloc(n*n*4),*C=calloc(n*n,4);
    for(int64_t i=0;i<n*n;i++){A[i]=(float)(i%7)*0.5f;B[i]=(float)(i%5)*0.25f;}
    slice_f32 a={A,n*n},b={B,n*n},c={C,n*n};
    double t0=now(); mul(n,a,b,c); double t1=now();
    double chk=0; for(int64_t i=0;i<n*n;i++) chk+=C[i];
    printf("%.2f GFLOP/s (%.1f ms) chk=%.1f\n", 2.0*n*n*n/(t1-t0)/1e9, (t1-t0)*1e3, chk);
    return 0; }

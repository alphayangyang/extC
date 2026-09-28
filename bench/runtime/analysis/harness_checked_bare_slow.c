#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+1e-9*t.tv_nsec; }
typedef struct { float *data; int64_t len; } slice_f32;
static inline float *slice_f32_index(slice_f32 v, int64_t i, const char *f, int l) {
    if (!v.data) { fprintf(stderr, "%s:%d: the view has no storage\n", f, l); exit(1); }
    if (i < 0 || i >= v.len) { fprintf(stderr, "%s:%d: trap: index %lld out of range (length %lld)\n", f, l, (long long)i, (long long)v.len); exit(1); }
    return &v.data[i];
}
static inline float *slice_f32_index_exp(slice_f32 v, int64_t i, const char *f, int l) {
    if (__builtin_expect(i < 0 || i >= v.len, 0)) { fprintf(stderr, "%s:%d: oob\n", f, l); exit(1); }
    return &v.data[i];
}
static void mulp(int64_t n, slice_f32 a, slice_f32 b, slice_f32 c){
  int64_t i = 0;
  while (i < n) { int64_t k = 0;
    while (k < n) {
      float aik = (*slice_f32_index(a, (((i * n) + k)), "m", 1));
      int64_t j = 0;
      while (j < n) {
        (*slice_f32_index(c, (((i * n) + j)), "m", 1)) = (*slice_f32_index(c, (((i * n) + j)), "m", 1)) + (aik * (*slice_f32_index(b, (((k * n) + j)), "m", 1)));
        j = (j + ((int64_t)(1)));
      }
      k = k + 1; }
    i = i + 1; } }

int main(void){
  int64_t n=768; float *A=malloc(n*n*4),*B=malloc(n*n*4),*C=calloc(n*n,4);
  for(int64_t i=0;i<n*n;i++){A[i]=(float)(i%7)*0.5f;B[i]=(float)(i%5)*0.25f;}
  slice_f32 a={A,n*n},b={B,n*n},c={C,n*n};
  double t0=now(); mulp(n,a,b,c); double t1=now();
  double chk=0; for(int64_t i=0;i<n*n;i++) chk+=C[i];
  printf("%.2f GFLOP/s (%.1f ms) chk=%.1f\n", 2.0*n*n*n/(t1-t0)/1e9,(t1-t0)*1e3,chk); return 0; }

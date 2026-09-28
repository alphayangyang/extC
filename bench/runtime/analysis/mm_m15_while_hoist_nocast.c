#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+1e-9*t.tv_nsec; }
typedef struct { float *data; int64_t len; } slice_f32;
static void mulp(int64_t n, slice_f32 a, slice_f32 b, slice_f32 c){
  float *ap=a.data,*bp=b.data,*cp=c.data;
  int64_t i = 0;
  while (i < n) { int64_t k = 0;
    while (k < n) { float aik = ap[(i * n) + k]; int64_t j = 0;
      while (j < n) { cp[(i * n) + j] = cp[(i * n) + j] + (aik * bp[(k * n) + j]); j = j + 1; }
      k = k + 1; }
    i = i + 1; } }
int main(void){
  int64_t n=768; float *A=malloc(n*n*4),*B=malloc(n*n*4),*C=calloc(n*n,4);
  for(int64_t i=0;i<n*n;i++){A[i]=(float)(i%7)*0.5f;B[i]=(float)(i%5)*0.25f;}
  slice_f32 a={A,n*n},b={B,n*n},c={C,n*n};
  double t0=now(); mulp(n,a,b,c); double t1=now();
  double chk=0; for(int64_t i=0;i<n*n;i++) chk+=C[i];
  printf("%.2f GFLOP/s (%.1f ms) chk=%.1f\n", 2.0*n*n*n/(t1-t0)/1e9,(t1-t0)*1e3,chk); return 0; }

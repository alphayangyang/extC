#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+1e-9*t.tv_nsec; }
static void mulp(int64_t n, float *a, float *b, float *c) {
    int64_t i = 0;
    while (i < n) { int64_t k = 0;
        while (k < n) {
            float aik = a[(int64_t)(((i * n) + k))];
            int64_t j = 0;
            while (j < n) {
                c[(int64_t)(((i * n) + j))] = c[(int64_t)(((i * n) + j))] + (aik * b[(int64_t)(((k * n) + j))]);
                j = (j + ((int64_t)(1)));
            }
            k = (k + ((int64_t)(1)));
        }
        i = (i + ((int64_t)(1)));
    }
}
int main(void){ int64_t n=768;
  float *a=malloc(n*n*4),*b=malloc(n*n*4),*c=calloc(n*n,4);
  for(int64_t i=0;i<n*n;i++){a[i]=(float)(i%7)*0.5f;b[i]=(float)(i%5)*0.25f;}
  double t0=now(); mulp(n,a,b,c); double t1=now();
  double chk=0; for(int64_t i=0;i<n*n;i++) chk+=c[i];
  printf("%.2f GFLOP/s (%.1f ms) chk=%.1f\n", 2.0*n*n*n/(t1-t0)/1e9,(t1-t0)*1e3,chk); return 0; }

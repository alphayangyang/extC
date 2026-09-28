#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+1e-9*t.tv_nsec; }
int main(int argc,char**argv){
  int64_t n = argc>1 ? atoll(argv[1]) : 768;
  float *a=malloc(n*n*4), *b=malloc(n*n*4), *c=malloc(n*n*4);
  for(int64_t i=0;i<n*n;i++){ a[i]=(float)(i%7)*0.5f; b[i]=(float)(i%5)*0.25f; c[i]=0; }
  double t0=now();
  for(int64_t i=0;i<n;i++){
    for(int64_t k=0;k<n;k++){
      float aik=a[i*n+k];
      for(int64_t j=0;j<n;j++) c[i*n+j]+=aik*b[k*n+j];
    }
  }
  double t1=now(); double f=2.0*(double)n*n*n;
  double chk=0; for(int64_t i=0;i<n*n;i++) chk+=c[i];
  printf("%.2f GFLOP/s  (%.1f ms)  chk=%.1f\n", f/(t1-t0)/1e9, (t1-t0)*1e3, chk);
  return 0; }

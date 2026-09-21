#include <stdint.h>
#include <stdio.h>
static uint32_t src[1048576], dst[1048576]; static int64_t cnt[256];
static uint64_t st, inc;
static uint32_t rnd(void){ uint64_t o=st; st=o*6364136223846793005ULL+inc;
  uint32_t x=(uint32_t)(((o>>18u)^o)>>27u), r=(uint32_t)(o>>59u); return (x>>r)|(x<<((-r)&31)); }
static void seed(uint64_t s,uint64_t q){ st=0; inc=(q<<1u)|1u; rnd(); st+=s; rnd(); }
int main(void){ int64_t n=1048576; seed(7,54);
  for(int64_t i=0;i<n;i++) src[i]=rnd();
  for(int pass=0;pass<4;pass++){ uint64_t sh=(uint64_t)(pass*8);
    for(int k=0;k<256;k++) cnt[k]=0;
    for(int64_t i=0;i<n;i++) cnt[(src[i]>>sh)&255]++;
    int64_t sum=0; for(int k=0;k<256;k++){ int64_t c=cnt[k]; cnt[k]=sum; sum+=c; }
    for(int64_t i=0;i<n;i++){ int64_t b=(src[i]>>sh)&255; dst[cnt[b]++]=src[i]; }
    for(int64_t i=0;i<n;i++) src[i]=dst[i]; }
  int64_t chk=0; for(int64_t i=0;i<n;i++) chk+=src[i];
  printf("radix = %lld\n",(long long)chk); }

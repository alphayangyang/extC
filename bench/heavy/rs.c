#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
static uint32_t src[10000000], dst[10000000];
static uint64_t big[4000000], bigTmp[4000000];
static int64_t cnt[256];
static uint64_t st, inc;
static uint32_t rnd(void){ uint64_t o=st; st=o*6364136223846793005ULL+inc;
  uint32_t x=(uint32_t)(((o>>18u)^o)>>27u), r=(uint32_t)(o>>59u); return (x>>r)|(x<<((-r)&31)); }
static void seed(uint64_t s,uint64_t q){ st=0; inc=(q<<1u)|1u; rnd(); st+=s; rnd(); }

static void sortU32(int64_t n, int64_t passes){
  for(int64_t pass=0;pass<passes;pass++){ uint64_t sh=(uint64_t)(pass*8);
    for(int k=0;k<256;k++) cnt[k]=0;
    for(int64_t i=0;i<n;i++) cnt[(uint64_t)src[i]>>sh & 255]++;
    int64_t sum=0; for(int k=0;k<256;k++){ int64_t c=cnt[k]; cnt[k]=sum; sum+=c; }
    for(int64_t i=0;i<n;i++){ int64_t b=(int64_t)(((uint64_t)src[i]>>sh)&255); dst[cnt[b]++]=src[i]; }
    for(int64_t i=0;i<n;i++) src[i]=dst[i]; } }
static void sortU64(int64_t n, int64_t passes){
  for(int64_t pass=0;pass<passes;pass++){ uint64_t sh=(uint64_t)(pass*8);
    for(int k=0;k<256;k++) cnt[k]=0;
    for(int64_t i=0;i<n;i++) cnt[(big[i]>>sh)&255]++;
    int64_t sum=0; for(int k=0;k<256;k++){ int64_t c=cnt[k]; cnt[k]=sum; sum+=c; }
    for(int64_t i=0;i<n;i++){ int64_t b=(int64_t)((big[i]>>sh)&255); bigTmp[cnt[b]++]=big[i]; }
    for(int64_t i=0;i<n;i++) big[i]=bigTmp[i]; } }
static bool sorted32(int64_t n){ for(int64_t i=1;i<n;i++) if(src[i-1]>src[i]) return false; return true; }
static bool sorted64(int64_t n){ for(int64_t i=1;i<n;i++) if(big[i-1]>big[i]) return false; return true; }
int main(void){ seed(7,54); int64_t n=10000000;
  for(int64_t i=0;i<n;i++) src[i]=rnd();
  sortU32(n,4);
  printf("① 1e7 u32 有序？ %s  首/末 = %u %u\n", sorted32(n)?"true":"false", src[0], src[n-1]);
  int64_t m=4000000;
  for(int64_t i=0;i<m;i++) big[i]=((uint64_t)rnd()<<32)|rnd();
  sortU64(m,8);
  printf("② 4e6 u64 有序？ %s  末 = %llu\n", sorted64(m)?"true":"false", (unsigned long long)big[m-1]); }

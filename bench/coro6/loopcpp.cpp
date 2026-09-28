#include <cstdio>
#include <cstdint>
static int64_t run(int64_t k, int64_t m){ int64_t s=0;
  for(int64_t id=0; id<k; id++) for(int64_t i=0;i<m;i++) s += (i%65536)*65521 + id*40503;
  return s; }
int main(){ int64_t t=run(8,2000); for(int r=0;r<1;r++) t+=run(512,20000);
  printf("%lld\n",(long long)t); return 0; }

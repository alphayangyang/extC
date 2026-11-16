// C++ std::vector<int64_t> growth: the fair opponent for the growable-array comparison.
// build: c++ -O2 -std=c++17 -o /tmp/r1/vectorbench tools/probe/vectorbench.cpp
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <sys/resource.h>

static long long now_ns(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000000000LL+t.tv_nsec;}
static long maxrss_kb(){rusage u;getrusage(RUSAGE_SELF,&u);return u.ru_maxrss;}

struct CountAlloc {
  static long long nAlloc, bytesAlloc;
  using value_type = int64_t;
  CountAlloc() = default;
  template<class U> struct rebind { using other = CountAlloc; };
  template<class U> CountAlloc(const CountAlloc&) {}
  int64_t* allocate(std::size_t n){ nAlloc++; bytesAlloc += (long long)n*(long long)sizeof(int64_t);
    int64_t*p=(int64_t*)std::malloc(n*sizeof(int64_t)); if(!p) std::abort(); return p; }
  void deallocate(int64_t*p, std::size_t){ std::free(p); }
  bool operator==(const CountAlloc&) const { return true; }
  bool operator!=(const CountAlloc&) const { return false; }
};
long long CountAlloc::nAlloc=0, CountAlloc::bytesAlloc=0;

int main(int argc,char**argv){
  const char* mode = argc>1?argv[1]:"pushback";
  const size_t FINAL=8000000;
  unsigned long long sum=0;
  long long t0=now_ns();
  size_t cap=0;
  if(!strcmp(mode,"pushback")){
    std::vector<int64_t,CountAlloc> v;
    for(size_t i=0;i<FINAL;i++) v.push_back((int64_t)(i*3+1));
    cap=v.capacity();
    for(size_t i=0;i<v.size();i+=997) sum+=(unsigned long long)v[i];
  } else if(!strcmp(mode,"reserve")){
    std::vector<int64_t,CountAlloc> v; v.reserve(FINAL);
    for(size_t i=0;i<FINAL;i++) v.push_back((int64_t)(i*3+1));
    cap=v.capacity();
    for(size_t i=0;i<v.size();i+=997) sum+=(unsigned long long)v[i];
  } else if(!strcmp(mode,"grow11")){
    std::vector<int64_t,CountAlloc> v;
    size_t c=1024; v.resize(c); for(size_t i=0;i<c;i++) v[i]=(int64_t)(i*3+1);
    while(c<FINAL){ size_t nc=c+c/10+1; v.resize(nc); for(size_t i=c;i<nc;i++) v[i]=(int64_t)(i*3+1); c=nc; }
    cap=v.capacity();
    for(size_t i=0;i<v.size();i+=997) sum+=(unsigned long long)v[i];
  } else { fprintf(stderr,"usage: vectorbench pushback|reserve|grow11\n"); return 2; }
  long long t1=now_ns();
  long long final_bytes=(long long)cap*(long long)sizeof(int64_t);
  printf("mode=%-8s final_cap=%zu reallocs=%lld alloc_mb=%.1f copied_mb=%.1f grow_ms=%.2f maxrss_kb=%ld sum=%llu\n",
    mode,cap,CountAlloc::nAlloc-1,CountAlloc::bytesAlloc/1048576.0,(CountAlloc::bytesAlloc-final_bytes)/1048576.0,(t1-t0)/1e6,maxrss_kb(),sum);
  return 0;
}
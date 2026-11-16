#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>

static long long now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000000000LL+t.tv_nsec;}
static void die(const char*m){perror(m);exit(1);}
static long maxrss_kb(void){struct rusage u;getrusage(RUSAGE_SELF,&u);return u.ru_maxrss;}

#define FINAL 8000000ULL
#define START 1024ULL
#define Q (2u<<20)

static unsigned char*reserve(size_t bytes,int huge){
  unsigned char*raw=mmap(NULL,bytes+Q,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
  if(raw==MAP_FAILED)die("mmap reserve");
  unsigned char*base=(unsigned char*)(((uintptr_t)raw+Q-1)&~(uintptr_t)(Q-1));
  size_t head=(size_t)(base-raw), tail=(size_t)((raw+bytes+Q)-(base+bytes));
  if(head) munmap(raw,head);
  if(tail) munmap(base+bytes,tail);
  if(huge && madvise(base,bytes,MADV_HUGEPAGE)!=0) perror("madvise");
  return base;
}

int main(int argc,char**argv){
  const char*p=argc>1?argv[1]:"slack4";
  int slack=4, huge=0, mode=0;
  if(!strcmp(p,"copy")) mode=0;
  else if(!strcmp(p,"big")) mode=2;
  else if(!strcmp(p,"bighuge")){ mode=2; huge=1; }
  else if(!strncmp(p,"slack",5)){ mode=1; slack=atoi(p+5); if(slack<2)slack=2; }
  else { fprintf(stderr,"usage: slackgrow copy|slackN|big|bighuge\n"); return 2; }

  size_t cap=START, max=START*2;
  int64_t *base=NULL;
  long long copies=0, copied_bytes=0, nmap=0, nmunmap=0, nmadv=0;
  long long t0=now_ns();
  if(mode==0){ base=(int64_t*)reserve(max*8,0); nmap++; }
  if(mode==1){ max=cap*slack; base=(int64_t*)reserve(max*8,0); nmap++; }
  if(mode==2){ max=(1ULL<<30)/8; base=(int64_t*)reserve(max*8,huge); nmap++; if(huge)nmadv++; }
  for(uint64_t i=0;i<cap;i++) base[i]=(int64_t)(i*3+1);

  while(cap<FINAL){
    size_t ncap=cap + cap/10 + 1;
    if(ncap>max){
      if(mode==2){ fprintf(stderr,"fixed reserve exhausted at %zu\n",cap); return 1; }
      size_t nmax = (mode==1)? ncap*slack : ncap*2;
      int64_t*nb=(int64_t*)reserve(nmax*8,0); nmap++;
      memcpy(nb,base,cap*8); copies++; copied_bytes+=(long long)cap*8;
      munmap((void*)base,max*8); nmunmap++;
      base=nb; max=nmax;
    }
    for(size_t i=cap;i<ncap;i++) base[i]=(int64_t)(i*3+1);
    cap=ncap;
  }
  long long t1=now_ns();
  unsigned long long sum=0; for(uint64_t i=0;i<cap;i+=997) sum+=(unsigned long long)base[i];
  printf("policy=%-8s final=%llu grow_ms=%.2f copies=%lld copied_mb=%.1f mmap=%lld munmap=%lld madvise=%lld maxrss_kb=%ld sum=%llu\n",
    p,(unsigned long long)cap,(t1-t0)/1e6,copies,copied_bytes/1048576.0,nmap,nmunmap,nmadv,maxrss_kb(),sum);
  return 0;
}
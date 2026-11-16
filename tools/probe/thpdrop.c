#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static long long now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000000000LL+t.tv_nsec;}
static void die(const char*m){perror(m);exit(1);}

static void smaps(const void*base,size_t len,long*rss,long*huge){
  FILE*f=fopen("/proc/self/smaps","r"); *rss=-1;*huge=-1; if(!f)return;
  char line[512]; unsigned long s=0,e=0; int in=0; unsigned long want=(unsigned long)base;
  while(fgets(line,sizeof line,f)){
    if(sscanf(line,"%lx-%lx",&s,&e)==2){ in = (s==want); continue; }
    if(!in) continue;
    if(!strncmp(line,"Rss:",4)) *rss=atol(line+4);
    else if(!strncmp(line,"AnonHugePages:",14)) *huge=atol(line+14);
    else if(!strncmp(line,"VmFlags:",8)) in=0;
  }
  fclose(f);
}

int main(int argc,char**argv){
  const char*mode=argc>1?argv[1]:"plain";
  long mb=argc>2?atol(argv[2]):64;
  int reps=argc>3?atoi(argv[3]):3;
  size_t len=(size_t)mb<<20, Q=2u<<20;
  unsigned char*raw=mmap(NULL,len+Q,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
  if(raw==MAP_FAILED)die("mmap");
  unsigned char*base=(unsigned char*)(((uintptr_t)raw+Q-1)&~(uintptr_t)(Q-1));
  size_t head=(size_t)(base-raw), tail=(size_t)((raw+len+Q)-(base+len));
  if(head) munmap(raw,head);
  if(tail) munmap(base+len,tail);
  int huge=!strcmp(mode,"huge");
  if(huge && madvise(base,len,MADV_HUGEPAGE)!=0) perror("madvise");
  size_t pages=(len+4095)/4096;
  printf("mode=%s mb=%ld len_kb=%zu pages=%zu\n",mode,mb,len/1024,pages);
  for(int r=0;r<reps;r++){
    long rss=0,ah=0;
    long long t0=now_ns();
    for(size_t i=0;i<pages;i++) base[i*4096]=(unsigned char)(i&0x7f);
    long long t1=now_ns();
    smaps(base,len,&rss,&ah);
    long long t2=now_ns();
    if(madvise(base,len,MADV_DONTNEED)!=0) perror("dontneed");
    long long t3=now_ns();
    long rss2=0,ah2=0; smaps(base,len,&rss2,&ah2);
    long long t4=now_ns();
    for(size_t i=0;i<pages;i++) base[i*4096]=(unsigned char)(i&0x7f);
    long long t5=now_ns();
    long rss3=0,ah3=0; smaps(base,len,&rss3,&ah3);
    printf("rep=%d touch_ms=%.2f rss=%ld anonhuge=%ld drop_ms=%.2f rss_after=%ld anonhuge_after=%ld retouch_ms=%.2f rss_b=%ld anonhuge_b=%ld\n",
      r,(t1-t0)/1e6,rss,ah,(t3-t2)/1e6,rss2,ah2,(t5-t4)/1e6,rss3,ah3);
  }
  return 0;
}
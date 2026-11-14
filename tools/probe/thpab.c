// thpab.c - 同一数据 footprint、同一访问序列，4 KiB 页 vs 2 MiB 页（MADV_HUGEPAGE）的 A/B。
// 用途：把 TLB 走表项从 cache/DRAM 项里分离出来（两臂的 line 数与访问顺序完全相同）。
// 读法见 docs/analysis/CALIBRATION.md 第 11 节。用法：cc -O2 -o /tmp/thpab tools/probe/thpab.c && taskset -c 6 /tmp/thpab 6
// 用途：把 TLB 项从 cache/DRAM 项里分离出来。
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sched.h>
#include <errno.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static uint64_t rng(uint64_t *s){uint64_t x=*s;x^=x<<13;x^=x>>7;x^=x<<17;*s=x;return x;}
static uint64_t sink;
static double chase(char *base,long iters){size_t off=0;__asm__ __volatile__(""::"r"(base):"memory");double t0=now();for(long i=0;i<iters;i++)off=*(size_t*)(base+off);double t1=now();__asm__ __volatile__(""::"r"(off));sink+=off;return (t1-t0)*1e9/(double)iters;}
static size_t huge_kb(void){FILE*f=fopen("/proc/self/smaps","r");if(!f)return 0;char line[256];size_t tot=0;while(fgets(line,sizeof line,f)){if(strncmp(line,"AnonHugePages:",14)==0)tot+=(size_t)strtoull(line+14,NULL,10);}fclose(f);return tot;}
static void cycle(size_t n,char*base,size_t stride){uint64_t s=12345678901ull;size_t*perm=malloc(n*sizeof(size_t));for(size_t i=0;i<n;i++)perm[i]=i;for(size_t i=n;i>1;i--){size_t j=rng(&s)%i;size_t t=perm[i-1];perm[i-1]=perm[j];perm[j]=t;}for(size_t i=0;i<n;i++)*(size_t*)(base+perm[i]*stride)=perm[(i+1)%n]*stride;free(perm);}
static double measure(char*base,double budget_ns){double est=chase(base,20000);if(est<0.05)est=0.05;long it=(long)(budget_ns/est);if(it<20000)it=20000;if(it>40000000)it=40000000;double best=1e18;for(int r=0;r<5;r++){double v=chase(base,it);if(v<best)best=v;}return best;}
int main(int argc,char**argv){
    if(argc>1){cpu_set_t s;CPU_ZERO(&s);CPU_SET(atoi(argv[1]),&s);sched_setaffinity(0,sizeof(s),&s);}
    printf("%10s %12s %12s %12s %10s\n","footprint","4K ns/load","2M ns/load","ratio","huge_kB");
    size_t fp[]={2u<<20,4u<<20,6u<<20,8u<<20,12u<<20,16u<<20,24u<<20,32u<<20,48u<<20,64u<<20,128u<<20,256u<<20};
    for(unsigned k=0;k<sizeof(fp)/sizeof(fp[0]);k++){
        size_t bytes=fp[k], n=bytes/64; double v[2];
        for(int mode=0;mode<2;mode++){
            char *raw=(char*)mmap(NULL,bytes+(2u<<20),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
            if(raw==MAP_FAILED){printf("mmap fail %s\n",strerror(errno));return 1;}
            char *b=(char*)(((uintptr_t)raw+(2u<<20)-1)&~((uintptr_t)(2u<<20)-1));
            if(mode==1) madvise(raw,bytes+(2u<<20),MADV_HUGEPAGE);
            memset(b,0,bytes);
            if(mode==1){ /* 再打一遍以触发 khugepaged 合并 */ int acc=0; for(size_t i=0;i<bytes;i+=4096) acc+=b[i]; sink+=acc; }
            cycle(n,b,64);
            v[mode]=measure(b,2.0e8);
            if(mode==1) printf("%10zu %12.2f %12.2f %12.3f %10zu (huge after)\n",bytes,v[0],v[1],v[1]/v[0],huge_kb());
            munmap(raw,bytes+(2u<<20));
        }
        if(0) printf("%10zu %12.2f\n",bytes,v[0]);
    }
    return 0;
}
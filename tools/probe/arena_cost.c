/* arena_cost.c -- what one VA arena of the extent substrate costs in address space, resident set, and time.
 *
 * The runtime keeps EXTC_RC_EXT_ARENAS reservations of 64 GiB each, lazily, and never unmaps them (src/back/rt/32-extent.inc).
 * This probe reproduces one reservation exactly and reports the three costs that are easy to confuse:
 *   - address space: VmSize grows by the reservation, VmRSS does not (MAP_NORESERVE, nothing touched)
 *   - metadata: nextQ = 4 B/quantum + ordQ = 1 B/quantum = 160 KiB for 32,768 quanta, independent of the reservation size
 *   - pages: only touched pages are resident; with MADV_HUGEPAGE a 2 MiB fault brings the whole quantum
 *
 * Usage: cc -O2 -std=gnu11 -o arena_cost tools/probe/arena_cost.c && taskset -c 6 ./arena_cost
 * Numbers in docs/analysis/GC-ROOFLINE.md 6.22.7 come from one run of this probe (core 6).
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}

/* VmSize and VmRSS from /proc/self/status. AnonHugePages is absent on this kernel (WSL2), so it is not reported. */
static void kv(const char *tag){
    FILE *f=fopen("/proc/self/status","r");char b[256],vs[64]="?",rs[64]="?";
    while(fgets(b,sizeof b,f)){
        if(!strncmp(b,"VmSize:",7))sscanf(b+7,"%63s",vs);
        if(!strncmp(b,"VmRSS:",6))sscanf(b+6,"%63s",rs);
    }
    if(f)fclose(f);
    printf("%-20s VmSize=%s kB  VmRSS=%s kB\n",tag,vs,rs);
}

int main(void){
    const size_t Q=(size_t)1<<21;              /* 2 MiB quantum, EXTC_RC_EXT_QSHIFT */
    const size_t NQ=(size_t)1<<15;             /* 32,768 quanta = 64 GiB, EXTC_RC_EXT_ORD */
    const size_t SPAN=NQ*Q;
    kv("before");

    double t0=now_ms();
    char *raw=(char *)mmap(0,SPAN+Q,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    double t1=now_ms();
    if(raw==MAP_FAILED){perror("mmap 64 GiB");return 1;}
    char *base=(char *)(((uintptr_t)raw+Q-1)&~(uintptr_t)(Q-1));
    kv("after mmap 64 GiB");
    printf("\n  mmap = %.1f us, alignment head = %zu B\n",(t1-t0)*1e3,(size_t)(base-raw));

    int32_t *nextQ=(int32_t *)calloc(NQ,sizeof(int32_t));
    uint8_t *ordQ=(uint8_t *)calloc(NQ,sizeof(uint8_t));
    if(!nextQ||!ordQ){fprintf(stderr,"calloc failed\n");return 1;}
    kv("after metadata");
    printf("\n  metadata = %zu KiB (nextQ %zu B + ordQ %zu B)\n",(NQ*sizeof(int32_t)+NQ)/1024,NQ*sizeof(int32_t),NQ);

    /* Touch 2 GiB of the reservation at quantum stride, with huge pages advised for the whole span. */
    size_t touch=(size_t)1024*Q;
    double a0=now_ms();
    if(madvise(base,touch,MADV_HUGEPAGE)!=0)fprintf(stderr,"madvise HUGEPAGE failed\n");
    double a1=now_ms();
    for(size_t i=0;i<touch;i+=Q)base[i]=(char)i;
    double a2=now_ms();
    kv("after touch 2 GiB");
    printf("\n  madvise = %.1f us, touch = %.1f ms (%.0f ns/KB)\n",(a1-a0)*1e3,(a2-a1),(a2-a1)*1e6/(touch/1024.0));

    /* Drop them again: what the return pass pays per byte. */
    double d0=now_ms();
    madvise(base,touch,MADV_DONTNEED);
    double d1=now_ms();
    kv("after DONTNEED");
    printf("\n  DONTNEED = %.1f ms (%.2f ns/KB)\n",(d1-d0),(d1-d0)*1e6/(touch/1024.0));
    return 0;
}
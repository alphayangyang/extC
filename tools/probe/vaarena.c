#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <time.h>

static long long now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000000000LL+t.tv_nsec;}
static void die(const char*m){perror(m);exit(1);}

#define QUNIT (2u<<20)
#define MAXORD 20

typedef struct {
  unsigned char *base; size_t bytes; int nq; int maxord;
  int32_t *next; uint8_t *ord; unsigned char *isfree;
  int32_t fl[MAXORD+1];
  long long nalloc, nfail, nsplit; int nmaps;
} Arena;

static void a_init(Arena*a,size_t bytes){
  memset(a,0,sizeof *a);
  a->bytes=bytes; a->nq=(int)(bytes/QUNIT);
  a->maxord=0; while((1<<a->maxord)<a->nq) a->maxord++;
  if((1<<a->maxord)!=a->nq){fprintf(stderr,"nq %d not pow2\n",a->nq);exit(1);}
  a->base=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
  if(a->base==MAP_FAILED)die("mmap arena");
  a->nmaps=1;
  a->next=malloc(sizeof(int32_t)*a->nq); a->ord=malloc(a->nq); a->isfree=calloc(a->nq,1);
  if(!a->next||!a->ord||!a->isfree)die("malloc");
  memset(a->ord,0xff,a->nq);
  for(int i=0;i<=MAXORD;i++) a->fl[i]=-1;
  a->fl[a->maxord]=0; a->ord[0]=(uint8_t)a->maxord; a->isfree[0]=1; a->next[0]=-1;
}

static int a_alloc(Arena*a,int nq){
  int k=0; while((1<<k)<nq) k++;
  if(k>a->maxord){a->nfail++;return -1;}
  int j=k; while(j<=a->maxord && a->fl[j]<0) j++;
  if(j>a->maxord){a->nfail++;return -1;}
  int32_t idx=a->fl[j]; a->fl[j]=a->next[idx]; a->isfree[idx]=0;
  while(j>k){ j--; int32_t b=idx+(1<<j); a->next[b]=a->fl[j]; a->fl[j]=b; a->ord[b]=(uint8_t)j; a->isfree[b]=1; a->nsplit++; }
  a->ord[idx]=(uint8_t)k; a->nalloc++;
  return idx;
}

static void a_free(Arena*a,int idx,int k){
  while(k<a->maxord){
    int32_t bud=idx^(1<<k);
    if(bud>=a->nq) break;
    if(!a->isfree[bud] || a->ord[bud]!=k) break;
    int32_t *p=&a->fl[k];
    while(*p>=0 && *p!=bud) p=&a->next[*p];
    if(*p!=bud) break;
    *p=a->next[bud]; a->isfree[bud]=0;
    idx = idx<bud? idx: bud; k++;
  }
  a->ord[idx]=(uint8_t)k; a->next[idx]=a->fl[k]; a->fl[k]=idx; a->isfree[idx]=1;
}

static int largest_free_order(Arena*a){ for(int k=a->maxord;k>=0;k--) if(a->fl[k]>=0) return k; return -1; }

static int count_vma(void){FILE*f=fopen("/proc/self/maps","r");if(!f)return -1;int c=0;char buf[512];while(fgets(buf,sizeof buf,f))c++;fclose(f);return c;}
static long status_kb(const char*key){FILE*f=fopen("/proc/self/status","r");if(!f)return -1;char line[256];long v=-1;size_t kl=strlen(key);while(fgets(line,sizeof line,f)){if(!strncmp(line,key,kl)){v=atol(line+kl);break;}}fclose(f);return v;}

int main(int argc,char**argv){
  const char*mode=argc>1?argv[1]:"shared";
  int n=argc>2?atoi(argv[2]):10000;
  int slack=argc>3?atoi(argv[3]):2;
  if(!strcmp(mode,"shared")){
    size_t gb=64; Arena a; a_init(&a,gb<<30);
    int *idx=malloc(sizeof(int)*n), *nqa=malloc(sizeof(int)*n);
    if(!idx||!nqa)die("malloc");
    long long t0=now_ns(); int ok=0;
    for(int i=0;i<n;i++){ int q=1+(i&1); int need=q*slack; int r=a_alloc(&a,need); idx[i]=r; nqa[i]=need; if(r>=0){ok++;} }
    long long t1=now_ns();
    long long used=(long long)ok*(1.5*slack)*QUNIT;
    printf("mode=shared n=%d slack=%d arena_gb=%zu ok=%d fail=%lld split=%lld\n",n,slack,gb,ok,a.nfail,a.nsplit);
    printf("alloc_ms=%.3f ns_per=%lld va_used_gb=%.1f rss_kb=%ld vma=%d\n",(t1-t0)/1e6,(t1-t0)/(ok?ok:1),used/1073741824.0,status_kb("VmRSS:"),count_vma());
    printf("largest_free_mib=%d\n",(1<<largest_free_order(&a))*2);
    return 0;
  }
  if(!strcmp(mode,"mmap")){
    void **p=malloc(sizeof(void*)*n); int *sz=malloc(sizeof(int)*n); if(!p||!sz)die("malloc");
    long long t0=now_ns(); int ok=0;
    for(int i=0;i<n;i++){ int q=1+(i&1); size_t need=(size_t)q*slack*QUNIT;
      void*m=mmap(NULL,need,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
      if(m==MAP_FAILED){p[i]=NULL;sz[i]=0;continue;} p[i]=m; sz[i]=(int)need; ((char*)m)[0]=1; ok++; }
    long long t1=now_ns();
    long long used=0; for(int i=0;i<n;i++) used+=sz[i];
    printf("mode=mmap n=%d slack=%d ok=%d fail=%d\n",n,slack,ok,n-ok);
    printf("alloc_ms=%.3f ns_per=%lld va_used_gb=%.1f rss_kb=%ld vma=%d\n",(t1-t0)/1e6,(t1-t0)/(ok?ok:1),used/1073741824.0,status_kb("VmRSS:"),count_vma());
    return 0;
  }
  if(!strcmp(mode,"frag")){
    size_t gb=8; Arena a; a_init(&a,gb<<30);
    int *idx=malloc(sizeof(int)*n); if(!idx)die("malloc");
    int got=0; for(int i=0;i<n;i++){ int r=a_alloc(&a,1); idx[i]=r; if(r<0)break; got++; }
    for(int i=0;i<got;i+=2) a_free(&a,idx[i],0);
    int lf=largest_free_order(&a);
    printf("mode=frag n=%d allocated=%d freed=%d arena_gb=%zu largest_free_mib=%d\n",n,got,(got+1)/2,gb,lf<0?0:(1<<lf)*2);
    int okc=0; for(int k=0;k<=8;k++){ int r=a_alloc(&a,1<<k); if(r>=0){a_free(&a,r,k);okc++;} }
    printf("largest_allocatable_order=%d\n",okc-1);
    int big=a_alloc(&a,4); printf("alloc_4quanta_after_frag=%s\n", big>=0?"ok":"FAIL");
    return 0;
  }
  fprintf(stderr,"usage: vaarena shared|mmap|frag [n] [slack]\n");
  return 2;
}
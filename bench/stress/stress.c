#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t st, inc;
static uint32_t rnd(void){ uint64_t o=st; st=o*6364136223846793005ULL+inc;
  uint32_t x=(uint32_t)(((o>>18u)^o)>>27u), r=(uint32_t)(o>>59u);
  return (x>>r)|(x<<((-r)&31)); }
static void seed(uint64_t s,uint64_t q){ st=0; inc=(q<<1u)|1u; rnd(); st+=s; rnd(); }
static uint64_t nextBounded(uint64_t b){ if(!b) return 0; uint64_t t=(0-b)%b, x=rnd();
  while(x<t) x=rnd(); return x%b; }
typedef struct node { int64_t value; struct node *next; } node;
static node *buildList(int64_t n){ node *h=malloc(sizeof(node)); h->value=(int64_t)nextBounded(1000); h->next=NULL;
  node *cur=h; for(int64_t i=0;i<n;i++){ node *c=malloc(sizeof(node)); c->value=(int64_t)nextBounded(1000); c->next=NULL;
    cur->next=c; cur=c; } return h; }
static void freeList(node *h){ while(h){ node *n=h->next; free(h); h=n; } }
static int64_t sumList(node *h){ int64_t t=0; while(h){ t+=h->value; h=h->next; } return t; }
static int64_t deep(int64_t n){ if(n==0) return 0; int64_t b[64]; b[0]=(int64_t)nextBounded(100);
  return b[0]+deep(n-1); }
static void shuffle(int *a,int n){ for(int i=n-1;i>0;i--){ int j=(int)nextBounded((uint64_t)(i+1));
  int t=a[i]; a[i]=a[j]; a[j]=t; } }
int main(void){ seed(20260920,54); int64_t total=0;
  for(int64_t r=0;r<300;r++){ node *l=buildList(2000); total+=sumList(l); freeList(l); }
  printf("① 链表 = %lld\n",(long long)total);
  int a[1024]; for(int i=0;i<1024;i++) a[i]=i; shuffle(a,1024);
  int64_t hits=0; for(int64_t k=0;k<200000;k++) hits += a[nextBounded(1024)];
  printf("② 随机访问 = %lld\n",(long long)hits);
  printf("③ 深递归 = %lld\n",(long long)deep(2000));
  uint8_t *big=malloc(4194304); memset(big,0,4194304);
  for(int64_t b=0;b<100000;b++) big[nextBounded(4194304)]=(uint8_t)nextBounded(256);
  printf("④ 大 buffer 首字节 = %d\n", big[0]); }

/* 决定性对比：冷结构很多（不产垃圾）+ 一个小热区（疯狂产垃圾）。
 *   region  冷结构各自成区、**永不清扫**；只扫热区 ⇒ 停顿 ∝ 热区（与冷数据量无关）
 *   global  一个堆、全部根 ⇒ 每次回收都要追**所有冷结构** ⇒ 停顿 ∝ 全堆
 * 冷数据量由 K 决定；总分配量、热区行为、触发策略完全一致。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <string.h>
typedef struct Obj { uint32_t mark; int64_t v; struct Obj *p0; } Obj;
static int64_t now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000000LL+t.tv_nsec;}
typedef struct { int64_t max_ns,total_ns; size_t n; } P;
static void padd(P*p,int64_t d){if(d>p->max_ns)p->max_ns=d;p->total_ns+=d;p->n++;}

typedef struct RB { struct RB*prev; Obj*o; } RB;
typedef struct { RB*top; size_t since,live,peak; } Region;
static Obj*ralloc(Region*r){Obj*o=calloc(1,sizeof(Obj));RB*b=malloc(sizeof(RB));b->prev=r->top;b->o=o;r->top=b;
  r->since+=sizeof(Obj);r->live+=sizeof(Obj);if(r->live>r->peak)r->peak=r->live;return o;}
static void rrelease(Region*r){while(r->top){RB*b=r->top;r->top=b->prev;r->live-=sizeof(Obj);free(b->o);free(b);}r->since=0;}
static void rsweep(Region*r,Obj**roots,int nr,P*p){
  int64_t t0=now_ns();
  for(RB*b=r->top;b;b=b->prev)b->o->mark=0;
  static Obj*st[1<<22];int sp=0;
  for(int i=0;i<nr;i++)if(roots[i])st[sp++]=roots[i];
  while(sp){Obj*o=st[--sp];if(o->mark)continue;o->mark=1;if(o->p0)st[sp++]=o->p0;}
  RB**l=&r->top; while(*l){RB*b=*l;if(b->o->mark)l=&b->prev;else{*l=b->prev;r->live-=sizeof(Obj);free(b->o);free(b);}}
  r->since=0; padd(p,now_ns()-t0);
}
static Obj**HV;static size_t HN,HCAP,HLIVE,HPEAK,HSINCE,HLIVE_AFTER;
static Obj*galloc(void){Obj*o=calloc(1,sizeof(Obj));if(HN==HCAP){HCAP=HCAP?HCAP*2:1024;HV=realloc(HV,HCAP*sizeof(Obj*));}
  HV[HN++]=o;HLIVE+=sizeof(Obj);if(HLIVE>HPEAK)HPEAK=HLIVE;return o;}
static void gcoll(Obj**roots,int nr,P*p){
  int64_t t0=now_ns();
  for(size_t i=0;i<HN;i++)HV[i]->mark=0;
  static Obj*st[1<<22];int sp=0;
  for(int i=0;i<nr;i++)if(roots[i])st[sp++]=roots[i];
  while(sp){Obj*o=st[--sp];if(o->mark)continue;o->mark=1;if(o->p0)st[sp++]=o->p0;}
  size_t w=0;for(size_t i=0;i<HN;i++){if(HV[i]->mark)HV[w++]=HV[i];else{free(HV[i]);HLIVE-=sizeof(Obj);}}
  HN=w;HSINCE=0;HLIVE_AFTER=HLIVE;padd(p,now_ns()-t0);
}
static long g_sink;
int main(int argc,char**argv){
  const char*mode=argc>1?argv[1]:"region";
  int K=argc>2?atoi(argv[2]):8;
  int L=argc>3?atoi(argv[3]):20000;      /* 每个冷结构的长度 */
  int N=argc>4?atoi(argv[4]):400000;
  size_t FLOOR=16384;
  int gl=!strcmp(mode,"global");

  Region*cold=calloc(K,sizeof(Region));
  Region hot={0}, churn={0};
  Obj**coldroot=calloc(K,sizeof(Obj*));
  Obj*hotroot=NULL,*saved=NULL;
  P p={0};

  /* 建冷结构：K 条链，各 L 个节点（此后不再改动 ⇒ 不产垃圾） */
  for(int k=0;k<K;k++) for(int j=0;j<L;j++){
    if(gl){Obj*o=galloc();o->v=j;o->p0=coldroot[k];coldroot[k]=o;}
    else  {Obj*o=ralloc(&cold[k]);o->v=j;o->p0=coldroot[k];coldroot[k]=o;}
  }
  size_t cold_bytes=(size_t)K*L*sizeof(Obj);

  int64_t t0=now_ns();
  for(int i=0;i<N;i++){
    if(gl){Obj*a=galloc(),*b=galloc(),*c=galloc();a->p0=b;b->p0=c;b->v=i;c->v=i+1;g_sink+=b->v+c->v;}
    else  {Obj*a=ralloc(&churn),*b=ralloc(&churn),*c=ralloc(&churn);a->p0=b;b->p0=c;b->v=i;c->v=i+1;g_sink+=b->v+c->v;}
    if(gl){Obj*e=galloc();e->v=i;saved=e;}
    else  {Obj*e=ralloc(&hot);e->v=i;saved=e;}
    if(gl){Obj*d=galloc();d->p0=hotroot;d->v=i;hotroot=d;}
    else  {Obj*d=ralloc(&hot);d->p0=hotroot;d->v=i;hotroot=d;}
    if(gl){
      HSINCE+=3*sizeof(Obj);
      static Obj*all[8192];int n=0;
      for(int k=0;k<K;k++)all[n++]=coldroot[k];
      all[n++]=hotroot;all[n++]=saved;
      size_t trig=HLIVE_AFTER>FLOOR?HLIVE_AFTER:FLOOR;
      if(HSINCE>=trig) gcoll(all,n,&p);
    } else {
      rrelease(&churn);
      size_t trig=hot.live>FLOOR?hot.live:FLOOR;
      if(hot.since>=trig){Obj*rr[2]={hotroot,saved};rsweep(&hot,rr,2,&p);}
    }
  }
  int64_t dt=now_ns()-t0;
  size_t peak=0,fin=0;
  if(gl){peak=HPEAK;fin=HLIVE;}
  else {for(int k=0;k<K;k++){peak+=cold[k].peak;fin+=cold[k].live;} peak+=hot.peak+churn.peak; fin+=hot.live;}
  printf("%-7s K=%-4d 冷数据=%5zuKiB N=%-7d 耗时=%7.1fms 峰值=%6zuKiB 回收=%-6zu 最大停顿=%9.1fus 停顿总=%8.2fms\n",
         mode,K,cold_bytes/1024,N,dt/1e6,peak/1024,p.n,p.max_ns/1000.0,p.total_ns/1e6);
  return 0;
}

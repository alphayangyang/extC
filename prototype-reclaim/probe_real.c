/* 真实负载 v2：**有界符号表/缓存**（K 桶 × 每桶最多 M 条，满了淘汰尾节点）。
 * 每轮：① 解析 3 个临时节点并遍历 ② 插入（头插） ③ 查询（沿链找 key，命中改值）
 *       ④ 淘汰尾节点（链表手术：prev->next=NULL） ⑤ 每 32 轮桶头前移（换引用）
 * 块头与对象**同一次分配、紧邻**（真实做法）⇒ 对象→块 O(1)。
 *
 * 变体：nocoll / region（淘汰留下垃圾等清扫）/ region+ev（淘汰当场释放块）/ global
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

typedef struct Obj { uint32_t mark; int64_t key, val; struct Obj *next; } Obj;
typedef struct RB { struct RB *next; struct RB **self; Obj *o; } RB;
static RB  *rbof (Obj *o) { return (RB *)((char *)o - sizeof(RB)); }

static int64_t now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000000LL+t.tv_nsec;}
typedef struct { int64_t max_ns,total_ns; size_t n; } P;
static void padd(P*p,int64_t d){if(d>p->max_ns)p->max_ns=d;p->total_ns+=d;p->n++;}

typedef struct { RB *top; size_t since,live,live_after,peak,ev; } Region;
static Obj *ralloc(Region*r){
    RB*b=malloc(sizeof(RB)+sizeof(Obj)); Obj*o=(Obj*)((char*)b+sizeof(RB));
    memset(o,0,sizeof(Obj)); b->o=o; b->next=r->top; b->self=&r->top; if(r->top)r->top->self=&b->next; r->top=b;
    r->since+=sizeof(Obj); r->live+=sizeof(Obj); if(r->live>r->peak)r->peak=r->live; return o;
}
static void rfree(Region*r, Obj*o){ RB*b=rbof(o); *b->self=b->next; if(b->next)b->next->self=b->self;
    r->live-=sizeof(Obj); r->since+=0; r->ev++; free(b); }
static void rrelease(Region*r){ while(r->top){RB*b=r->top;r->top=b->next;if(r->top)r->top->self=&r->top;r->live-=sizeof(Obj);free(b);} r->since=0;r->live_after=0; }
static void rsweep(Region*r, Obj**roots,int nr,P*p){
    int64_t t0=now_ns();
    for(RB*b=r->top;b;b=b->next) b->o->mark=0;
    static Obj*st[1<<20]; int sp=0;
    for(int i=0;i<nr;i++) if(roots[i]) st[sp++]=roots[i];
    while(sp){ Obj*o=st[--sp]; if(o->mark)continue; o->mark=1; if(o->next) st[sp++]=o->next; }
    RB**l=&r->top;
    while(*l){ RB*b=*l; if(b->o->mark){ b->self=l; l=&b->next; } else { *l=b->next; if(b->next)b->next->self=l; r->live-=sizeof(Obj); free(b);} }
    r->since=0; r->live_after=r->live; padd(p,now_ns()-t0);
}
static Obj**HV;static size_t HN,HCAP,HLIVE,HPEAK,HSINCE,HLIVE_AFTER;
static Obj*galloc(void){Obj*o=calloc(1,sizeof(Obj));if(HN==HCAP){HCAP=HCAP?HCAP*2:1024;HV=realloc(HV,HCAP*sizeof(Obj*));}
  HV[HN++]=o;HLIVE+=sizeof(Obj);if(HLIVE>HPEAK)HPEAK=HLIVE;return o;}
static void gcoll(Obj**roots,int nr,P*p){
  int64_t t0=now_ns();
  for(size_t i=0;i<HN;i++)HV[i]->mark=0;
  static Obj*st[1<<20];int sp=0;
  for(int i=0;i<nr;i++)if(roots[i])st[sp++]=roots[i];
  while(sp){Obj*o=st[--sp];if(o->mark)continue;o->mark=1;if(o->next)st[sp++]=o->next;}
  size_t w=0;for(size_t i=0;i<HN;i++){if(HV[i]->mark)HV[w++]=HV[i];else{free(HV[i]);HLIVE-=sizeof(Obj);}}
  HN=w;HSINCE=0;HLIVE_AFTER=HLIVE;padd(p,now_ns()-t0);
}
static long g_sink;
int main(int argc,char**argv){
  const char*mode=argc>1?argv[1]:"region";
  int K=argc>2?atoi(argv[2]):256;
  int M=argc>3?atoi(argv[3]):8;
  int N=argc>4?atoi(argv[4]):400000;
  size_t FLOOR=argc>5?(size_t)atol(argv[5]):4096;
  int gl=!strcmp(mode,"global"), ev=!strcmp(mode,"region+ev");

  Region*buck=calloc(K,sizeof(Region)); Region churn={0};
  Obj**bh=calloc(K,sizeof(Obj*)); Obj*ch=NULL; P p={0};
  size_t ins=0,evict=0,hit=0;

  int64_t t0=now_ns();
  for(int i=0;i<N;i++){
    int k=(i*2654435761u)%K; if(k<0)k+=K;
    /* ① 解析：3 临时 */
    Obj*a,*b,*c;
    if(gl){a=galloc();b=galloc();c=galloc();} else {a=ralloc(&churn);b=ralloc(&churn);c=ralloc(&churn);}
    a->next=b;b->next=c;c->val=i; ch=a; for(Obj*q=a;q;q=q->next) g_sink+=q->val;
    /* ② 插入（头插） */
    Obj*nd=gl?galloc():ralloc(&buck[k]); nd->key=i; nd->val=i*3; nd->next=bh[k]; bh[k]=nd; ins++;
    /* ③ 查询：沿链找 key（链长 ≤ M） */
    int64_t want = (i%3==0 && bh[k]) ? bh[k]->key : (i-7);
    for(Obj*q=bh[k];q;q=q->next) if(q->key==want){ q->val++; hit++; break; }
    /* ④ 淘汰尾节点（链表手术）；ev 模式当场释放块 */
    int n=0; Obj*prev=NULL,*cur=bh[k];
    while(cur){ if(!cur->next) break; prev=cur; cur=cur->next; n++; if(n>M+2) break; }
    if(cur && n>=M){
      if(prev){ prev->next=NULL; } else { bh[k]=NULL; }
      evict++;
      if(ev && !gl) rfree(&buck[k],cur);      /* 事件回收：O(1) */
    }
    /* ⑤ 换引用：桶头前移 */
    if((i%32)==0 && bh[k]) bh[k]=bh[k]->next;
    if(!gl) rrelease(&churn);
    /* 回收 */
    if(gl){
      HSINCE+=4*sizeof(Obj);
      static Obj*all[8192]; int q=0; for(int j=0;j<K;j++) all[q++]=bh[j]; all[q++]=ch;
      size_t trig=HLIVE_AFTER>FLOOR?HLIVE_AFTER:FLOOR;
      if(HSINCE>=trig) gcoll(all,q,&p);
    } else if(strcmp(mode,"nocoll")){
      for(int j=0;j<K;j++){
        size_t trig=buck[j].live_after>FLOOR?buck[j].live_after:FLOOR;
        if(buck[j].since>=trig){ Obj*rr[1]={bh[j]}; rsweep(&buck[j],rr,1,&p); }
      }
    }
  }
  int64_t dt=now_ns()-t0;
  size_t peak=0,fin=0,evd=0;
  if(gl){peak=HPEAK;fin=HLIVE;} else {for(int j=0;j<K;j++){peak+=buck[j].peak;fin+=buck[j].live;evd+=buck[j].ev;} peak+=churn.peak;fin+=churn.live;}
  printf("%-10s K=%-4d M=%d N=%-7d 耗时=%8.1fms 峰值=%6zuKiB 末态=%6zuKiB 回收=%-6zu 最大停顿=%9.1fus 停顿总=%8.2fms 插=%zu 淘汰=%zu 命中=%zu%s%zu\n",
         mode,K,M,N,dt/1e6,peak/1024,fin/1024,p.n,p.max_ns/1000.0,p.total_ns/1e6,ins,evict,hit, ev?" 事件回收=":"", evd);
  return 0;
}

#!/usr/bin/env python3
"""human-probe.py —— 人类形状的**写足迹/变更集/换血率**探针（task-46）。
只插**生成物副本**；口径与 `tests/reclaim-perf/PAGE-DIRTY-RECON.md` §1 **逐条一致**（`digraph` 的 1.38× 就是它量的）：
  · 每次引用写：页号 = `slot>>12`、512B 块号 = `slot>>9`，**周期内去重**；mark 结束时结算；
  · 占用槽来自 slab 的 `occ` 位图；活槽 = `occ & mark`；脏页上占用槽 = 占用槽的页号命中本周期脏页集；
  · **换血率（本件新增）**：对**每一个区域**分别记"本次 mark 的活对象地址集合"，与该区域**上一次 mark** 的集合求交
    ⇒ 报 `交集/活集`（按区域，覆盖**全部区域**，不是只报被收集区域）。
用法：python3 human-probe.py gen.c out.c ；env EXTC_PD=1 EXTC_PD_OUT=前缀
原始出处：/tmp/pagedirty/pd.py（同一作者、同一口径；本文件=它 + 换血率）。
每次 ref 存储：页号 = slot>>12、块号 = slot>>9 ⇒ 周期内去重计数。
每次 mark 后汇总：本周期脏页/脏块数、每页/每块实际槽位数、脏页覆盖率（新死对象所在页）、
"脏页×每页槽位"与"mark 走到的活槽位数"的对照。
用法：python3 pd.py gen.c out.c ；env EXTC_PD=1 EXTC_PD_OUT=前缀
"""
import sys
RT = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define PD_HASH (1u<<20)
static int pd_on=-1; static char pd_prefix[400];
static uint64_t *pd_page, *pd_blk; static int32_t *pd_pgen, *pd_bgen; static int32_t pd_gen=1;
static unsigned long pd_stores, pd_period_stores, pd_dirty_pages, pd_dirty_blks;
static unsigned long pd_ppages, pd_pblks, pd_dead_in_dirty, pd_dead_pages, pd_liveslots;
static unsigned long pd_deadpage_live, pd_deadpage_slots, pd_comp_skip;
static FILE *pd_rows;
static unsigned long hp_inter_sum, hp_live_sum, hp_rows;   /* 换血率（前向声明给 pd_dump 用） */
/* (A) 区域限定写集合：本周期被写的槽地址（mark 时按"是否落在本区域 slab 区间"过滤） */
static uint64_t *pd_ws; static size_t pd_ws_n, pd_ws_cap;
static uint64_t *pd_rpage, *pd_rblk; static int32_t *pd_rpgen, *pd_rbgen;
static unsigned long pd_rdirty_pages, pd_rdirty_blks, pd_r_slots_in_dirty, pd_r_sum_pages, pd_r_sum_blks, pd_r_sum_sid, pd_r_sum_live;
/* (B) 分配纪元：addr -> 纪元号（纪元 = mark 序号） */
static uint64_t *pd_ep_key; static int32_t *pd_ep_val; static int32_t pd_mark_epoch=1, pd_prev_epoch=0;
static unsigned long pd_newlive_sum, pd_newlive_live_sum;
static inline void pd_ep_set(uint64_t k, int32_t v){
    uint32_t h=(uint32_t)((k*2654435761ull)>>12)&(PD_HASH-1);
    for(uint32_t j=0;j<4096;j++){uint32_t i=(h+j)&(PD_HASH-1);
        if(pd_ep_val[i]==0 || pd_ep_key[i]==k){ pd_ep_key[i]=k; pd_ep_val[i]=v+1; return; } }
}
static inline int32_t pd_ep_get(uint64_t k){
    uint32_t h=(uint32_t)((k*2654435761ull)>>12)&(PD_HASH-1);
    for(uint32_t j=0;j<4096;j++){uint32_t i=(h+j)&(PD_HASH-1);
        if(pd_ep_val[i]==0) return 0;
        if(pd_ep_key[i]==k) return pd_ep_val[i]-1; }
    return 0;
}
static inline int pd_hask(uint64_t *tbl, int32_t *gen, uint32_t h, uint64_t key);   /* 前向声明 */
static inline void pd_rnote(uint64_t *tbl,int32_t *gen,unsigned long *cnt,uint64_t key){
    uint32_t h=(uint32_t)((key*2654435761ull)>>12)&(PD_HASH-1);
    if(pd_hask(tbl,gen,h,key)>=0) return;
    for(uint32_t j=0;j<4096;j++){uint32_t i=(h+j)&(PD_HASH-1);
        if(gen[i]!=pd_gen){gen[i]=pd_gen;tbl[i]=key;(*cnt)++;return;}}
}
static void pd_dump(void);
static void pd_arm(void){
    if(pd_on>=0) return; pd_on=getenv("EXTC_PD")?1:0; if(!pd_on) return;
    const char*pp=getenv("EXTC_PD_OUT"); snprintf(pd_prefix,sizeof pd_prefix,"%s",pp?pp:"/tmp/pd");
    pd_page=(uint64_t*)calloc(PD_HASH,8); pd_blk=(uint64_t*)calloc(PD_HASH,8);
    pd_rpage=(uint64_t*)calloc(PD_HASH,8); pd_rblk=(uint64_t*)calloc(PD_HASH,8);
    pd_rpgen=(int32_t*)calloc(PD_HASH,4); pd_rbgen=(int32_t*)calloc(PD_HASH,4);
    pd_ep_key=(uint64_t*)calloc(PD_HASH,8); pd_ep_val=(int32_t*)calloc(PD_HASH,4);
    pd_pgen=(int32_t*)calloc(PD_HASH,4); pd_bgen=(int32_t*)calloc(PD_HASH,4);
    if(!pd_page||!pd_blk||!pd_pgen||!pd_bgen||!pd_rpage||!pd_rblk||!pd_rpgen||!pd_rbgen||!pd_ep_key||!pd_ep_val)
        {fprintf(stderr,"PD-FAIL: 分配失败\n");exit(3);}
    char path[500]; snprintf(path,sizeof path,"%s.rows.csv",pd_prefix);
    pd_rows=fopen(path,"w");
    if(pd_rows) fprintf(pd_rows,"collect,period_stores,dirty_pages,dirty_blks,occslots,liveslots,slots_in_dirty,slots_per_dirty_page,r_dirty_pages,r_dirty_blks,r_slots_in_dirty,r_change_over_live\n");
    atexit(pd_dump);
}
static void pd_dump(void){ static int d; if(!pd_on||d) return; d=1;
    if(pd_rows) fclose(pd_rows);
    fprintf(stderr,"PD stores=%lu dirty_pages=%lu dirty_blks=%lu pages_sum=%lu blks_sum=%lu\n",pd_stores,pd_dirty_pages,pd_dirty_blks,pd_ppages,pd_pblks);
    fprintf(stderr,"PD liveslots=%lu dead_in_dirty=%lu dead_pages=%lu deadpage_live=%lu deadpage_slots=%lu\n",pd_liveslots,pd_dead_in_dirty,pd_dead_pages,pd_deadpage_live,pd_deadpage_slots);
    fprintf(stderr,"PD files=%s.rows.csv\n",pd_prefix);
    fprintf(stderr,"PD-TURN-SUM rows=%lu live_sum=%lu inter_sum=%lu ratio=%.4f\n",hp_rows,hp_live_sum,hp_inter_sum,
            hp_live_sum?(double)hp_inter_sum/(double)hp_live_sum:0.0);
    fprintf(stderr,"PD-REGION-SUM pages=%lu blks=%lu slots_in_dirty=%lu live=%lu change_over_live=%.4f\n",
            pd_r_sum_pages,pd_r_sum_blks,pd_r_sum_sid,pd_r_sum_live,
            pd_r_sum_live?(double)pd_r_sum_sid/(double)pd_r_sum_live:0.0);
    fprintf(stderr,"PD-EPOCH-SUM newlive=%lu live=%lu new_over_live=%.4f\n",pd_newlive_sum,pd_newlive_live_sum,
            pd_newlive_live_sum?(double)pd_newlive_sum/(double)pd_newlive_live_sum:0.0);
}
static inline int pd_hask(uint64_t *tbl, int32_t *gen, uint32_t h, uint64_t key){
    for(uint32_t k=0;k<4096;k++){ uint32_t i=(h+k)&(PD_HASH-1);
        if(gen[i]!=pd_gen) return -1;
        if(tbl[i]==key) return (int)i; }
    return -2;
}
static inline void pd_note(uint64_t key){
    uint32_t h=(uint32_t)((key*2654435761ull)>>12)&(PD_HASH-1);
    int r=pd_hask(pd_page,pd_pgen,h,key);
    if(r>=0) return;
    for(uint32_t k=0;k<4096;k++){ uint32_t i=(h+k)&(PD_HASH-1);
        if(pd_pgen[i]!=pd_gen){ pd_pgen[i]=pd_gen; pd_page[i]=key; pd_dirty_pages++; return; } }
}
static inline void pd_noteB(uint64_t key){
    uint32_t h=(uint32_t)((key*2654435761ull)>>12)&(PD_HASH-1);
    int r=pd_hask(pd_blk,pd_bgen,h,key);
    if(r>=0) return;
    for(uint32_t k=0;k<4096;k++){ uint32_t i=(h+k)&(PD_HASH-1);
        if(pd_bgen[i]!=pd_gen){ pd_bgen[i]=pd_gen; pd_blk[i]=key; pd_dirty_blks++; return; } }
}
static void pd_note_alloc(void *p){
    if(pd_on<0) pd_arm(); if(!pd_on||!p) return;
    pd_ep_set((uint64_t)(uintptr_t)p, pd_mark_epoch);
}
static void pd_store(void **slot){
    if(pd_on<0) pd_arm(); if(!pd_on) return;
    pd_stores++; pd_period_stores++;
    uint64_t a=(uint64_t)(uintptr_t)slot;
    pd_note(a>>12); pd_noteB(a>>9);
    if(pd_ws_n==pd_ws_cap){ size_t nc=pd_ws_cap?pd_ws_cap*2:65536;
        uint64_t *nw=(uint64_t*)realloc(pd_ws,nc*8); if(!nw){fprintf(stderr,"PD-FAIL: ws\n");exit(3);}
        pd_ws=nw; pd_ws_cap=nc; }
    pd_ws[pd_ws_n++]=a;
}
/* ===== 换血率：按区域记相邻两次 mark 的活集交集（全区域） ===== */
typedef struct HpTurn { ExtcRcRegion *r; uint64_t *prev; size_t nprev; struct HpTurn *next; } HpTurn;
static HpTurn *hp_turns;
static int hp_u64cmp(const void *a, const void *b){
    uint64_t x=*(const uint64_t*)a, y=*(const uint64_t*)b; return (x>y)-(x<y);
}
static HpTurn *hp_turn_of(ExtcRcRegion *r){
    for(HpTurn*t=hp_turns;t;t=t->next) if(t->r==r) return t;
    HpTurn*t=(HpTurn*)calloc(1,sizeof *t); if(!t){fprintf(stderr,"PD-FAIL: turn\n");exit(3);}
    t->r=r; t->next=hp_turns; hp_turns=t; return t;
}
static void hp_turn_note(ExtcRcRegion *r, uint64_t *live, size_t n){
    if(!pd_on) return;
    HpTurn*t=hp_turn_of(r);
    qsort(live,n,sizeof(uint64_t),hp_u64cmp);
    size_t i=0,j=0,inter=0;
    while(i<n && j<t->nprev){
        if(live[i]==t->prev[j]){inter++;i++;j++;}
        else if(live[i]<t->prev[j]) i++;
        else j++;
    }
    hp_inter_sum+=inter; hp_live_sum+=n; hp_rows++;
    fprintf(stderr,"PD-TURN region=%p live=%zu inter=%zu ratio=%.4f\n",(void*)r,n,inter,n?(double)inter/(double)n:0.0);
    free(t->prev);
    t->prev=(uint64_t*)malloc((n?n:1)*sizeof(uint64_t));
    if(!t->prev){fprintf(stderr,"PD-FAIL: turn buf\n");exit(3);}
    memcpy(t->prev,live,n*sizeof(uint64_t)); t->nprev=n;
}

static size_t pd_after_mark(ExtcRcRegion *r, void *const *roots, size_t nroots){
    pd_arm(); if(!pd_on) return 0;
    size_t slots=0; for(ExtcRcSlab*s=r->slabs;s;s=s->next) slots+=s->nslots;
    /* 每页槽位数：占用槽按页分桶，统计"脏页里有多少占用槽" */
    unsigned long liveslots=0, occslots=0, slots_in_dirty=0;
    uint64_t *pgv=(uint64_t*)malloc((size_t)(slots?slots:1)*8);
    if(!pgv){fprintf(stderr,"PD-FAIL: pgv\n");exit(3);}
    uint64_t *hplive=(uint64_t*)malloc((size_t)(slots?slots:1)*8);   /* 换血率用：本区域活对象地址 */
    if(!hplive){fprintf(stderr,"PD-FAIL: hplive\n");exit(3);}
    size_t base=0;
    for(ExtcRcSlab*s=r->slabs;s;s=s->next){
        for(uint32_t w=0;w<s->nwords;w++){
            uint64_t b=s->occ[w];
            while(b){uint32_t q=(uint32_t)__builtin_ctzll(b);b&=b-1;
                occslots++;
                void *o=EXTC_RC_SLAB_DATA(s)+(((size_t)w*64+q)*s->ss);
                if(s->mark[w]&(1ull<<q)){ hplive[liveslots]=(uint64_t)(uintptr_t)o; liveslots++; }
                pgv[occslots-1]=(uint64_t)(uintptr_t)o>>12;
            }
        }
        base+=s->nslots;
    }
    for(unsigned long z=0;z<occslots;z++){
        uint32_t h=(uint32_t)((pgv[z]*2654435761ull)>>12)&(PD_HASH-1);
        if(pd_hask(pd_page,pd_pgen,h,pgv[z])>=0) slots_in_dirty++;
    }
    free(pgv);
    /* (A) 区域限定：只留落在**本区域 slab 区间**里的写 */
    pd_rdirty_pages=0; pd_rdirty_blks=0; pd_r_slots_in_dirty=0;
    for(size_t wi=0; wi<pd_ws_n; wi++){
        uint64_t a=pd_ws[wi]; int in=0;
        for(ExtcRcSlab*s2=r->slabs;s2;s2=s2->next){
            uintptr_t lo=(uintptr_t)EXTC_RC_SLAB_DATA(s2);
            uintptr_t hi=lo+(size_t)s2->nslots*(size_t)s2->ss;
            if((uintptr_t)a>=lo && (uintptr_t)a<hi){ in=1; break; }
        }
        if(!in) continue;
        pd_rnote(pd_rpage,pd_rpgen,&pd_rdirty_pages,a>>12);
        pd_rnote(pd_rblk,pd_rbgen,&pd_rdirty_blks,a>>9);
    }
    for(ExtcRcSlab*s3=r->slabs;s3;s3=s3->next){
        for(uint32_t w=0;w<s3->nwords;w++){
            uint64_t b=s3->occ[w];
            while(b){uint32_t q=(uint32_t)__builtin_ctzll(b);b&=b-1;
                void *o=EXTC_RC_SLAB_DATA(s3)+(((size_t)w*64+q)*s3->ss);
                uint32_t h=(uint32_t)((((uint64_t)(uintptr_t)o>>12)*2654435761ull)>>12)&(PD_HASH-1);
                if(pd_hask(pd_rpage,pd_rpgen,h,(uint64_t)(uintptr_t)o>>12)>=0) pd_r_slots_in_dirty++;
            }
        }
    }
    pd_r_sum_pages+=pd_rdirty_pages; pd_r_sum_blks+=pd_rdirty_blks;
    pd_r_sum_sid+=pd_r_slots_in_dirty; pd_r_sum_live+=liveslots;
    /* (B) 分配纪元：活槽里"分配于上一纪元之后"的占比 */
    unsigned long newlive=0, liveN=0;
    for(ExtcRcSlab*s4=r->slabs;s4;s4=s4->next){
        for(uint32_t w=0;w<s4->nwords;w++){
            uint64_t b=s4->mark[w];
            while(b){uint32_t q=(uint32_t)__builtin_ctzll(b);b&=b-1;
                void *o=EXTC_RC_SLAB_DATA(s4)+(((size_t)w*64+q)*s4->ss);
                liveN++;
                int32_t e=pd_ep_get((uint64_t)(uintptr_t)o);
                if(e>pd_prev_epoch) newlive++;
            }
        }
    }
    pd_newlive_sum+=newlive; pd_newlive_live_sum+=liveN;
    fprintf(stderr,"PD-EPOCH mark=%d region=%p live=%lu newlive=%lu ratio=%.4f\n",pd_mark_epoch,(void*)r,liveN,newlive,
            liveN?(double)newlive/(double)liveN:0.0);
    pd_prev_epoch=pd_mark_epoch; pd_mark_epoch++;
    pd_ws_n=0;
    hp_turn_note(r, hplive, (size_t)liveslots);
    free(hplive);
    /* 新死对象：进入 mark 前标记、现在未标记（用 dc 快照不可得 ⇒ 用 mark 位与 occ 之差做近似：
       本周期"死"= occupid 且未标记；与脏页对照仍成立） */
    unsigned long dead_in_dirty=0, dead_pages=0, dp_live=0, dp_slots=0;
    base=0;
    for(ExtcRcSlab*s=r->slabs;s;s=s->next){
        for(uint32_t w=0;w<s->nwords;w++){
            uint64_t b=s->occ[w]&~s->mark[w];
            while(b){uint32_t q=(uint32_t)__builtin_ctzll(b);b&=b-1;
                void *o=EXTC_RC_SLAB_DATA(s)+(((size_t)w*64+q)*s->ss);
                uint64_t pg=(uint64_t)(uintptr_t)o>>12;
                /* 脏页查找 */
                uint32_t h=(uint32_t)((pg*2654435761u)>>12)&(PD_HASH-1); int found=0;
                for(uint32_t k=0;k<4096;k++){uint32_t i=(h+k)&(PD_HASH-1);
                    if(pd_pgen[i]!=pd_gen) break;
                    if(pd_page[i]==pg){found=1;break;}}
                if(found) dead_in_dirty++; else dead_pages++;
            }
        }
        base+=s->nslots;
    }
    pd_liveslots+=liveslots;
    pd_dead_in_dirty+=dead_in_dirty; pd_dead_pages+=dead_pages;
    pd_ppages+=pd_dirty_pages; pd_pblks+=pd_dirty_blks;
    if(pd_rows) fprintf(pd_rows,"%d,%lu,%lu,%lu,%lu,%lu,%lu,%.2f,%.0f,%.0f,%lu,%.4f\n",pd_gen,pd_period_stores,pd_dirty_pages,pd_dirty_blks,
        occslots,liveslots,slots_in_dirty, pd_dirty_pages?(double)slots_in_dirty/(double)pd_dirty_pages:0.0,
        (double)pd_rdirty_pages,(double)pd_rdirty_blks,pd_r_slots_in_dirty,
        liveslots?(double)pd_r_slots_in_dirty/(double)liveslots:0.0);
    pd_period_stores=0; pd_dirty_pages=0; pd_dirty_blks=0;
    pd_gen++;
    return 0;
}
'''
A_ALLOC="__attribute__((unused)) static void *extc_rc_alloc(ExtcRcRegion *r, size_t n, const ExtcRcType *td) {"
A_MARK="__attribute__((unused)) static size_t extc_rc_mark(ExtcRcRegion *r, void *const *roots, size_t nroots) {"
A_RET="    return marked;\n}"
STORES=[("__attribute__((unused)) static void extc_rc_initRef(void **slot, void *v) {","pd_store(slot);"),
 ("__attribute__((unused)) static void extc_rc_initRefNoHold(void **slot, void *v) {","pd_store(slot);"),
 ("__attribute__((unused)) static void extc_rc_storeRef(void **slot, void *v) {","pd_store(slot);"),
 ("__attribute__((unused)) static inline void extc_rc_storeRefOwned(void **slot, void *v, void *owner) {","pd_store(slot);")]
def main():
    src,dst=sys.argv[1],sys.argv[2]
    t=open(src,encoding='utf-8',errors='surrogateescape').read()
    for a in (A_ALLOC,A_MARK,A_RET)+tuple(s[0] for s in STORES):
        if t.count(a)!=1: print("pd: 锚点不唯一 "+a[:60],file=sys.stderr); return 2
    t=t.replace(A_ALLOC,RT+"\n"+A_ALLOC,1)
    t=t.replace(A_RET,"    pd_after_mark(r, roots, nroots);\n"+A_RET,1)
    for a,c in STORES: t=t.replace(a,a+"\n    "+c,1)
    open(dst,'w',encoding='utf-8',errors='surrogateescape').write(t)
    print("pd: 已插桩 "+src,file=sys.stderr); return 0
sys.exit(main())

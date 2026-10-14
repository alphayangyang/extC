#!/usr/bin/env python3
"""tools/rc-live-components.py —— 活集**弱连通分量** + 相邻两次 `collect` 的**标记周转率**（只勘察）。

用法：
    python3 tools/rc-live-components.py gen.c > gen.lc.c
    cc -std=c11 -fwrapv -O2 gen.lc.c -o prog
    EXTC_LIVE_COMP=1 EXTC_LIVE_COMP_OUT=/tmp/lc ./prog 1000000 20261013

每次 `mark` 结束后（= 每次 `collect` 的活集已定）在**副本**里算：

1. **活集 WCC**：标记集（= 本区从种子可达的闭包）上的弱连通分量，沿 `ExtcRcType{nptr, off}`
   走（与运行期同一条走法）。给分量数、最大分量、**最大分量占活集比例**、以及去掉最大分量后的分布
   ⇒ 回答"**能不能按引用闭包切**"。
2. **周转率**：与**上一次 `collect` 的标记集**比：`交集` / `本次新增`（mark_{n+1}\\mark_n）/
   `本次消失`（mark_n\\mark_{n+1}）；另记**两次之间新分配的对象数** ⇒ 回答"**能不能只重扫增量**"。

判据（牙，不静默给 0）：
  * `Σ 各分量对象数 == marked`（每个活对象恰好属于一个分量）；
  * 每个分量的对象数 ≥ 1；分量分析结束后**没有未分配**的活对象；
  * 关掉 `EXTC_LIVE_COMP` 时一行不输出、一次扫描都不多做。

只写 stderr / `EXTC_LIVE_COMP_OUT` 前缀的文件；stdout 一行不动；不碰发射器（产物逐字节不变）。
"""
import argparse
import sys

RUNTIME = r'''
/* ================= 活集 WCC + 周转率（tools/rc-live-components.py 插入） ================= */
#define LCOMP_MAX 8192
static int    extc_lc_on = -1;
static FILE  *extc_lc_f = 0;
static char   extc_lc_prefix[512];
static void extc_lc_dump(void);   /* 前向声明：arm 里挂 atexit */
static long long extc_lc_allocs;                 /* 分配计数（每次 collect 取增量） */
static long long extc_lc_prev_allocs;
/* 逐次 */
static size_t extc_lc_rows;
static long long lc_marked[LCOMP_MAX], lc_wccn[LCOMP_MAX], lc_wccmax[LCOMP_MAX];
static long long lc_slots[LCOMP_MAX], lc_inter[LCOMP_MAX], lc_added[LCOMP_MAX], lc_removed[LCOMP_MAX];
static long long lc_prevmarked[LCOMP_MAX], lc_allocsince[LCOMP_MAX];
/* 上一次 collect 的标记位图：按 slab 指针存一份副本 */
typedef struct ExtcLcPrev { ExtcRcSlab *s; uint64_t *bits; struct ExtcLcPrev *next; } ExtcLcPrev;
static ExtcLcPrev *extc_lc_prev;
static unsigned char *extc_lc_seen;
static size_t extc_lc_seen_cap;
static void *extc_lc_stk;
static size_t extc_lc_stkcap;
static size_t extc_lc_wcc_written;

static void extc_lc_fail(const char *why) {
    fprintf(stderr, "LIVECOMP-FAIL: %s\n", why);
    exit(3);
}
static void extc_lc_arm(void) {
    if (extc_lc_on >= 0) return;
    extc_lc_on = getenv("EXTC_LIVE_COMP") ? 1 : 0;
    if (extc_lc_on != 1) return;
    const char *pp = getenv("EXTC_LIVE_COMP_OUT");
    snprintf(extc_lc_prefix, sizeof extc_lc_prefix, "%s", pp ? pp : "/tmp/extc-live-comp");
    extc_lc_f = stderr;
    atexit(extc_lc_dump);
}
static uint64_t *extc_lc_prev_of(ExtcRcSlab *s) {
    for (ExtcLcPrev *p = extc_lc_prev; p; p = p->next) if (p->s == s) return p->bits;
    return 0;
}
static void extc_lc_snapshot(ExtcRcRegion *r) {
    for (ExtcRcSlab *s = r->slabs; s; s = s->next) {
        uint64_t *bits = extc_lc_prev_of(s);
        if (!bits) {
            ExtcLcPrev *p = (ExtcLcPrev *)calloc(1, sizeof *p);
            if (!p) extc_lc_fail("prev 节点分配失败");
            p->s = s;
            p->bits = (uint64_t *)calloc(s->nwords ? s->nwords : 1, sizeof(uint64_t));
            if (!p->bits) extc_lc_fail("prev 位图分配失败");
            p->next = extc_lc_prev;
            extc_lc_prev = p;
            bits = p->bits;
        }
        memcpy(bits, s->mark, (size_t)s->nwords * sizeof(uint64_t));
    }
}
/* 标记集上的弱连通分量（沿类型描述符走；活对象为结点，边两端都必须是活对象） */
static void extc_lc_wcc(ExtcRcRegion *r, FILE *fw) {
    size_t slots = 0;
    for (ExtcRcSlab *s = r->slabs; s; s = s->next) slots += s->nslots;
    if (slots > extc_lc_seen_cap) {
        free(extc_lc_seen);
        extc_lc_seen = (unsigned char *)calloc(slots, 1);
        if (!extc_lc_seen) extc_lc_fail("seen 缓冲分配失败");
        extc_lc_seen_cap = slots;
    } else {
        memset(extc_lc_seen, 0, slots);
    }
    if (slots + 16 > extc_lc_stkcap) {
        free(extc_lc_stk);
        extc_lc_stk = malloc((slots + 16) * sizeof(void *));
        if (!extc_lc_stk) extc_lc_fail("栈分配失败");
        extc_lc_stkcap = slots + 16;
    }
    void **stk = (void **)extc_lc_stk;
    long long marked = 0, wccn = 0, wccmax = 0, sumwcc = 0, slotmarked = 0;
    for (ExtcRcSlab *s = r->slabs; s; s = s->next)
        for (uint32_t w = 0; w < s->nwords; w++)
            marked += (long long)__builtin_popcountll(s->mark[w] & s->occ[w]);
    /* 扁平槽号 → 分量遍历 */
    size_t base = 0;
    for (ExtcRcSlab *s = r->slabs; s; s = s->next) {
        for (uint32_t w = 0; w < s->nwords; w++) {
            uint64_t bits = s->mark[w] & s->occ[w];
            while (bits) {
                uint32_t b = (uint32_t)__builtin_ctzll(bits);
                bits &= bits - 1;
                size_t gi = base + (size_t)w * 64 + b;
                if (extc_lc_seen[gi]) continue;
                extc_lc_seen[gi] = 1;
                long long comp = 1;
                long long compbytes = (long long)s->ss;
                size_t n = 0;
                stk[n++] = EXTC_RC_SLAB_DATA(s) + (((size_t)w * 64 + b) * s->ss);
                while (n) {
                    void *o = stk[--n];
                    ExtcRcSlab *os = extc_rc_ownSlab(o);
                    if (!os) extc_lc_fail("分量遍历拿到非 rc 指针（不可能：只从活槽来）");
                    size_t oi = extc_rc_slotOf(os, o);
                    const ExtcRcType *td = extc_rc_typeOf(os, oi);
                    size_t nch = 0;
                    void *ch[64];
                    if (td == EXTC_RC_CONSERVATIVE) {
                        size_t words = os->ss / sizeof(void *);
                        for (size_t q = 0; q < words && nch < 64; q++) {
                            void *c = ((void **)o)[q];
                            if (c) ch[nch++] = c;
                        }
                    } else if (td) {
                        for (uint32_t q = 0; q < td->nptr && nch < 64; q++) {
                            void *c = *(void **)((char *)o + td->off[q]);
                            if (c) ch[nch++] = c;
                        }
                    }
                    for (size_t q = 0; q < nch; q++) {
                        ExtcRcSlab *cs = extc_rc_ownSlab(ch[q]);
                        if (!cs || cs->region != r) continue;       /* 区域外：不是本区结点 */
                        size_t ci = extc_rc_slotOf(cs, ch[q]);
                        uint64_t cb = extc_rc_bit(ci);
                        if (!(cs->occ[extc_rc_word(ci)] & cb)) continue;   /* 空闲：不是结点 */
                        if (!(cs->mark[extc_rc_word(ci)] & cb)) continue;  /* 未标记：不是活结点 */
                        size_t cg = 0; int found = 0;
                        /* 定位它在扁平表里的位置：按 slab 链再走一遍（slab 数 ≤ 十几个） */
                        { size_t bb = 0;
                          for (ExtcRcSlab *q2 = r->slabs; q2; q2 = q2->next) {
                              if (q2 == cs) { cg = bb + ci; found = 1; break; }
                              bb += q2->nslots;
                          } }
                        if (!found) extc_lc_fail("分量里找不到目标 slab");
                        if (extc_lc_seen[cg]) continue;
                        /* 断言：这条边的两端都标记了，且目标属于本区（不然它不该被算进闭包） */
                        if (!(cs->mark[extc_rc_word(ci)] & cb)) extc_lc_fail("边指向未标记对象");
                        extc_lc_seen[cg] = 1;
                        comp++;
                        compbytes += (long long)cs->ss;
                        stk[n++] = ch[q];
                    }
                }
                wccn++;
                sumwcc += comp;
                if (comp > wccmax) wccmax = comp;
                if (fw) fprintf(fw, "%zu,%lld,%lld\n", extc_lc_rows, comp, compbytes);
            }
        }
        base += s->nslots;
    }
    if (sumwcc != marked) extc_lc_fail("Σ 分量对象数 != marked 数（分量没盖满活集）");
    if (wccn == 0 && marked != 0) extc_lc_fail("活集非空却没有分量");
    if (extc_lc_rows < LCOMP_MAX) {
        lc_marked[extc_lc_rows] = marked;
        lc_wccn[extc_lc_rows] = wccn;
        lc_wccmax[extc_lc_rows] = wccmax;
        lc_slots[extc_lc_rows] = (long long)slots;
    }
    (void)slotmarked;
}
static void extc_lc_after_mark(ExtcRcRegion *r) {
    extc_lc_arm();
    if (extc_lc_on != 1) return;
    static FILE *fw;
    if (!fw) {
        char path[600];
        snprintf(path, sizeof path, "%s.wcc.csv", extc_lc_prefix);
        fw = fopen(path, "w");
        if (fw) fprintf(fw, "collect,wcc_size,wcc_bytes\n");
    }
    extc_lc_wcc(r, extc_lc_rows < LCOMP_MAX ? fw : 0);
    /* 周转率：与上一次标记位图比 */
    long long inter = 0, added = 0, removed = 0, prevm = 0;
    { int have_prev = extc_lc_prev != 0;
      for (ExtcRcSlab *s = r->slabs; s; s = s->next) {
          uint64_t *pb = have_prev ? extc_lc_prev_of(s) : 0;
          for (uint32_t w = 0; w < s->nwords; w++) {
              uint64_t cur = s->mark[w] & s->occ[w];
              uint64_t pv = pb ? (pb[w] & s->occ[w]) : 0;
              inter += (long long)__builtin_popcountll(cur & pv);
              added += (long long)__builtin_popcountll(cur & ~pv);
              removed += (long long)__builtin_popcountll(pv & ~cur);
              prevm += (long long)__builtin_popcountll(pv);
          }
      }
    }
    if (extc_lc_rows < LCOMP_MAX) {
        lc_inter[extc_lc_rows] = inter; lc_added[extc_lc_rows] = added;
        lc_removed[extc_lc_rows] = removed; lc_prevmarked[extc_lc_rows] = prevm;
        lc_allocsince[extc_lc_rows] = extc_lc_allocs - extc_lc_prev_allocs;
    }
    extc_lc_snapshot(r);
    extc_lc_prev_allocs = extc_lc_allocs;
    extc_lc_rows++;
}
'''

A_MARKRET = "    return marked;\n}"
A_MARKPUSH = "__attribute__((unused)) static void *extc_rc_alloc(ExtcRcRegion *r, size_t n, const ExtcRcType *td) {"
A_ALLOC = "    EXTC_RC_TICK(0);   /* 分项计时（`EXTC_RC_TIME=1`） */\n    extc_rc_notePart(r);"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--out", default="-")
    a = ap.parse_args()
    t = open(a.src, encoding="utf-8", errors="surrogateescape").read()
    if t.count(A_MARKRET) != 1:
        print("rc-live-components: 找不到唯一的 mark 返回点", file=sys.stderr)
        return 2
    if t.count(A_MARKPUSH) != 1:
        print("rc-live-components: 找不到唯一的 alloc 签名", file=sys.stderr)
        return 2
    if t.count(A_ALLOC) != 1:
        print("rc-live-components: 找不到唯一的 alloc 入口", file=sys.stderr)
        return 2
    # dump（用 RUNTIME 里声明的数组，追加在 RUNTIME 末尾之前不方便 ⇒ 单独拼一段）
    dump = r'''
static void extc_lc_dump(void) {
    if (extc_lc_on != 1) return;
    if (extc_lc_f) {
        char path[600];
        snprintf(path, sizeof path, "%s.rows.csv", extc_lc_prefix);
        FILE *fr = fopen(path, "w");
        if (fr) {
            fprintf(fr, "collect,marked,wcc_n,wcc_max,slots,inter,added,removed,prev_marked,allocs_since\n");
            for (size_t i = 0; i < extc_lc_rows && i < LCOMP_MAX; i++)
                fprintf(fr, "%zu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", i, lc_marked[i],
                        lc_wccn[i], lc_wccmax[i], lc_slots[i], lc_inter[i], lc_added[i],
                        lc_removed[i], lc_prevmarked[i], lc_allocsince[i]);
            fclose(fr);
        }
        long long tot_marked = 0, tot_inter = 0, tot_added = 0, tot_removed = 0, tot_alloc = 0;
        long long maxwcc_share_max = 0;
        for (size_t i = 0; i < extc_lc_rows && i < LCOMP_MAX; i++) {
            tot_marked += lc_marked[i]; tot_inter += lc_inter[i]; tot_added += lc_added[i];
            tot_removed += lc_removed[i]; tot_alloc += lc_allocsince[i];
            if (lc_marked[i] > 0 && lc_wccmax[i] * 1000 / lc_marked[i] > maxwcc_share_max)
                maxwcc_share_max = lc_wccmax[i] * 1000 / lc_marked[i];
        }
        fprintf(extc_lc_f, "LIVECOMP collects=%zu marked_sum=%lld inter=%lld added=%lld removed=%lld "
                "allocs_between=%lld max_wcc_share_permille_max=%lld\n",
                extc_lc_rows, tot_marked, tot_inter, tot_added, tot_removed, tot_alloc, maxwcc_share_max);
        fprintf(extc_lc_f, "LIVECOMP files=%s.{rows,wcc}.csv\n", extc_lc_prefix);
    }
}
'''
    t = t.replace(A_MARKPUSH, RUNTIME + dump + "\n" + A_MARKPUSH, 1)
    t = t.replace(A_MARKRET, "    extc_lc_after_mark(r);\n    return marked;\n}", 1)
    t = t.replace(A_ALLOC, "    extc_lc_allocs++;\n" + A_ALLOC, 1)
    out = open(a.out, "w", encoding="utf-8", errors="surrogateescape") if a.out != "-" else sys.stdout
    out.write(t)
    if out is not sys.stdout:
        out.close()
    print("rc-live-components: 已插桩", a.src, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

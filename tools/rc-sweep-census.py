#!/usr/bin/env python3
"""tools/rc-sweep-census.py —— 清扫**结构量**普查（只动生成物副本，不碰发射器）。

用法：
    python3 tools/rc-sweep-census.py --gid-off 8 --gpin-off 72 gen.c > gen.cen.c
    cc -std=c11 -fwrapv -O2 gen.cen.c -o prog
    EXTC_SWEEP_CENSUS=1 EXTC_SWEEP_CENSUS_OUT=/tmp/cen ./prog 250000 20261013

量的是（全部 env 门控；不设 `EXTC_SWEEP_CENSUS` 时一行都不输出）：
  1. 每次清扫：**扫过多少槽**（Σ slab 的 nslots）、**进入时占用多少个槽**（Σ popcount(occ)）、
     **死集多少个槽**（occ & ~mark）、**freeSlot 调用了多少次**、slab 数；逐次落盘。
  2. 死集的**连通分量**尺寸分布（沿类型描述符走指针，与运行期的 `mark`/`releaseOld` 同一条走法）
     —— 这是"**按结构整片归还**"在结构上的**上限**：一次清扫里成片死掉的东西有多大。
  3. 按**程序自己的事实**（`digraph` 的 `node.gid` / `node.gpin`，偏移由命令行给）分类被释放对象：
     `gpin == 0` = "本组无跨组入边" ⇒ 程序自己就判它**独占死**；给出**①/gpin 可覆盖比例**
     （按对象数、按扫过槽数两个口径）。
  4. 两版分区粒度的**区域尺寸分布**：`每批/每生命期` = 每次清扫释放的那一批；`每组一个区` = 每个
     `gid` 组。落盘成 CSV，供上层算分布与投影。

牙（`--` 说明）：分类字段读错（偏移不对）⇒ 取值不合法就 **exit 3**，绝不静默给 0；
另外每条都被断言：`freed ⊆ 死集`、`freeSlot 次数 == 死集槽数`。

口径说明：本工具**不改编译器**（产物逐字节不变由 `cmp` 证明），只在**副本**上插桩；
所有输出走 stderr（或 `EXTC_SWEEP_CENSUS_OUT` 指定的文件），stdout 一行不动。
"""
import argparse
import sys

RUNTIME = r'''
#include <time.h>   /* clock_gettime：本工具要量时长 */
/* ================= 清扫结构普查（tools/rc-sweep-census.py 插入） =================
 * 只在 EXTC_SWEEP_CENSUS=1 时记账/分析；不设时一行不输出、也不做分量分析。
 * 全部输出走 stderr 或 EXTC_SWEEP_CENSUS_OUT 前缀的文件；stdout 一行不动。 */
#define CEN_MAXROWS 8192
static int    extc_cen_on = -1, extc_cen_struct = 0;
static FILE  *extc_cen_f = 0;
static char   extc_cen_prefix[512];
static size_t extc_cen_rows, extc_cen_sweeps_total;
static size_t extc_cen_slots_scan[CEN_MAXROWS], extc_cen_occ[CEN_MAXROWS];
static size_t extc_cen_dead[CEN_MAXROWS], extc_cen_freed[CEN_MAXROWS];
static size_t extc_cen_nslabs[CEN_MAXROWS];
/* 死集连通分量（结构量的"成片死掉"上限）：尺寸桶 + 精确前 4096 个 */
static long long extc_cen_comp_bucket[40];
static size_t    extc_cen_comp_exact[4096];
static size_t    extc_cen_comp_n, extc_cen_comp_over, extc_cen_comp_total;
static long long extc_cen_comp_obj_total, extc_cen_comp_obj_bucket[40];
/* gid 组统计（只统计被释放的对象） */
static int64_t  *extc_cen_gcnt = 0, *extc_cen_gext = 0, *extc_cen_gbytes = 0;
static size_t    extc_cen_gcap = 0;
static long long extc_cen_freed_all, extc_cen_freed_gpin0, extc_cen_freed_gpin1, extc_cen_freed_node;
static long long extc_cen_slots_gpin0, extc_cen_slots_gpin1;
static int       extc_cen_in_sweep = 0;
static size_t    extc_cen_dead_expect = 0, extc_cen_freed_now = 0;
static int       extc_cen_gid_off = 8, extc_cen_gpin_off = 72;
static int       extc_cen_node_ss = 80;   /* 只有这个尺寸类（node）有 gid/gpin 字段 */
static unsigned char *extc_cen_seen = 0;
static size_t    extc_cen_seen_cap = 0;
static void extc_cen_dump(void);   /* 前向声明：arm 里要挂 atexit */
/* ---- 时长（与 rc-pause-probe 同口径：clock_gettime(CLOCK_MONOTONIC) 的进→出墙钟）---- */
static long long extc_cen_sweep_ns[CEN_MAXROWS];
static long long extc_cen_mark_ns[4096];
static size_t    extc_cen_mark_n;
static long long extc_cen_ns(void) {
    struct timespec t;
    t.tv_sec = 0; t.tv_nsec = 0;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static int extc_cen_cmp(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static long long extc_cen_sum(const long long *v, size_t n) {
    long long s = 0;
    for (size_t i = 0; i < n; i++) s += v[i];
    return s;
}
static void extc_cen_q(const char *tag, long long *v, size_t n) {
    if (!n) return;
    qsort(v, n, sizeof v[0], extc_cen_cmp);
    fprintf(extc_cen_f, "CENSUS-time %s n=%zu p50=%lld p90=%lld p99=%lld max=%lld sum=%lld\n",
            tag, n, v[n / 2],
            v[(size_t)(n * 9 / 10) < n ? (size_t)(n * 9 / 10) : n - 1],
            v[(size_t)(n * 99 / 100) < n ? (size_t)(n * 99 / 100) : n - 1],
            v[n - 1], extc_cen_sum(v, n));
}

static void extc_cen_fail(const char *why) {
    fprintf(stderr, "CENSUS-FAIL: %s\n", why);
    exit(3);
}

static void extc_cen_arm(void) {
    if (extc_cen_on >= 0) return;
    extc_cen_struct = getenv("EXTC_SWEEP_CENSUS") ? 1 : 0;
    extc_cen_on = (extc_cen_struct || getenv("EXTC_SWEEP_TIME")) ? 1 : 0;
    if (!extc_cen_on) return;   /* 两种模式都装 dump：只量时长时也要出时间分位 */
    const char *pp = getenv("EXTC_SWEEP_CENSUS_OUT");
    snprintf(extc_cen_prefix, sizeof extc_cen_prefix, "%s",
             pp ? pp : "/tmp/extc-sweep-census");
    extc_cen_f = stderr;
    atexit(extc_cen_dump);
}

/* 扁平槽号：slab 在 sarr 里的序号 + 该 slab 内槽号 */
static size_t extc_cen_flat(ExtcRcSlab **sarr, size_t *off, size_t nsl, ExtcRcSlab *s, size_t i) {
    for (size_t q = 0; q < nsl; q++) if (sarr[q] == s) return off[q] + i;
    return (size_t)-1;
}

/* 死集 = occ & ~mark（进入清扫时 mark 还在）。返回死集槽数。 */
static size_t extc_cen_begin(ExtcRcRegion *r) {
    extc_cen_arm();
    if (!extc_cen_on) { extc_cen_in_sweep = 0; return 0; }
    if (!extc_cen_struct) {   /* 只量时长：一次扫描都不多做（探针开销 = 两次 clock_gettime）*/
        extc_cen_in_sweep = 1;
        extc_cen_dead_expect = 0;
        extc_cen_freed_now = 0;
        return 0;
    }
    size_t slots = 0, occ = 0, dead = 0, nsl = 0;
    for (ExtcRcSlab *s = r->slabs; s; s = s->next) {
        nsl++; slots += s->nslots;
        for (uint32_t w = 0; w < s->nwords; w++) {
            occ += (size_t)__builtin_popcountll(s->occ[w]);
            dead += (size_t)__builtin_popcountll(s->occ[w] & ~s->mark[w]);
        }
    }
    if (extc_cen_rows < CEN_MAXROWS) {
        extc_cen_slots_scan[extc_cen_rows] = slots;
        extc_cen_occ[extc_cen_rows] = occ;
        extc_cen_dead[extc_cen_rows] = dead;
        extc_cen_nslabs[extc_cen_rows] = nsl;
    }
    /* ---- 死集连通分量（只在结构量模式跑；只量时长时跳过，避免污染时长）---- */
    if (dead && extc_cen_struct) {
        if (slots > extc_cen_seen_cap) {
            free(extc_cen_seen);
            extc_cen_seen = (unsigned char *)calloc(slots, 1);
            if (!extc_cen_seen) extc_cen_fail("seen 缓冲分配失败");
            extc_cen_seen_cap = slots;
        } else {
            memset(extc_cen_seen, 0, slots);
        }
        size_t *off = (size_t *)malloc((nsl + 1) * sizeof(size_t));
        ExtcRcSlab **sarr = (ExtcRcSlab **)malloc((nsl ? nsl : 1) * sizeof(ExtcRcSlab *));
        void **stk = (void **)malloc((dead + 16) * sizeof(void *));
        if (!off || !sarr || !stk) extc_cen_fail("分量分析缓冲分配失败");
        size_t ix = 0;
        for (ExtcRcSlab *s = r->slabs; s; s = s->next) {
            sarr[ix] = s;
            off[ix] = ix ? off[ix - 1] + sarr[ix - 1]->nslots : 0;
            ix++;
        }
        for (size_t si = 0; si < nsl; si++) {
            ExtcRcSlab *s = sarr[si];
            for (uint32_t w = 0; w < s->nwords; w++) {
                uint64_t dd = s->occ[w] & ~s->mark[w];
                while (dd) {
                    uint32_t b = (uint32_t)__builtin_ctzll(dd);
                    dd &= dd - 1;
                    size_t gi = off[si] + (size_t)w * 64 + b;
                    if (extc_cen_seen[gi]) continue;
                    extc_cen_seen[gi] = 1;
                    long long comp = 1;
                    size_t n = 0;
                    stk[n++] = EXTC_RC_SLAB_DATA(s) + (((size_t)w * 64 + b) * s->ss);
                    while (n) {
                        void *o = stk[--n];
                        ExtcRcSlab *os = extc_rc_ownSlab(o);
                        if (!os || os->region != r) continue;
                        size_t oi = extc_rc_slotOf(os, o);
                        if (!(os->occ[extc_rc_word(oi)] & extc_rc_bit(oi))) continue;
                        if (os->mark[extc_rc_word(oi)] & extc_rc_bit(oi)) continue;  /* 活对象不算这一片 */
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
                            if (!cs || cs->region != r) continue;
                            size_t ci = extc_rc_slotOf(cs, ch[q]);
                            if (!(cs->occ[extc_rc_word(ci)] & extc_rc_bit(ci))) continue;
                            if (cs->mark[extc_rc_word(ci)] & extc_rc_bit(ci)) continue;
                            size_t cg = extc_cen_flat(sarr, off, nsl, cs, ci);
                            if (cg == (size_t)-1 || extc_cen_seen[cg]) continue;
                            extc_cen_seen[cg] = 1;
                            comp++;
                            stk[n++] = ch[q];
                        }
                    }
                    size_t bk = 0, v = (size_t)comp;
                    while (v > 1 && bk < 39) { v = (v + 1) >> 1; bk++; }
                    extc_cen_comp_bucket[bk]++;
                    extc_cen_comp_obj_bucket[bk] += comp;
                    extc_cen_comp_obj_total += comp;
                    extc_cen_comp_total++;
                    if (extc_cen_comp_n < 4096) extc_cen_comp_exact[extc_cen_comp_n++] = (size_t)comp;
                    else extc_cen_comp_over++;
                }
            }
        }
        free(stk); free(sarr); free(off);
    }
    extc_cen_in_sweep = 1;
    extc_cen_dead_expect = dead;
    extc_cen_freed_now = 0;
    return dead;
}

/* 每个被释放的对象：分类（只在清扫期间；freeSlot 也被别处调用，别处一律不记） */
static void extc_cen_free(ExtcRcSlab *s, char *p) {
    if (!extc_cen_on || !extc_cen_in_sweep || !extc_cen_struct) return;
    size_t i = extc_rc_slotOf(s, p);
    if (!(s->occ[extc_rc_word(i)] & extc_rc_bit(i))) extc_cen_fail("freeSlot 释放了一个非占用槽");
    if (s->mark[extc_rc_word(i)] & extc_rc_bit(i)) extc_cen_fail("freeSlot 释放了一个被标记（活）的槽");
    extc_cen_freed_all++;
    extc_cen_freed_now++;
    if (!extc_cen_struct) return;
    if ((int)s->ss != extc_cen_node_ss) return;   /* 其它尺寸类没有 gid/gpin 布局，只计总数 */
    extc_cen_freed_node++;
    int64_t gid = 0;
    unsigned char gp = 0;
    memcpy(&gid, p + extc_cen_gid_off, sizeof gid);
    gp = (unsigned char)p[extc_cen_gpin_off];
    if (gp != 0 && gp != 1)
        extc_cen_fail("gpin 字段不是 0/1 —— 偏移不对（读到了别的字节）");
    if (gid < 0 || gid > ((int64_t)1 << 40))
        extc_cen_fail("gid 越界 —— 偏移不对");
    if ((size_t)gid >= extc_cen_gcap) {
        size_t nc = extc_cen_gcap ? extc_cen_gcap : 1024;
        while (nc <= (size_t)gid) nc *= 2;
        int64_t *na = (int64_t *)realloc(extc_cen_gcnt, nc * sizeof(int64_t));
        int64_t *nb = (int64_t *)realloc(extc_cen_gext, nc * sizeof(int64_t));
        int64_t *nc2 = (int64_t *)realloc(extc_cen_gbytes, nc * sizeof(int64_t));
        if (!na || !nb || !nc2) extc_cen_fail("gid 表分配失败");
        memset(na + extc_cen_gcap, 0, (nc - extc_cen_gcap) * sizeof(int64_t));
        memset(nb + extc_cen_gcap, 0, (nc - extc_cen_gcap) * sizeof(int64_t));
        memset(nc2 + extc_cen_gcap, 0, (nc - extc_cen_gcap) * sizeof(int64_t));
        extc_cen_gcnt = na; extc_cen_gext = nb; extc_cen_gbytes = nc2; extc_cen_gcap = nc;
    }
    extc_cen_gcnt[gid]++;
    extc_cen_gbytes[gid] += (int64_t)s->ss;
    if (gp == 0) extc_cen_gext[gid]++;
    if (gp == 0) { extc_cen_freed_gpin0++; extc_cen_slots_gpin0 += (long long)s->ss; }
    else         { extc_cen_freed_gpin1++; extc_cen_slots_gpin1 += (long long)s->ss; }
}

static void extc_cen_end(size_t freed, long long ns) {
    if (!extc_cen_on || !extc_cen_in_sweep) return;
    if (extc_cen_struct) {
        if (freed != extc_cen_dead_expect)
            extc_cen_fail("清扫释放数与死集大小不等（freed != popcount(occ & ~mark)）");
        if (extc_cen_freed_now != freed)
            extc_cen_fail("freeSlot 记账数与清扫返回的 freed 不等");
    }
    if (extc_cen_rows < CEN_MAXROWS) { extc_cen_freed[extc_cen_rows] = freed; extc_cen_sweep_ns[extc_cen_rows] = ns; }
    extc_cen_rows++;
    extc_cen_sweeps_total++;
    extc_cen_in_sweep = 0;
}

static void extc_cen_dump(void) {
    if (!extc_cen_on) return;
    char path[600];
    snprintf(path, sizeof path, "%s.sweeps.csv", extc_cen_prefix);
    FILE *fs = fopen(path, "w");
    if (fs) {
        fprintf(fs, "sweep,slabs,slots_scan,occ_visit,dead,freed,sweep_ns\n");
        for (size_t i = 0; i < extc_cen_rows && i < CEN_MAXROWS; i++)
            fprintf(fs, "%zu,%zu,%zu,%zu,%zu,%zu,%lld\n", i, extc_cen_nslabs[i], extc_cen_slots_scan[i],
                    extc_cen_occ[i], extc_cen_dead[i], extc_cen_freed[i], extc_cen_sweep_ns[i]);
        if (extc_cen_sweeps_total > CEN_MAXROWS) fprintf(fs, "# 只记了前 %d 次，共 %zu 次\n", CEN_MAXROWS, extc_cen_sweeps_total);
        fclose(fs);
    }
    snprintf(path, sizeof path, "%s.groups.csv", extc_cen_prefix);
    FILE *fg = fopen(path, "w");
    size_t groups = 0; long long gmax = 0;
    for (size_t g = 0; g < extc_cen_gcap; g++) if (extc_cen_gcnt[g]) {
        groups++;
        if (extc_cen_gcnt[g] > gmax) gmax = extc_cen_gcnt[g];
        if (fg) fprintf(fg, "%zu,%lld,%lld,%lld\n", g, (long long)extc_cen_gcnt[g], (long long)extc_cen_gext[g], (long long)extc_cen_gbytes[g]);
    }
    if (fg) fclose(fg);
    snprintf(path, sizeof path, "%s.comps.csv", extc_cen_prefix);
    FILE *fc = fopen(path, "w");
    if (fc) {
        for (size_t i = 0; i < extc_cen_comp_n; i++) fprintf(fc, "%zu\n", extc_cen_comp_exact[i]);
        if (extc_cen_comp_over) fprintf(fc, "# 还有 %zu 个分量没记（封顶 4096）\n", extc_cen_comp_over);
        fclose(fc);
    }
    fprintf(extc_cen_f, "CENSUS-mode struct=%d time=1\n", extc_cen_struct);
    if (extc_cen_struct) fprintf(extc_cen_f,
        "CENSUS sweeps=%zu rows=%zu freed=%lld freed_node=%lld gpin0_freed=%lld gpin1_freed=%lld "
        "gpin0_slotbytes=%lld gpin1_slotbytes=%lld groups_with_freed=%zu group_max=%lld "
        "comps=%zu comp_obj_total=%lld\n",
        extc_cen_sweeps_total, extc_cen_rows, extc_cen_freed_all, extc_cen_freed_node, extc_cen_freed_gpin0,
        extc_cen_freed_gpin1, extc_cen_slots_gpin0, extc_cen_slots_gpin1, groups, gmax,
        extc_cen_comp_total, extc_cen_comp_obj_total);
    for (size_t b = 0; b < 40; b++)
        if (extc_cen_comp_bucket[b])
            fprintf(extc_cen_f, "CENSUS-comp bucket=%zu comps=%lld objs=%lld\n",
                    b, extc_cen_comp_bucket[b], extc_cen_comp_obj_bucket[b]);
    fprintf(extc_cen_f, "CENSUS files=%s.{sweeps,groups,comps}.csv\n", extc_cen_prefix);
    extc_cen_q("sweep_ns", extc_cen_sweep_ns, extc_cen_rows);
    extc_cen_q("mark_ns", extc_cen_mark_ns, extc_cen_mark_n < 4096 ? extc_cen_mark_n : 4096);
}
'''

ANCHOR_GLOBALS = "__attribute__((unused)) static void extc_rc_freeSlot(ExtcRcSlab *s, size_t i) {"
ANCHOR_SWEEP = "static size_t extc_rc_sweep(ExtcRcRegion *r) {"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--gid-off", type=int, default=8)
    ap.add_argument("--gpin-off", type=int, default=72)
    ap.add_argument("--node-ss", type=int, default=80)
    ap.add_argument("--out", default="-")
    a = ap.parse_args()
    t = open(a.src, encoding="utf-8", errors="surrogateescape").read()

    rt = RUNTIME.replace("static int       extc_cen_gid_off = 8, extc_cen_gpin_off = 72;",
                         f"static int       extc_cen_gid_off = {a.gid_off}, extc_cen_gpin_off = {a.gpin_off};")
    rt = rt.replace("static int       extc_cen_node_ss = 80;", f"static int       extc_cen_node_ss = {a.node_ss};")
    if t.count(ANCHOR_GLOBALS) != 1 or t.count(ANCHOR_SWEEP) != 1:
        print("rc-sweep-census: 找不到唯一的 extc_rc_freeSlot / extc_rc_sweep 定义", file=sys.stderr)
        return 2
    t = t.replace(ANCHOR_GLOBALS, rt + "\n" + ANCHOR_GLOBALS, 1)

    a1 = "static size_t extc_rc_sweep(ExtcRcRegion *r) {\n    size_t freed = 0;\n"
    if t.count(a1) != 1:
        print("rc-sweep-census: extc_rc_sweep 形状不对", file=sys.stderr)
        return 2
    t = t.replace(a1, "static size_t extc_rc_sweep(ExtcRcRegion *r) {\n    size_t freed = 0;\n"
                      "    long long _cen_s0 = extc_cen_ns();\n"
                      "    extc_cen_begin(r);\n", 1)
    a2 = "    r->freedSlots += freed;\n    return freed;\n}"
    if t.count(a2) != 1:
        print("rc-sweep-census: extc_rc_sweep 结尾形状不对", file=sys.stderr)
        return 2
    t = t.replace(a2, "    extc_cen_end(freed, extc_cen_ns() - _cen_s0);\n    r->freedSlots += freed;\n    return freed;\n}", 1)
    a_mark = "__attribute__((unused)) static size_t extc_rc_mark(ExtcRcRegion *r, void *const *roots, size_t nroots) {\n    r->msp = 0;"
    if t.count(a_mark) != 1:
        print("rc-sweep-census: extc_rc_mark 形状不对", file=sys.stderr)
        return 2
    t = t.replace(a_mark, "__attribute__((unused)) static size_t extc_rc_mark(ExtcRcRegion *r, void *const *roots, size_t nroots) {\n"
                          "    long long _cen_m0 = extc_cen_ns();\n    r->msp = 0;", 1)
    a_mret = "    return marked;\n}"
    if t.count(a_mret) != 1:
        print("rc-sweep-census: extc_rc_mark 返回形状不对", file=sys.stderr)
        return 2
    t = t.replace(a_mret, "    if (extc_cen_mark_n < 4096) extc_cen_mark_ns[extc_cen_mark_n] = extc_cen_ns() - _cen_m0;\n"
                          "    if (extc_cen_on) extc_cen_mark_n++;\n    return marked;\n}", 1)
    a3 = "    char *p = EXTC_RC_SLAB_DATA(s) + i * s->ss;\n    if (extc_rc_rsIncN)"
    if t.count(a3) != 1:
        print("rc-sweep-census: extc_rc_freeSlot 形状不对", file=sys.stderr)
        return 2
    t = t.replace(a3, "    char *p = EXTC_RC_SLAB_DATA(s) + i * s->ss;\n    extc_cen_free(s, p);\n"
                      "    if (extc_rc_rsIncN)", 1)

    out = open(a.out, "w", encoding="utf-8", errors="surrogateescape") if a.out != "-" else sys.stdout
    out.write(t)
    if out is not sys.stdout:
        out.close()
    print("rc-sweep-census: 已插桩", a.src, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

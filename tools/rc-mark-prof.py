#!/usr/bin/env python3
"""tools/rc-mark-prof.py —— `mark` 相的**分段归因**（只动生成物副本，不碰发射器）。

用法：
    python3 tools/rc-mark-prof.py gen.c > gen.mp.c
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O2 gen.mp.c -o prog
    EXTC_MARK_PROF=1 EXTC_MARK_PROF_OUT=/tmp/mp ./prog 1000000 20261013

把一次 `mark` 拆成四段（每段都是**同一函数内的墙上计时**，四次 `clock_gettime` 一次 mark）：

  1. `seed_roots`  —— 帧根表扫描 + 入队；
  2. `seed_rsbit`  —— 跨区种子位图（`rsbit & occ`）整片扫描 + 入队；
  3. `drain`       —— 栈式字段遍历：出栈 / 归属校验 / 占用位与标记位探针 / 置标记位 /
                      沿 `ExtcRcType{nptr, off}` 追指针 + 压栈；
  4. 残余          —— `total − (1+2+3)`（说不清的进这里，不硬摊）。

同时数出结构性事实（用于估计"哪一段可跳"）：根表项数、`rsbit` 扫描字数、从 `rsbit` 入队的槽数、
**"rsbit 扫描了字但一个种子都没入队"的 mark 次数**（S2 的适用率）、对象出栈数、标记对象数、
被三种 `continue` 各挡掉多少、压栈孩子数、访问的 `nptr` 总数（⇒ 平均出度）、保守描述符对象数、
标记栈最大深度。全部 env 门控（不设 `EXTC_MARK_PROF` 时一行不输出、也不记数）。

**牙**（不静默给 0）：每个 mark 结束断言
`出栈数 == 标记数 + 三种跳过数`、`标记数 == 函数返回的 marked`、`进入时标记栈为空`；
任一不成立立即 `MARKPROF-FAIL` + exit 3。
"""
import argparse
import sys

RUNTIME = r'''
/* ================= mark 分段归因（tools/rc-mark-prof.py 插入） =================
 * 需要 -D_POSIX_C_SOURCE=200809L。只读诊断：不改判决、不改发射、不写标记栈的内容。 */
#include <time.h>
#define MP_MAX 8192
static int    extc_mp_on = -1;
static FILE  *extc_mp_f = 0;
static char   extc_mp_prefix[512];
static long long mp_ns_seed_roots[MP_MAX], mp_ns_seed_rsbit[MP_MAX];
static long long mp_ns_drain[MP_MAX], mp_ns_total[MP_MAX];
static long long mp_row_bytes[MP_MAX], mp_row_thr[MP_MAX], mp_row_gap[MP_MAX];
static long long mp_row_pop[MP_MAX], mp_row_marked[MP_MAX], mp_row_children[MP_MAX], mp_row_nptr[MP_MAX];
static long long mp_row_rs_words[MP_MAX];
static size_t mp_rows;
static long long mp_start_ns, mp_last_ns;
static long long mp_cur_bytes, mp_cur_thr;
/* 当前 mark 的计数器 */
static long long mp_roots_n, mp_roots_push, mp_rs_words, mp_rs_push;
static long long mp_pop, mp_marked, mp_skip_out, mp_skip_occ, mp_skip_mark;
static long long mp_children, mp_nptr, mp_cons_objs, mp_cons_words, mp_stackmax;
/* 累计（所有 mark） */
static long long mp_t_roots_n, mp_t_roots_push, mp_t_rs_words, mp_t_rs_push, mp_t_rs_marks;
static long long mp_t_rs_empty_marks, mp_t_pop, mp_t_marked, mp_t_skip_out, mp_t_skip_occ;
static long long mp_t_skip_mark, mp_t_children, mp_t_nptr, mp_t_cons_objs, mp_t_cons_words;
static long long mp_t_stackmax;
static int     mp_in_mark;

static void extc_mp_fail(const char *why) {
    fprintf(stderr, "MARKPROF-FAIL: %s\n", why);
    exit(3);
}
static long long extc_mp_ns(void) {
    struct timespec t;
    t.tv_sec = 0; t.tv_nsec = 0;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static int extc_mp_cmp(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static void extc_mp_q(const char *tag, long long *v, size_t n) {
    if (!n) return;
    qsort(v, n, sizeof v[0], extc_mp_cmp);
    long long s = 0;
    for (size_t i = 0; i < n; i++) s += v[i];
    fprintf(extc_mp_f, "MARKPROF-time %s n=%zu p50=%lld p90=%lld p99=%lld max=%lld sum=%lld\n",
            tag, n, v[n / 2], v[n * 9 / 10 < n ? n * 9 / 10 : n - 1],
            v[n * 99 / 100 < n ? n * 99 / 100 : n - 1], v[n - 1], s);
}
static void extc_mp_dump(void) {
    if (extc_mp_on != 1) return;
    char path[600];
    snprintf(path, sizeof path, "%s.marks.csv", extc_mp_prefix);
    FILE *fc = fopen(path, "w");
    if (fc) {
        fprintf(fc, "mark,ns_seed_roots,ns_seed_rsbit,ns_drain,ns_total,gap_ns,bytes,thr,pop,marked,children,nptr,rs_words\n");
        for (size_t i = 0; i < mp_rows && i < MP_MAX; i++)
            fprintf(fc, "%zu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                    i, mp_ns_seed_roots[i], mp_ns_seed_rsbit[i], mp_ns_drain[i], mp_ns_total[i],
                    mp_row_gap[i], mp_row_bytes[i], mp_row_thr[i], mp_row_pop[i], mp_row_marked[i],
                    mp_row_children[i], mp_row_nptr[i], mp_row_rs_words[i]);
        fclose(fc);
    }
    extc_mp_q("seed_roots", mp_ns_seed_roots, mp_rows < MP_MAX ? mp_rows : MP_MAX);
    extc_mp_q("seed_rsbit", mp_ns_seed_rsbit, mp_rows < MP_MAX ? mp_rows : MP_MAX);
    extc_mp_q("drain", mp_ns_drain, mp_rows < MP_MAX ? mp_rows : MP_MAX);
    extc_mp_q("total", mp_ns_total, mp_rows < MP_MAX ? mp_rows : MP_MAX);
    fprintf(extc_mp_f, "MARKPROF marks=%zu roots_n=%lld roots_push=%lld rs_words=%lld rs_push=%lld "
            "rs_marks=%lld rs_empty_marks=%lld pop=%lld marked=%lld skip_out=%lld skip_occ=%lld "
            "skip_mark=%lld children=%lld nptr=%lld cons_objs=%lld cons_words=%lld stackmax=%lld\n",
            mp_rows, mp_t_roots_n, mp_t_roots_push, mp_t_rs_words, mp_t_rs_push, mp_t_rs_marks,
            mp_t_rs_empty_marks, mp_t_pop, mp_t_marked, mp_t_skip_out, mp_t_skip_occ, mp_t_skip_mark,
            mp_t_children, mp_t_nptr, mp_t_cons_objs, mp_t_cons_words, mp_t_stackmax);
    fprintf(extc_mp_f, "MARKPROF files=%s.marks.csv\n", extc_mp_prefix);
}
static void extc_mp_arm(void) {
    if (extc_mp_on >= 0) return;
    extc_mp_on = getenv("EXTC_MARK_PROF") ? 1 : 0;
    if (extc_mp_on != 1) return;
    const char *pp = getenv("EXTC_MARK_PROF_OUT");
    snprintf(extc_mp_prefix, sizeof extc_mp_prefix, "%s", pp ? pp : "/tmp/extc-mark-prof");
    extc_mp_f = stderr;
    atexit(extc_mp_dump);
}
static void extc_mp_begin(ExtcRcRegion *r, void *const *roots, size_t nroots) {
    extc_mp_arm();
    if (extc_mp_on != 1) return;
    if (mp_in_mark) extc_mp_fail("mark 重入（上一趟没结束）");
    if (r->msp != 0) extc_mp_fail("进入 mark 时标记栈非空");
    (void)roots;
    mp_cur_bytes = r->bytes;
    mp_cur_thr = r->thr;
    mp_start_ns = extc_mp_ns();
    mp_in_mark = 1;
    mp_roots_n = (long long)nroots; mp_roots_push = 0; mp_rs_words = 0; mp_rs_push = 0;
    mp_pop = 0; mp_marked = 0; mp_skip_out = 0; mp_skip_occ = 0; mp_skip_mark = 0;
    mp_children = 0; mp_nptr = 0; mp_cons_objs = 0; mp_cons_words = 0; mp_stackmax = 0;
}
static void extc_mp_end(ExtcRcRegion *r, size_t marked, long long ns_roots, long long ns_rsbit,
                        long long ns_total) {
    (void)r;
    if (extc_mp_on != 1) return;
    if (mp_pop != mp_marked + mp_skip_out + mp_skip_occ + mp_skip_mark)
        extc_mp_fail("出栈数 != 标记数 + 三种跳过数（分段计数对不上）");
    if ((size_t)mp_marked != marked)
        extc_mp_fail("计数到的标记数 != mark 返回的 marked");
    mp_in_mark = 0;
    if (mp_rows < MP_MAX) {
        mp_ns_seed_roots[mp_rows] = ns_roots;
        mp_ns_seed_rsbit[mp_rows] = ns_rsbit;
        mp_ns_drain[mp_rows] = ns_total - ns_roots - ns_rsbit;
        mp_ns_total[mp_rows] = ns_total;
        mp_row_bytes[mp_rows] = mp_cur_bytes;
        mp_row_thr[mp_rows] = mp_cur_thr;
        mp_row_gap[mp_rows] = mp_last_ns ? mp_start_ns - mp_last_ns : 0;
        mp_row_pop[mp_rows] = mp_pop;
        mp_row_marked[mp_rows] = mp_marked;
        mp_row_children[mp_rows] = mp_children;
        mp_row_nptr[mp_rows] = mp_nptr;
        mp_row_rs_words[mp_rows] = mp_rs_words;
    }
    mp_last_ns = mp_start_ns;
    mp_rows++;
    mp_t_roots_n += mp_roots_n; mp_t_roots_push += mp_roots_push; mp_t_rs_words += mp_rs_words;
    mp_t_rs_push += mp_rs_push; mp_t_rs_marks++;
    if (mp_rs_words > 0 && mp_rs_push == 0) mp_t_rs_empty_marks++;
    mp_t_pop += mp_pop; mp_t_marked += mp_marked; mp_t_skip_out += mp_skip_out;
    mp_t_skip_occ += mp_skip_occ; mp_t_skip_mark += mp_skip_mark; mp_t_children += mp_children;
    mp_t_nptr += mp_nptr; mp_t_cons_objs += mp_cons_objs; mp_t_cons_words += mp_cons_words;
    if (mp_stackmax > mp_t_stackmax) mp_t_stackmax = mp_stackmax;
}
'''

A_MARK = "__attribute__((unused)) static size_t extc_rc_mark(ExtcRcRegion *r, void *const *roots, size_t nroots) {\n    r->msp = 0;"
A_MARKPUSH = "__attribute__((unused)) static void extc_rc_markPush(ExtcRcRegion *r, void *o) {"
A_ROOTS = "        if (roots[i]) extc_rc_markPush(r, roots[i]);"
A_RS = "            uint64_t bits = s->rsbit[w] & s->occ[w];   /* 仍占用的登记槽才是入口 */"
A_RS_PUSH = "                extc_rc_markPush(r, EXTC_RC_SLAB_DATA(s) + ((size_t)w * 64 + b) * s->ss);\n            }\n        }\n    }\n    size_t marked = 0;"
A_POP = ("        void *o = r->mstk[--r->msp];\n"
         "        ExtcRcSlab *s = extc_rc_ownSlab(o);\n"
         "        if (!s || s->region != r) continue;        /* 区域外/非 rc ⇒ 当活的，止步 */")
A_OUT = "        if (!s || s->region != r) continue;        /* 区域外/非 rc ⇒ 当活的，止步 */"
A_OCC = ("        if (!(s->occ[extc_rc_word(i)] & bit)) continue;   /* 空闲槽不是对象 */\n"
         "        if (s->mark[extc_rc_word(i)] & bit) continue;\n")
A_MARKED = "        s->mark[extc_rc_word(i)] |= bit;\n        marked++;"
A_CONS = "            size_t words = s->ss / sizeof(void *);\n            for (size_t w2 = 0; w2 < words; w2++) {\n                void *child = ((void **)o)[w2];\n                if (child) extc_rc_markPush(r, child);\n            }"
A_NPTR = "        for (uint32_t k = 0; k < t->nptr; k++) {   /* 义务 4：按对象自己的描述符走 */\n            void *child = *(void **)((char *)o + t->off[k]);\n            if (child) extc_rc_markPush(r, child);\n        }"
A_RET = "    return marked;\n}"
A_PUSH_DEF = "    r->mstk[r->msp++] = o;\n}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--out", default="-")
    a = ap.parse_args()
    t = open(a.src, encoding="utf-8", errors="surrogateescape").read()
    for name, pat in (("mark 签名", A_MARK), ("根表入队", A_ROOTS), ("rsbit 读字", A_RS),
                      ("rsbit 入队+段尾", A_RS_PUSH), ("出栈+区域外跳过", A_POP),
                      ("空闲槽+已标记跳过", A_OCC), ("置标记", A_MARKED),
                      ("保守分支", A_CONS), ("nptr 循环", A_NPTR), ("mark 返回", A_RET),
                      ("栈深度钩子", A_PUSH_DEF), ("markPush 签名", A_MARKPUSH)):
        if t.count(pat) != 1:
            print(f"rc-mark-prof: 找不到唯一的 {name}（{t.count(pat)} 处）", file=sys.stderr)
            return 2

    t = t.replace(A_MARKPUSH, RUNTIME + "\n" + A_MARKPUSH, 1)
    t = t.replace(A_PUSH_DEF, "    if (extc_mp_on == 1 && mp_in_mark && r->msp > (size_t)mp_stackmax)\n"
                              "        mp_stackmax = (long long)r->msp;\n" + A_PUSH_DEF, 1)
    # 函数体：声明 + 四个计时点
    t = t.replace(A_MARK, A_MARK.replace("    r->msp = 0;",
        "    long long _mp0 = extc_mp_ns(), _mp1 = 0, _mp2 = 0, _mp3 = 0;\n"
        "    extc_mp_begin(r, roots, nroots);\n    r->msp = 0;"), 1)
    t = t.replace(A_ROOTS, "        if (roots[i]) { mp_roots_push++; extc_rc_markPush(r, roots[i]); }\n"
                           "    _mp1 = extc_mp_ns();", 1)
    t = t.replace(A_RS, "            mp_rs_words++;\n" + A_RS, 1)
    t = t.replace(A_RS_PUSH,
                  "                mp_rs_push++;\n" + A_RS_PUSH.replace("    size_t marked = 0;",
                  "    _mp2 = extc_mp_ns();\n    size_t marked = 0;"), 1)
    t = t.replace(A_POP, "        mp_pop++;\n"
                          "        void *o = r->mstk[--r->msp];\n"
                          "        ExtcRcSlab *s = extc_rc_ownSlab(o);\n"
                          "        if (!s || s->region != r) { mp_skip_out++; continue; }        /* 区域外/非 rc ⇒ 当活的，止步 */", 1)
    t = t.replace(A_OCC, "        if (!(s->occ[extc_rc_word(i)] & bit)) { mp_skip_occ++; continue; }   /* 空闲槽不是对象 */\n"
                          "        if (s->mark[extc_rc_word(i)] & bit) { mp_skip_mark++; continue; }\n", 1)
    t = t.replace(A_MARKED, "        s->mark[extc_rc_word(i)] |= bit;\n        mp_marked++;\n        marked++;", 1)
    t = t.replace(A_CONS, "            mp_cons_objs++;\n            size_t words = s->ss / sizeof(void *);\n"
                          "            for (size_t w2 = 0; w2 < words; w2++) {\n                void *child = ((void **)o)[w2];\n"
                          "                mp_cons_words++;\n                if (child) { mp_children++; extc_rc_markPush(r, child); }\n            }", 1)
    t = t.replace(A_NPTR, "        mp_nptr += (long long)t->nptr;\n        for (uint32_t k = 0; k < t->nptr; k++) {   /* 义务 4：按对象自己的描述符走 */\n"
                          "            void *child = *(void **)((char *)o + t->off[k]);\n"
                          "            if (child) { mp_children++; extc_rc_markPush(r, child); }\n        }", 1)
    t = t.replace(A_RET, "    _mp3 = extc_mp_ns();\n    extc_mp_end(r, marked, _mp1 - _mp0, _mp2 - _mp1, _mp3 - _mp0);\n    return marked;\n}", 1)

    out = open(a.out, "w", encoding="utf-8", errors="surrogateescape") if a.out != "-" else sys.stdout
    out.write(t)
    if out is not sys.stdout:
        out.close()
    print("rc-mark-prof: 已插桩", a.src, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

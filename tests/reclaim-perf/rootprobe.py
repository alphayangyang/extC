#!/usr/bin/env python3
"""把 extc 生成的、带回收运行期的 C 做**只读探针插桩**，回答两个问题：

  1. 每个清扫触发点上物化的根表里，**有几个槽、有几个某一刻非空、有几个一辈子是死的**；
  2. `owns` 守卫（`extc_rc_ownSlab` / `extc_rc_ownLower` / `extc_rc_owns`）被调了多少次，
     其中多少次是**必须**走二分的、多少次落在空指针/表外/空闲槽。

用法：rootprobe.py <in.c> <out.c>，然后照常 cc 编译。
探针只加计数，不改任何判据；结果走 stderr，前缀 extc_probe。
"""
import re
import sys

SRC, DST = sys.argv[1], sys.argv[2]
s = open(SRC).read()


def sub_once(pat, rep, why, flags=0):
    global s
    new, n = re.subn(pat, rep, s, count=1, flags=flags)
    assert n == 1, "probe patch failed: " + why
    s = new


# ---- 1. 在 maybeCollect 之后插探针函数 --------------------------------------
anchor = """static int extc_rc_maybeCollect(ExtcRcRegion *r, void *const *roots, size_t nroots) {
    if (!extc_rc_shouldSweep(r)) return -1;
    return (int)extc_rc_collect(r, roots, nroots);
}
"""
assert anchor in s, "maybeCollect anchor not found"

probe_vars = r'''
/* ---- 探针：只读计数，不改判据（变量提前声明：ownSlab 定义在 maybeCollect 之前）---- */
#define EXTC_PROBE_MAXSITE 32
static struct {
    long calls;
    size_t n;
    unsigned char *ever;
    long long nonnull_sum, nonnull_max, nonnull_last;
} extc_probe_site[EXTC_PROBE_MAXSITE];
static long extc_probe_sweeps, extc_probe_collect_calls;
static long extc_probe_ownN_max;
static long extc_probe_ownSlab_calls, extc_probe_ownSlab_null, extc_probe_ownSlab_miss,
            extc_probe_ownSlab_hit, extc_probe_ownSlab_rej_off, extc_probe_ownSlab_rej_slot,
            extc_probe_ownSlab_rej_data;
static long extc_probe_owns_calls, extc_probe_owns_false;
static long extc_probe_ownLower_calls;
static int extc_probe_armed;
'''
vars_anchor = "#define EXTC_RC_SLAB_DATA(s) ((char *)(s) + EXTC_RC_SLAB_HDR)\n"
assert vars_anchor in s, "vars anchor not found"
s = s.replace(vars_anchor, vars_anchor + probe_vars, 1)

probe = r'''
static void extc_probe_dump(void) {
    long tot_calls = 0; long long tot_slots = 0;
    for (int i = 0; i < EXTC_PROBE_MAXSITE; i++) {
        if (!extc_probe_site[i].calls) continue;
        size_t ever = 0;
        for (size_t k = 0; k < extc_probe_site[i].n; k++)
            if (extc_probe_site[i].ever[k]) ever++;
        fprintf(stderr,
            "extc_probe root site=%d calls=%ld nslots=%zu ever_nonnull=%zu dead_slots=%zu "
            "avg_nonnull=%.1f max_nonnull=%lld last_nonnull=%lld slots_materialized=%lld\n",
            i, extc_probe_site[i].calls, extc_probe_site[i].n, ever,
            extc_probe_site[i].n - ever,
            (double)extc_probe_site[i].nonnull_sum / (double)extc_probe_site[i].calls,
            extc_probe_site[i].nonnull_max, extc_probe_site[i].nonnull_last,
            (long long)extc_probe_site[i].calls * (long long)extc_probe_site[i].n);
        tot_calls += extc_probe_site[i].calls;
        tot_slots += (long long)extc_probe_site[i].calls * (long long)extc_probe_site[i].n;
    }
    fprintf(stderr, "extc_probe totals root_calls=%ld slots_materialized=%lld sweeps=%ld "
                    "collect_calls=%ld ownSlab_calls=%ld ownSlab_null=%ld ownSlab_miss=%ld "
                    "ownSlab_rej_off=%ld ownSlab_rej_slot=%ld ownSlab_rej_data=%ld ownSlab_hit=%ld "
                    "ownLower_calls=%ld owns_calls=%ld owns_false=%ld ownN_max=%ld\n",
            tot_calls, tot_slots, extc_probe_sweeps, extc_probe_collect_calls,
            extc_probe_ownSlab_calls, extc_probe_ownSlab_null, extc_probe_ownSlab_miss,
            extc_probe_ownSlab_rej_off, extc_probe_ownSlab_rej_slot, extc_probe_ownSlab_rej_data,
            extc_probe_ownSlab_hit,
            extc_probe_ownLower_calls, extc_probe_owns_calls, extc_probe_owns_false,
            extc_probe_ownN_max);
}

static void extc_probe_root(int site, void *const *roots, size_t n) {
    if (!extc_probe_armed) { extc_probe_armed = 1; atexit(extc_probe_dump); }
    if (site < 0 || site >= EXTC_PROBE_MAXSITE) return;
    if (!extc_probe_site[site].ever) {
        extc_probe_site[site].ever = (unsigned char *)calloc(n ? n : 1, 1);
        extc_probe_site[site].n = n;
    } else if (extc_probe_site[site].n != n) {
        return;
    }
    extc_probe_site[site].calls++;
    long long nn = 0;
    for (size_t k = 0; k < n; k++)
        if (roots[k]) { nn++; extc_probe_site[site].ever[k] = 1; }
    extc_probe_site[site].nonnull_sum += nn;
    if (nn > extc_probe_site[site].nonnull_max) extc_probe_site[site].nonnull_max = nn;
    extc_probe_site[site].nonnull_last = nn;
}

static int extc_probe_call(int site, ExtcRcRegion *r, void *const *roots, size_t n) {
    extc_probe_root(site, roots, n);
    return extc_rc_maybeCollect(r, roots, n);
}
'''
s = s.replace(anchor, anchor + probe, 1)

# ---- 2. 插桩：清扫 / owns 统计 ----------------------------------------------
sub_once(r"\n(\s*)size_t marked = extc_rc_mark\(r, roots, nroots\);",
         r"\n\1extc_probe_collect_calls++;\n\1size_t marked = extc_rc_mark(r, roots, nroots);",
         "collect counter")
sub_once(r"(static size_t extc_rc_sweep\(ExtcRcRegion \*r\) \{\n)",
         r"\1    extc_probe_sweeps++;\n", "sweep counter")
sub_once(r"(static ExtcRcSlab \*extc_rc_ownSlab\(void \*p\) \{\n)",
         r"\1    extc_probe_ownSlab_calls++;\n", "ownSlab counter")
sub_once(r"(    if \(!p\) return \(ExtcRcSlab \*\)0;\n)",
         "    if (!p) { extc_probe_ownSlab_null++; return (ExtcRcSlab *)0; }\n",
         "ownSlab null counter")
sub_once(r"(    if \(i >= extc_rc_ownN\) return \(ExtcRcSlab \*\)0;\n)",
         "    if (i >= extc_rc_ownN) { extc_probe_ownSlab_miss++; return (ExtcRcSlab *)0; }\n",
         "ownSlab miss counter")
sub_once(r"(    if \(extc_rc_slabOf\(p\) != s\) return \(ExtcRcSlab \*\)0;\n)",
         "    if (extc_rc_slabOf(p) != s) { extc_probe_ownSlab_rej_off++; return (ExtcRcSlab *)0; }\n",
         "ownSlab off-reject counter")
sub_once(r"(    if \(k >= s->nslots\) return \(ExtcRcSlab \*\)0;\n)",
         "    if (k >= s->nslots) { extc_probe_ownSlab_rej_slot++; return (ExtcRcSlab *)0; }\n",
         "ownSlab slot-reject counter")
# 第 5 道校验（数据指针必须正好是槽首）：漏了它，五类计数加不齐 ownSlab_calls。
# 注意这几条必须**整行替换**——若把插桩行插在原行之前，原行会先返回，计数就成了死代码。
sub_once(r"(    if \(EXTC_RC_SLAB_DATA\(s\) \+ \(k << s->shift\) != \(char \*\)p\) return \(ExtcRcSlab \*\)0;\n)",
         "    if (EXTC_RC_SLAB_DATA(s) + (k << s->shift) != (char *)p) "
         "{ extc_probe_ownSlab_rej_data++; return (ExtcRcSlab *)0; }\n",
         "ownSlab data-reject counter")
sub_once(r"(    return s;\n\})",
         r"    extc_probe_ownSlab_hit++;\n    return s;\n}", "ownSlab hit counter")
sub_once(r"(static size_t extc_rc_ownLower\(const void \*base\) \{\n)",
         r"\1    extc_probe_ownLower_calls++;\n", "ownLower counter")
# 登记表最大条目数 ⇒ 二分深度（log2）。
sub_once(r"(    extc_rc_ownTab\[i\] = s;\n    extc_rc_ownN\+\+;\n)",
         "    extc_rc_ownTab[i] = s;\n    extc_rc_ownN++;\n"
         "    if (extc_rc_ownN > extc_probe_ownN_max) extc_probe_ownN_max = (long)extc_rc_ownN;\n",
         "ownN max tracker")
sub_once(r"(static int extc_rc_owns\(void \*p\) \{\n)",
         r"\1    extc_probe_owns_calls++;\n", "owns counter")
sub_once(re.escape("    if (!s) return 0;\n    size_t k = extc_rc_slotOf(s, p);\n"
         "    return (s->occ[extc_rc_word(k)] & extc_rc_bit(k)) != 0;\n"),
         "    if (!s) { extc_probe_owns_false++; return 0; }\n    size_t k = extc_rc_slotOf(s, p);\n"
         "    return (s->occ[extc_rc_word(k)] & extc_rc_bit(k)) != 0;\n",
         "owns false counter")

# ---- 3. 调用点：maybeCollect(...) → extc_probe_call(__LINE__, ...) -----------
# 两种发射形态都认：入口守卫**落地前**是 `maybeCollect(extc_rc_regionOf(...), …)`；
# 落地后是 `{ ExtcRcRegion *__extc_rcgN = extc_rc_regionOf(...);
#             if (extc_rc_shouldSweep(__extc_rcgN)) maybeCollect(__extc_rcgN, …); }`
# —— 后者里根表本来就只有真要扫时才物化，探针挂在同一个 `if` 之下，读数含义一致。
pat = re.compile(
    r"extc_rc_maybeCollect\(((?:extc_rc_regionOf\([^;]*?\))|(?:__extc_rcg\d+)), "
    r"\(void \*\[\]\)\{ ([^}]*?) \}, (\d+)\);")
_site = [0]
def _rep(m):
    i = _site[0]; _site[0] += 1
    return ("extc_probe_call(%d, %s, (void *const *)(void *[]){ %s }, %s);"
            % (i, m.group(1), m.group(2), m.group(3)))
s, cnt = pat.subn(_rep, s)
assert cnt > 0, "no call sites patched"
print("patched call sites:", cnt, file=sys.stderr)

open(DST, "w").write(s)

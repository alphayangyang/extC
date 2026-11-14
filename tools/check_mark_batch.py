#!/usr/bin/env python3
"""Structural smoke check for the batched mark walk, with mutations.

This does not establish a latency bound; it only keeps the shape that buys the
latency (pop a batch, prefetch each popped object as it lands in the batch, then
scan the batch) from being silently refactored back into a serial
one-object-at-a-time chase. It also pins the prefetch where it pays: inside the
fill loop, not re-split into a second pass over the batch, and pins the hoisted
batch limit (min of batch width / budget / work quota / stack depth computed
once) instead of re-testing four conditions on every pop.

The scan body lives in one file-scope macro (EXTC_RC_MARK_SCAN) shared by two
call sites: the batch loop (deep stack) and a shallow-stack fast path that pops
and scans objects without assembling a batch, draining the shallow stack in a
loop of its own. The fast path exists because the batch machinery (array / limit
min / fill loop / scan loop) is pure fixed cost when the mark stack holds only a
handful of objects; the batch is a prefetch-lead-time mechanism, so it only pays
when enough objects are queued. The drain loop pins one more thing: once we are
in the shallow path, re-entering the outer loop to re-test the walk/budget/
shallow conditions for every single object costs about as much as the scan
so the shallow path keeps popping until the stack grows past the threshold.
Both properties are pinned below -- measurement: callgrind on cyclic_pressure
200000 drops 94,587,483 -> 89,675,120 Ir once the shallow path is in.
"""
import argparse
import ast
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def emitter_text():
    source = (ROOT / 'src/back/reclaim.c').read_text()
    start = source.index('RECLAIM_RT[]')
    end = source.index('\n};', start)
    return ''.join(ast.literal_eval(m.group()) for m in re.finditer(r'"(?:[^"\\]|\\.)*"', source[start:end]))


def _no_clock(text, start, end):
    i = text.find(start)
    j = text.find(end, i + 1) if i >= 0 else -1
    return i >= 0 and j > i and 'extc_rc_pauseNow' not in text[i:j]


def _after(text, start, *nodes):
    i = text.find(start)
    if i < 0:
        return False
    tail = text[i:]
    pos = [tail.find(n) for n in nodes]
    return all(pp >= 0 for pp in pos) and pos == sorted(pos)


def violations(text):
    checks = {
        'scan macro defined': '#define EXTC_RC_MARK_SCAN(_rawIn) do { ' in text,
        'scan macro takes raw': '    uintptr_t _raw = (_rawIn); ' in text,
        'scan body shared once': text.count('uintptr_t _raw = (_rawIn);') == 1,
        'scan body not inlined twice': 'uintptr_t _raw = _batch[_b];' not in text,
        'shallow threshold defined': ('#ifndef EXTC_RC_MARK_SHALLOW' in text and
                                      '#define EXTC_RC_MARK_SHALLOW' in text),
        'shallow fast path': ('if ((size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0' in text and
                              'void *_one = r->mstk[--r->msp];' in text and
                              'EXTC_RC_MARK_SCAN((uintptr_t)_one);' in text),
        # 浅栈路径要一次把浅栈排空：每弹一个就回外层重判 while/浅栈条件，比扫描体本身还显眼
        'shallow drain loop': ('} while (r->msp > 0 && (size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0' in text),
        'batch constant': '#define EXTC_RC_MARK_BATCH_POP' in text,
        # 冷档：活集超过缓存阈值后每步预算只许**更小**（同一停顿预算下 pop 变贵），且只许降不许升。
        'cold tier defined': ('#ifndef EXTC_RC_MARK_COLD_BYTES' in text and '#define EXTC_RC_MARK_COLD_BYTES' in text and
                              '#ifndef EXTC_RC_MARK_COLD_BATCH' in text and '#define EXTC_RC_MARK_COLD_BATCH' in text),
        'cold tier clamp': ('if (r->live > (int64_t)EXTC_RC_MARK_COLD_BYTES && b > (size_t)EXTC_RC_MARK_COLD_BATCH) '
                            '{ b = (size_t)EXTC_RC_MARK_COLD_BATCH; extc_rc_markColdN++; }' in text),
        'cold tier inside advance': all(x in text for x in ('extc_rc_markAdvance(ExtcRcRegion *r) {',
                                                                'EXTC_RC_MARK_COLD_BATCH) { b = (size_t)EXTC_RC_MARK_COLD_BATCH',
                                                                'extc_rc_markStep(r, b);')) and
                                    (text.index('extc_rc_markAdvance(ExtcRcRegion *r) {') <
                                     text.index('EXTC_RC_MARK_COLD_BATCH) { b = (size_t)EXTC_RC_MARK_COLD_BATCH') <
                                     text.index('extc_rc_markStep(r, b);')),
        'cold tier only lowers': ('#if EXTC_RC_MARK_COLD_BATCH > EXTC_RC_MARK_BATCH_MAX' in text and
                                  'EXTC_RC_MARK_COLD_BATCH exceeds EXTC_RC_MARK_BATCH_MAX' in text),
        'cold tier reported': ('cold_steps=%lld' in text and 'extc_rc_markColdN' in text and
                               'cons_max=%lld' in text and 'extc_rc_wConsMax' in text),
        'batch array': 'uintptr_t _batch[EXTC_RC_MARK_BATCH_POP];' in text,
        'limit hoisted': ('size_t _lim = EXTC_RC_MARK_BATCH_POP;' in text and
                          'if (budget < _lim) _lim = budget;' in text and
                          'if (_sp < _lim) _lim = _sp;' in text),
        'stack head hoisted': 'void **_stk = r->mstk; size_t _sp = r->msp;' in text,
        'pop from local head': '_batch[_nb] = (uintptr_t)_stk[--_sp];' in text,
        'stack head restored': 'r->msp = _sp;' in text,
        'pop loop': 'while (_nb < _lim) {' in text,
        'budget accounting': 'budget -= _nb; pops += _nb;' in text,
        'prefetch in fill loop': re.search(r'_batch\[_nb\] = \(uintptr_t\)_stk\[--_sp\];\s*\n\s*__builtin_prefetch\(\(void \*\)\(_batch\[_nb\]', text) is not None,
        'no separate prefetch sweep': 'for (size_t _pf = 0' not in text,
        # 大对象分段扫描：批扫被 _split 截断时必须把**本批已弹出、还没扫**的栈项压回（它们已经
        # 从标记栈弹出来了，丢掉就是可达对象漏标）；三个循环都要在 _split 上停步；续扫游标不占
        # 栈，由 markStep 入口的 markResume 整步处理；所有完成判定都要多看一眼 scanObj。
        'batch scan calls macro': 'EXTC_RC_MARK_SCAN(_batch[_b]);' in text,
        # 走步循环里不许读时钟：按时间切步会让 [work] 逐次不同（gate-reclaim-work 断言的确定性不许破），
        # 停顿界只能来自工作预算（pop/字/压栈），不能来自墙钟。
        'batch walk reads no clock': _no_clock(text, '        size_t _b = 0;', '        if (_split) {   /* segmentation'),
        'shallow drain reads no clock': _no_clock(text, '            do {', '            } while (r->msp > 0 && (size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW'),
        'split stops the step': text.count('&& !_split') >= 3,
        'split leaves resume cursor': 'r->scanObj = o; r->scanOff = 0; r->scanDesc = 0; _split = 1;' in text,
        'split keeps unscanned batch items': ('if (_split) {' in text and 'while (_nb > _b) {' in text and
                                             'extc_rc_markPushForce(r, (void *)(_raw & ~(uintptr_t)1));' in text),
        'resume handled at entry': ('if (r->scanObj && (size_t)r->msp <= (size_t)EXTC_RC_MARK_STACK_SOFT) {' in text and
                                    'extc_rc_markResume(r);' in text),
        # R7：单步工作量由编译期常量封顶。这组断言钉的是「界还在源码里」，不是「界有多大」。
        'step pop cap': ('#ifndef EXTC_RC_MARK_STEP_POP_MAX' in text and '#define EXTC_RC_MARK_STEP_POP_MAX' in text and
                         'budget > (size_t)EXTC_RC_MARK_STEP_POP_MAX) budget = (size_t)EXTC_RC_MARK_STEP_POP_MAX' in text),
        'step push cap': ('#define EXTC_RC_MARK_PUSH_STEP' in text and
                          'if (pushes > (size_t)EXTC_RC_MARK_PUSH_STEP) _split = 1;' in text and
                          'if (r->msp > _sp) pushes' in text),
        'inline scan allowance': ('size_t _scanLeft = (size_t)EXTC_RC_SCAN_CHUNK;' in text and
                                  'if (words > _scanLeft) {' in text and '_scanLeft -= words;' in text),
        'mark stack soft cap': ('#ifndef EXTC_RC_MARK_STACK_SOFT' in text and '#define EXTC_RC_MARK_STACK_SOFT' in text),
        # R28：路径由屋顶决定。活集过了冷档阈值以后**不再**走浅栈快路（串行指针链，m=1，τ_pop 86-91 ns），
        # 改走批量路径让 miss 重叠（m≈4.8，τ_pop 20.5 ns）——两条路径的指令数逐位相同（§6.17），
        # 差的只是 miss 能不能重叠。阈值就是冷档那一个旋钮 EXTC_RC_MARK_COLD_BYTES，不另开常数。
        'warm-only path selector': ('#ifndef EXTC_RC_MARK_SHALLOW_WARM_ONLY' in text and
                                    '#define EXTC_RC_MARK_SHALLOW_WARM_ONLY 1' in text and
                                    '#if EXTC_RC_MARK_SHALLOW_WARM_ONLY' in text and
                                    '&& (int64_t)r->live <= (int64_t)EXTC_RC_MARK_COLD_BYTES' in text),
        # 描述符续扫：cursor 的第二种索引（0 = 保守词，1 = 字段）必须两条都能走，否则长描述符又回到一步扫完。
        'descriptor cursor': ('int32_t     scanDesc;' in text and 'if (r->scanDesc) {' in text and
                              'r->scanOff = (uint32_t)(st + walked);' in text),
        'work counters reported': ('pop_max=%lld' in text and 'push_max=%lld' in text and 'pops_tot=%lld' in text),
        # 巨块载荷分片丢页：一步只丢一个 chunk，free 只在页表已空时发生（1 ms 级 munmap 不许回来）。
        'huge payload dropped in pieces': ('extc_rc_bigTrimStep' in text and 'MADV_DONTNEED' in text and
                                          '#define EXTC_RC_BIG_DROP_BYTES' in text),
        'free drain bounded below a millisecond': ('#define EXTC_RC_FREE_HARD_DEADLINE_US 50' in text and
                                                  '#define EXTC_RC_FREE_DEADLINE_US 40' in text),
        'completion checks cursor': text.count('r->scanObj') >= 10 and text.count('|| r->scanObj') >= 5,
        'width ceiling': re.search(r'#if EXTC_RC_MARK_BATCH_POP > EXTC_RC_MARK_BATCH_MAX\s*\n#error', text) is not None,
        # 扫描体：子对象循环走「指针步进」而不是每次重读 t->nptr/t->off[k]（-O1 下实测省 0.68%）；
        # 推进计数器的分派点改成内联（-O1 不跨调用点内联 ⇒ 每趟一次调用/返回）。
        'child loop pointer walk': ('const uint32_t *_op = t->off; const uint32_t *_oend = _op + t->nptr;' in text and
                                    'for (; _op < _oend; ++_op) {' in text and
                                    'void *child = *(void **)((char *)o + *_op);' in text),
        # 推进计数的分派点必须仍是 always_inline 的内联函数；P1 给配额记账后它的返回
        # 类型从 void 变成 size_t（返回本轮推进的工作量，调用点 _used += ...），
        # 门禁只钉「内联分派点」，不钉返回类型。
        'mark advance inlined': re.search(
            r'__attribute__\(\(unused\)\) static inline __attribute__\(\(always_inline\)\) '
            r'\w+ extc_rc_markAdvance\(', text) is not None,
    }
    return [k for k, ok in checks.items() if not ok]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = emitter_text()
    if args.selftest:
        assert not violations(text)
        for token in ['#define EXTC_RC_MARK_SCAN(_rawIn) do { ',
                      '    uintptr_t _raw = (_rawIn); ',
                      '#define EXTC_RC_MARK_SHALLOW',
                      'if ((size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0',
                      '} while (r->msp > 0 && (size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0',
                      '#define EXTC_RC_MARK_BATCH_POP',
                      '#define EXTC_RC_MARK_COLD_BATCH 32',
                      'if (r->live > (int64_t)EXTC_RC_MARK_COLD_BYTES',
                      'cold_steps=%lld',
                      '#if EXTC_RC_MARK_COLD_BATCH > EXTC_RC_MARK_BATCH_MAX',
                      'uintptr_t _batch[EXTC_RC_MARK_BATCH_POP];',
                      'size_t _lim = EXTC_RC_MARK_BATCH_POP;',
                      'if (budget < _lim) _lim = budget;',
                      'void **_stk = r->mstk; size_t _sp = r->msp;',
                      'if (_sp < _lim) _lim = _sp;',
                      'r->msp = _sp;',
                      'while (_nb < _lim) {',
                      'budget -= _nb; pops += _nb;',
                      'EXTC_RC_MARK_SCAN(_batch[_b]);',
                      'if ((size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0',
                      'while (_nb > _b) {',
                      'r->scanObj = o; r->scanOff = 0; r->scanDesc = 0; _split = 1;',
                      'if (r->scanObj && (size_t)r->msp <= (size_t)EXTC_RC_MARK_STACK_SOFT) {',
                      '#define EXTC_RC_MARK_STEP_POP_MAX',
                      'budget > (size_t)EXTC_RC_MARK_STEP_POP_MAX) budget = (size_t)EXTC_RC_MARK_STEP_POP_MAX',
                      'if (pushes > (size_t)EXTC_RC_MARK_PUSH_STEP) _split = 1;',
                      '#define EXTC_RC_MARK_PUSH_STEP',
                      'if (words > _scanLeft) {',
                      '_scanLeft -= words;',
                      '#define EXTC_RC_MARK_STACK_SOFT',
                      '#if EXTC_RC_MARK_SHALLOW_WARM_ONLY',
                      'int32_t     scanDesc;',
                      '#define EXTC_RC_FREE_HARD_DEADLINE_US 50',
                      'extc_rc_markResume(r);',
                      '__builtin_prefetch((void *)(_batch[_nb] & ~(uintptr_t)1)',
                      '#if EXTC_RC_MARK_BATCH_POP > EXTC_RC_MARK_BATCH_MAX',
                      'const uint32_t *_op = t->off;',
                      '__attribute__((unused)) static inline __attribute__((always_inline)) size_t extc_rc_markAdvance(']:
            assert token in text, token
            mutated = text.replace(token, '/* injected */')
            assert violations(mutated), token
            print('ok red:', token)
        # 单独一趟预取循环不许回来：那会把预取推后（攒完再走一遍），也白多一趟循环
        split = text.replace('budget -= _nb; pops += _nb;',
                             'for (size_t _pf = 0; _pf < _nb; _pf++) __builtin_prefetch((void *)(_batch[_pf] & ~(uintptr_t)1), 0, 1);\n        budget -= _nb; pops += _nb;')
        assert 'no separate prefetch sweep' in violations(split)
        print('ok red: separate prefetch sweep')
        # 分段截断后不把没扫的批项压回（可达对象漏标）也必须红。
        lost = text.replace('if (_split) {', 'if (0) {')
        assert 'split keeps unscanned batch items' in violations(lost)
        print('ok red: split drops unscanned batch items')
        # 完成判定漏看续扫游标（会提前收尾/漏标）也必须红。
        nocursor = text.replace('|| r->scanObj', '')
        assert 'completion checks cursor' in violations(nocursor)
        print('ok red: completion ignores scan cursor')
        # 对象处理体抄回扫描循环里（宏只剩一处调用点）⇒ 两份体迟早漂
        dup = text.replace('EXTC_RC_MARK_SCAN(_batch[_b]);',
                           'uintptr_t _raw = _batch[_b]; (void)_raw;')
        assert 'scan body not inlined twice' in violations(dup)
        print('ok red: batch body inlined again')
        # 在走步循环里插一次读钟（按时间切步）必须红：那会让 [work] 随运行环境变化，破掉工作量确定性
        tick = text.replace('            EXTC_RC_MARK_SCAN(_batch[_b]);',
                            '            if (extc_rc_pauseNow() < 0) break;\n            EXTC_RC_MARK_SCAN(_batch[_b]);')
        assert 'batch walk reads no clock' in violations(tick)
        print('ok red: clock read inside batch walk')
        # 续扫入口丢掉标记栈软上限（cursor 会把栈推到可达前沿）必须红。
        nosoft = text.replace('if (r->scanObj && (size_t)r->msp <= (size_t)EXTC_RC_MARK_STACK_SOFT) {', 'if (r->scanObj) {')
        assert 'resume handled at entry' in violations(nosoft)
        print('ok red: resume ignores mark stack soft cap')
        # 路径选择退回「只看栈深」（冷档又走浅栈串行链）必须红：那是本轮修掉的性能洞。
        warmoff = text.replace('&& (int64_t)r->live <= (int64_t)EXTC_RC_MARK_COLD_BYTES', '&& 1')
        assert 'warm-only path selector' in violations(warmoff)
        print('ok red: path selector ignores the live set')
        # 每步 pop 硬上限被删掉（压力档回到 1024）必须红。
        nocap = text.replace('budget > (size_t)EXTC_RC_MARK_STEP_POP_MAX) budget = (size_t)EXTC_RC_MARK_STEP_POP_MAX', 'budget > 0) budget = budget')
        assert 'step pop cap' in violations(nocap)
        print('ok red: step pop cap removed')
        # 扫描额度守卫被删掉（大数组一步扫完）必须红。
        noallow = text.replace('if (words > _scanLeft) {', 'if (words > (size_t)EXTC_RC_SCAN_CHUNK) {')
        assert 'inline scan allowance' in violations(noallow)
        print('ok red: inline scan allowance removed')
        return 0
    bad = violations(text)
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok batched mark walk structure')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())

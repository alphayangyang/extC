#!/usr/bin/env python3
"""对**生成物 C** 做静态清点，回答两个量化问题（RECON.md 的问题 2 / 问题 5）：

  1. 触发点根表里的槽，按「基名是形参 / 文件级全局 / 本帧局部量」分类各有多少个
     —— 「本帧局部量」就是方案②里必须取地址（`&slot`、不能留在寄存器）的那一档。
  2. 调用图上的「可能触到 home arena 的调用点」占比：只有这种调用点才需要压/弹根栈，
     其余调用点在静态上就能省掉（问题 5）。判据 = 被调者的 home 可达性
     （自己发 `(*__extc_home)` 上的触发 / 分配，或经由被调者可达）。

用法（对单个生成物或一个目录里的 `*.c`）：
    tests/reclaim-rootstack/cost-model.py <file.c|dir> [...]
    --json <path>   逐文件明细；--quiet 只打合计。

「调用点」只数**被调者名字不以 `extc_` 开头**的调用（`extc_*` 是编译器内联的运行期/原语，
根栈不会插在它们前面）。解析只做行级括号配平，够用；解析失败的文件跳过并报 stderr。

只读输入，不改任何仓库文件。
"""
import argparse
import json
import os
import re
import sys

GLOBAL_RE = re.compile(r'^(?:static\s+|const\s+|extern\s+)*[A-Za-z_][\w \t\*]*?\s*\*?\s*([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(=|;)')
LOCAL_RE = re.compile(r'^[ \t]+(?:const\s+|static\s+|volatile\s+)*[A-Za-z_][\w \t\*]*?\s*\*?\s*([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(=|;|\[)')
IDENT_CALL = re.compile(r'(?<![\w>.$])([A-Za-z_]\w*)\s*\(')


def split_params(s):
    if not s.strip() or s.strip() == 'void':
        return []
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
            continue
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        cur += ch
    out.append(cur)
    names = []
    for p in out:
        m = re.search(r'([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*$', p.strip())
        names.append(m.group(1) if m else p.strip())
    return names


def match_header(ln):
    if not ln.endswith('{') or ln.startswith((' ', '\t', '#')):
        return None
    s = ln.rstrip()[:-1].rstrip()
    if re.match(r'^(typedef|struct|enum|union)\b', s):
        return None
    while s.startswith('__attribute__'):
        i = s.find('(')
        if i < 0:
            return None
        depth = 0
        for k in range(i, len(s)):
            if s[k] == '(':
                depth += 1
            elif s[k] == ')':
                depth -= 1
                if depth == 0:
                    break
        s = s[k + 1:].lstrip()
    if not s.endswith(')'):
        return None
    depth = 0
    k = -1
    for j in range(len(s) - 1, -1, -1):
        if s[j] == ')':
            depth += 1
        elif s[j] == '(':
            depth -= 1
            if depth == 0:
                k = j
                break
    if k < 0:
        return None
    m = re.search(r'([A-Za-z_]\w*)\s*$', s[:k])
    if not m:
        return None
    return m.group(1), split_params(s[k + 1:len(s) - 1])


def table_entries(text):
    out, i, needle = [], 0, '(void *)('
    while True:
        j = text.find(needle, i)
        if j < 0:
            return out
        k, depth, buf = j + len(needle), 1, ''
        while k < len(text) and depth:
            c = text[k]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
            buf += c
            k += 1
        out.append(buf.strip())
        i = k + 1


def slot_refs(text):
    """帧里**声明为引用槽**的 C 左值：codegen 每建一个引用槽都会发 `(void **)&X`
    （简单槽写作 `(void **)&l`、投影槽写作 `(void **)&(b.n)`）。
    这是"根栈要登记的槽"的集合（含只在声明处出现、从不进任何触发根表的槽）。"""
    out, i, needle = set(), 0, '(void **)&'
    while True:
        j = text.find(needle, i)
        if j < 0:
            return out
        k = j + len(needle)
        if k < len(text) and text[k] == '(':
            k, depth, buf = k + 1, 1, ''
            while k < len(text) and depth:
                c = text[k]
                if c == '(':
                    depth += 1
                elif c == ')':
                    depth -= 1
                    if depth == 0:
                        break
                buf += c
                k += 1
        else:
            k2, depth, buf = k, 0, ''
            while k2 < len(text):
                c = text[k2]
                if c in '([':
                    depth += 1
                elif c in ')]':
                    if depth == 0:
                        break
                    depth -= 1
                elif c == ',' and depth == 0:
                    break
                buf += c
                k2 += 1
            k = k2
        out.add(buf.strip())
        i = k + 1


def parse_c(text):
    lines = text.split('\n')
    funcs, globals_ = [], set()
    cur, depth = None, 0
    for idx, ln in enumerate(lines):
        if cur is None:
            h = match_header(ln)
            if h:
                cur = [h[0], h[1], [], idx]
                depth = ln.count('{') - ln.count('}')
                funcs.append(cur)
                if depth <= 0:
                    cur = None
            else:
                if not ln.startswith((' ', '\t', '}')) and ln.rstrip().endswith(';') and '(' not in ln:
                    g = GLOBAL_RE.match(ln)
                    if g and g.group(1) not in ('typedef', 'return', 'struct'):
                        globals_.add(g.group(1))
            continue
        cur[2].append(ln)
        depth += ln.count('{') - ln.count('}')
        if depth <= 0:
            cur = None
    return funcs, globals_


def base_symbol(expr):
    m = re.match(r'[\(\*&\s]*([A-Za-z_]\w*)', expr)
    return m.group(1) if m else expr


def frame_slots(refs, params, locals_, globals_):
    """从 `(void **)&(X)` 集合里挑出**本帧的槽**（排除 `t->f` / `(*p).f` / `__extc_*` 临时量）。
    返回 {表达式: 'local'|'param'|'global'}。"""
    out = {}
    for e in refs:
        if '->' in e or e.startswith('(*') or e.startswith('*'):
            continue
        b = base_symbol(e)
        if b.startswith('__extc'):
            continue
        if b in globals_:
            out[e] = 'global'
        elif b in params:
            out[e] = 'param'
        elif b in locals_:
            out[e] = 'local'
    return out


def analyse(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    funcs, globals_ = parse_c(text)
    names = {f[0] for f in funcs}
    info = {}
    for name, params, body, _ in funcs:
        joined = '\n'.join(body)
        locals_ = set()
        for bl in body:
            lm = LOCAL_RE.match(bl)
            if lm:
                locals_.add(lm.group(1))
        slots = []
        distinct = {}
        table_bytes = 0
        for ln in body:
            if 'extc_rc_maybeCollect(__extc_rcg' not in ln:
                continue
            j = ln.find('(void *[]){')
            if j < 0:
                continue
            tbl = ln[j:ln.rfind(' }') + 2] if ' }' in ln[j:] else ln[j:]
            table_bytes += len(tbl)
            for e in table_entries(ln[j:]):
                b = base_symbol(e)
                kind = ('global' if b in globals_ else 'param' if b in params
                        else 'local' if b in locals_ else 'other')
                slots.append((kind, b))
                distinct[(kind, e)] = True
        calls = []
        for bl in body:
            for cm in IDENT_CALL.finditer(bl):
                n = cm.group(1)
                if n in names and n != name and not n.startswith('extc_'):
                    calls.append(n)
        info[name] = {'home_param': '__extc_home' in params, 'slots': slots,
                      'distinct': distinct,
                      'decl_slots': frame_slots(slot_refs(joined), params, locals_, globals_),
                      'table_bytes': table_bytes, 'calls': calls,
                      'home_alloc': ('regionOf((void *)&((*__extc_home))' in joined
                                     or re.search(r'regionOf\(\(void \*\)__owc\d+->home', joined) is not None)}
    reach = {n: info[n]['home_alloc'] for n in info}
    changed = True
    while changed:
        changed = False
        for n, d in info.items():
            if reach[n]:
                continue
            if any(reach.get(c) for c in d['calls']):
                reach[n] = True
                changed = True
    return info, reach


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--json', default=None)
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    files = []
    for p in a.paths:
        if os.path.isdir(p):
            for dp, _, fs in os.walk(p):
                files += [os.path.join(dp, f) for f in sorted(fs) if f.endswith('.c')]
        else:
            files.append(p)
    files.sort()
    tot = {'files': 0, 'slots': {'local': 0, 'param': 0, 'global': 0, 'other': 0},
           'distinct': {'local': 0, 'param': 0, 'global': 0, 'other': 0},
           'calls': 0, 'calls_home': 0, 'funcs': 0, 'funcs_home_param': 0,
           'table_bytes': 0, 'out_bytes': 0, 'frames_with_table': 0, 'k_hist': {},
           'frames_need_table': 0, 'table_slots': 0, 'decl_slots': 0,
           'decl_kinds': {'local': 0, 'param': 0, 'global': 0}}
    detail = {}
    if not a.quiet:
        print(f'{"文件":44s} {"函数":>4s} {"根表槽 局/参/全/他":>22s} {"调用点":>7s} {"→home":>7s}')
    for f in files:
        try:
            fi, reach = analyse(f)
        except Exception as e:
            print(f'  skip {f}: {e}', file=sys.stderr)
            continue
        slots = {'local': 0, 'param': 0, 'global': 0, 'other': 0}
        dist = {'local': 0, 'param': 0, 'global': 0, 'other': 0}
        calls = calls_home = 0
        tbl_bytes = 0
        frames = 0
        for n, d in fi.items():
            for k, _ in d['slots']:
                slots[k] += 1
            if d['distinct']:
                frames += 1
                k = len(d['distinct'])
                tot['k_hist'][str(k)] = tot['k_hist'].get(str(k), 0) + 1
                for kk, _ in d['distinct']:
                    dist[kk] += 1
            tbl_bytes += d['table_bytes']
            calls += len(d['calls'])
            calls_home += sum(1 for c in d['calls'] if reach.get(c))
            tot['decl_slots'] += len(d['decl_slots'])
            for v in d['decl_slots'].values():
                tot['decl_kinds'][v] += 1
            if d['decl_slots'] and any(reach.get(c) for c in d['calls']):
                tot['frames_need_table'] += 1
                tot['table_slots'] += len(d['decl_slots'])
        tot['files'] += 1
        tot['funcs'] += len(fi)
        tot['funcs_home_param'] += sum(1 for d in fi.values() if d['home_param'])
        for k in slots:
            tot['slots'][k] += slots[k]
            tot['distinct'][k] += dist[k]
        tot['calls'] += calls
        tot['calls_home'] += calls_home
        tot['table_bytes'] += tbl_bytes
        tot['frames_with_table'] += frames
        tot['out_bytes'] += os.path.getsize(f)
        detail[os.path.basename(f)] = {'funcs': len(fi), 'slots': slots, 'distinct': dist,
                                       'calls': calls, 'calls_home': calls_home,
                                       'table_bytes': tbl_bytes, 'frames_with_table': frames}
        if not a.quiet:
            print(f'{os.path.basename(f):44s} {len(fi):4d} '
                  f'{slots["local"]:7d}/{slots["param"]:5d}/{slots["global"]:5d}/{slots["other"]:5d} '
                  f'{calls:7d} {calls_home:7d}')
    s, d = tot['slots'], tot['distinct']
    print('-- 合计 --')
    print(f'文件 {tot["files"]}  函数 {tot["funcs"]}（收 home 形参 {tot["funcs_home_param"]}）')
    print(f'根表槽**出现次数** {sum(s.values())} = 本帧局部 {s["local"]} / 形参 {s["param"]} / 全局 {s["global"]} / 未分类 {s["other"]}')
    print(f'根表槽**去重后**（帧 × 槽，≈方案②每帧地址表的项数）{sum(d.values())} = '
          f'本帧局部 {d["local"]} / 形参 {d["param"]} / 全局 {d["global"]} / 未分类 {d["other"]}')
    print(f'有根表的帧 {tot["frames_with_table"]}；根表字面量合计 {tot["table_bytes"]} B '
          f'（占生成物 {tot["out_bytes"]} B 的 {100.0 * tot["table_bytes"] / max(1, tot["out_bytes"]):.2f}%）')
    dk = tot['decl_kinds']
    print(f'帧内**声明**的引用槽（`(void **)&(X)` 去重、排除堆槽）{tot["decl_slots"]} 个 = '
          f'局部 {dk["local"]} / 形参 {dk["param"]} / 全局 {dk["global"]}；'
          f'其中「有引用槽、且调用可能触 home 的函数」的帧 {tot["frames_need_table"]} 个、'
          f'表项合计 {tot["table_slots"]}（≈方案②真正要建表的量）')
    hist = sorted(tot['k_hist'].items(), key=lambda kv: -int(kv[0]))[:8]
    print('每帧去重槽数 Top: ' + ', '.join(f'{k}槽×{v}帧' for k, v in hist))
    if tot['calls']:
        print(f'调用点 {tot["calls"]}，可能触到 home arena 的 {tot["calls_home"]} '
              f'⇒ 静态可省 {100.0 * (tot["calls"] - tot["calls_home"]) / tot["calls"]:.1f}%')
    if a.json:
        with open(a.json, 'w') as fh:
            json.dump({'total': tot, 'files': detail}, fh, indent=1)
        print(f'明细 → {a.json}')


if __name__ == '__main__':
    main()

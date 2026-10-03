#!/usr/bin/env python3
"""Check every `switch` over a type kind for a missing `TY_*` case.

Why this exists: adding a type constructor means touching several switches that walk the
type tree (`ttSubstitute`, `typeContainsRef`, `ttMangle`, the codegen printer, ...). Those
switches cannot be merged into one table without a visitor, and C has no way to say "this
switch is exhaustive" -- so the family stays spread out and a forgotten case is a silent
wrong answer. This turns "remember to update every switch" into a gate.

Usage:  python3 tools/check_tykind.py [--list]
Exit:   0 when every kind-switch handles every kind it could see, 1 otherwise.
"""
import os
import re, sys, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cscan   # src_files: one recursive discovery, refuses to return nothing

KINDS = []
src = open(cscan.src_file('.', 'ast.h'), encoding='utf-8').read()
m = re.search(r'typedef enum \{(.*?)\} TypeKind;', src, re.S)
for line in m.group(1).split('\n'):
    mm = re.match(r'\s*(TY_\w+)', line)
    if mm: KINDS.append(mm.group(1))

# Collect switch bodies whose subject mentions `kind`.
def switches(path):
    raw = open(path, encoding='utf-8', errors='replace').read()
    # Blank comments out **in place** (keep offsets) so reported line numbers point at the
    # real source line -- the first version reported post-strip lines and pointed at nothing.
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), raw, flags=re.S)
    text = re.sub(r'//[^\n]*', lambda m: ' ' * (m.end() - m.start()), text)
    out = []
    for m2 in re.finditer(r'switch\s*\(([^)]*)\)\s*\{', text):
        if 'kind' not in m2.group(1): continue
        i = m2.end(); depth = 1
        while i < len(text) and depth:
            if text[i] == '{': depth += 1
            elif text[i] == '}': depth -= 1
            i += 1
        body = text[m2.end():i-1]
        line = text[:m2.start()].count('\n') + 1
        cases = set(re.findall(r'case\s+(TY_\w+)', body))
        has_default = bool(re.search(r'\bdefault\s*:', body))
        # A `default` is how this family diverges silently: a kind added later lands there and
        # does whatever the default does. So a default owes the reader a reason **in the
        # source**, right where it is -- the marker below, plus one clause saying why every
        # kind that is not listed may fall through. The 4 sites in types.c got theirs when this
        # gate was added.
        justified = 'kind-default:' in raw[max(0, m2.start()-400):i]
        out.append((path, line, cases, has_default, len(body), justified))
    return out

bad = 0; rows = []
for f in cscan.src_files('.', ('.c',)):
    for path, line, cases, dflt, size, just in switches(f):
        if not cases: continue
        missing = [k for k in KINDS if k not in cases]
        rows.append((path, line, len(cases), dflt, missing, size, just))

print(f'类型 kind 的 switch：{len(rows)} 处（TY_* 共 {len(KINDS)} 个）')
print(f'{"位置":28s} {"case数":>6s} {"default":>8s} {"理由":>5s}  未处理')
for path, line, n, dflt, missing, size, just in rows:
    flag = ''
    if missing and not dflt:
        flag = '   <-- 缺且无 default'; bad += 1
    elif missing and not just:
        flag = '   <-- default 没写理由（`kind-default:` 标记）'; bad += 1
    print(f'{path.split("/")[-1]+":"+str(line):28s} {n:6d} {"有" if dflt else "-":>8s} '
          f'{"有" if just else "-":>5s}  '
          f'{",".join(k.replace("TY_","") for k in missing) if missing else "(全)":40s}{flag}')
print()
if bad:
    print(f'FAIL {bad} 处 switch 既漏了 kind 又没有 default ⇒ 新构造器会静默走错分支')
    sys.exit(1)
print('ok  每处 kind-switch 要么枚举齐全，要么有 default 兜底')

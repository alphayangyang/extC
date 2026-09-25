#!/usr/bin/env python3
"""手册覆盖检查：驱动开关逐项、公开面按模块计数（棘轮）。

    tools/check_manual.py            报告
    tools/check_manual.py --gate     有**新增**未覆盖项时失败（check.sh 调用）
    tools/check_manual.py --write-baseline

公开面的权威清单是 `tools/manual-surface.txt`（由源码生成）。手册正文是唯一真源
（`docs/manual/*.md`），HTML 是生成物。基线按**模块**记未覆盖计数：只许变少，不许变多 ——
这样新加一个公开成员而不写手册会被闸门挡住，而补齐既有欠账不会被阻塞。
"""
import re, sys, glob, os, subprocess

BASE = 'tools/manual-baseline.txt'
man = '\n'.join(open(f, encoding='utf-8').read() for f in glob.glob('docs/manual/*.md')
                if os.path.basename(f) != '20-internals.md')

def flags():
    exe = os.environ.get('EXTC', 'build/extc')
    out = subprocess.run([exe, '--help'], capture_output=True, text=True)
    return sorted(set(re.findall(r'(?<![\w-])(--?[a-zA-Z][\w-]*)', out.stdout + out.stderr)))

def surface():
    rows = [l.rstrip('\n').split('\t') for l in open('tools/manual-surface.txt', encoding='utf-8')
            if l.strip() and not l.startswith('#')]
    return [r for r in rows if r[4] == 'public']

miss_flag = [f for f in flags() if f not in man]
by_mod = {}
for mod, ty, name, kind, vis in surface():
    # 判定要求成员**出现在行内代码体里**（`` `name` `` / `` `name(` `` / `` `Type.name` ``），
    # 而不是散文里偶然出现同名 —— 否则"覆盖"会变成巧合。
    if not re.search(r'`[^`\n]*?(?<![\w])%s(?![\w])[^`\n]*?`' % re.escape(name), man):
        by_mod[mod] = by_mod.get(mod, 0) + 1

base = {}
if os.path.exists(BASE):
    for l in open(BASE, encoding='utf-8'):
        if l.strip() and not l.startswith('#'):
            k, v = l.rstrip('\n').split('\t'); base[k] = int(v)
grown = sorted((m, c, base.get(m, 0)) for m, c in by_mod.items() if c > base.get(m, 0))
fixed = sorted((m, base[m], by_mod.get(m, 0)) for m in base if base[m] > by_mod.get(m, 0))

if '--gate' in sys.argv:
    bad = False
    if miss_flag:
        print('手册漏了 %d 个驱动开关：%s' % (len(miss_flag), ', '.join(miss_flag))); bad = True
    if grown:
        for m, c, b in grown: print('模块 %s 的未覆盖公开成员从 %d 涨到 %d' % (m, b, c))
        bad = True
    if bad: sys.exit(1)
    print('ok  驱动开关全部在手册中；公开面未覆盖计数未增长（合计 %d）' % sum(by_mod.values())); sys.exit(0)

print('驱动开关：%d 个，手册未提 %d 个%s' % (len(flags()), len(miss_flag), '：' + ', '.join(miss_flag) if miss_flag else ''))
print('公开面：未覆盖 %d 个（按模块）：' % sum(by_mod.values()))
for m, c in sorted(by_mod.items(), key=lambda kv: -kv[1]): print('  %-16s %3d（基线 %d）' % (m, c, base.get(m, 0)))
for m, c, b in grown: print('  ⚠ %s 计数上涨：%d -> %d' % (m, b, c))
for m, b, c in fixed: print('  ✓ %s 已补齐 %d 个（可从基线下调到 %d）' % (m, b - c, c))
if '--write-baseline' in sys.argv:
    open(BASE, 'w', encoding='utf-8').write(
        '# tools/check_manual.py 的基线：各模块**尚未被手册覆盖**的公开成员计数。\n'
        '# 只许变少：补齐一批就把数字改小；新加公开成员而不写手册会被闸门挡住。\n'
        + ''.join('%s\t%d\n' % (m, c) for m, c in sorted(by_mod.items())))
    print('基线已写：', BASE)

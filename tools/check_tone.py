#!/usr/bin/env python3
"""手册的文体检查：正式书面语，不用第一/第二人称，不用口语与反问。

规则（docs/manual/README.md「文体约定」一节）：
  - 不出现第一人称「我 / 咱」，不出现第二人称「你 / 您」；主语缺失或写「使用者」；
  - 不用口语词与感叹；陈述句代替反问句。

`tools/tone-baseline.txt` 记录**尚未改写**的既有条目（键 = 文件|规则|整行文字，行号变动不影响）。
默认模式只报告；`--gate` 在出现**基线之外**的新条目时失败（基线只许变短，不许变长）。
"""
import re, sys, glob, os

RULES = [('first-person', r'[我咱](?![们])'),
         ('second-person', r'[你您]'),
         ('colloquial', r'这玩意儿|说白了|其实|别拿|挨骂|踩过|挺[好难]|吧[，。、]|呢[，。]|嘛|哦|哟'),
         ('rhetorical', r'？')]
BASE = 'tools/tone-baseline.txt'

def findings():
    out = []
    for f in sorted(glob.glob('docs/manual/*.md')):
        text = open(f, encoding='utf-8').read()
        text = re.sub(r'```.*?```', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
        # 「文体约定」一节为了说明规则，必须引用被禁的词与句式 —— 跳过它，否则规则文件自己违规。
        text = re.sub(r'\n## 文体约定.*?(?=\n## |\Z)', '\n', text, flags=re.S)
        for i, line in enumerate(text.split('\n'), 1):
            if line.lstrip().startswith('<!--'): continue
            bare = re.sub(r'`[^`]*`', '', line)          # 行内代码不算
            for name, pat in RULES:
                if re.search(pat, bare):
                    out.append((name, f, i, line.strip()[:70]))
                    break
    return out

key = lambda name, f, i, line: '%s|%s|%s' % (os.path.basename(f), name, line)
found = findings()
base = set()
if os.path.exists(BASE):
    base = {l.rstrip('\n') for l in open(BASE, encoding='utf-8') if l.strip() and not l.startswith('#')}
cur = {key(*x) for x in found}
new = sorted(k for k in cur if k not in base)
fixed = sorted(k for k in base if k not in cur)
if '--gate' in sys.argv:
    if new:
        print('手册文体：出现 %d 处基线之外的非正式写法（基线只许变短）' % len(new))
        for k in new[:12]: print('   ', k)
        sys.exit(1)
    print('ok  手册文体与基线一致（%d 处待改写，未新增）' % len(cur)); sys.exit(0)
print('手册文体：共 %d 处（基线 %d，新增 %d，已改好可删 %d）' % (len(cur), len(base), len(new), len(fixed)))
for n in new[:10]: print('   新增 ', n)
for f_ in fixed[:10]: print('   已修好，请从基线删除：', f_)
if '--write-baseline' in sys.argv:
    open(BASE, 'w', encoding='utf-8').write(
        '# tools/check_tone.py 的基线：手册里**尚未改写**的非正式条目。\n'
        '# 只许变短：改好一条就从这里删一条（键 = 文件|规则|整行文字）。\n'
        + '\n'.join(sorted(cur)) + '\n')
    print('基线已写：', BASE)

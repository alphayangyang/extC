#!/usr/bin/env python3
"""tools/check_manual_examples.py —— 手册里的**完整示例**必须编得过。

为什么需要它：手册现有的四道闸门（公开面清单、覆盖计数、文体、HTML 新鲜度）都不检查
"手册里的话是不是真的"。2026-09-27 的审计里，手册有 3 个带 `fn main` 的示例根本编不过
（`04-types.md` 的 `fn set(self: ref box<T>, …)`、`08-struct.md` 的 `fn moveBy(self: ref …)`、
`18-modules.md` 的 `match` 缺变体），而没有任何一道闸门会红。

口径：
  · 只收 ```extc 围栏、且正文里含 `fn main` 的块 —— 片段（签名、表达式）不参与；
  · 有意不完整的块（省略号、多文件示例、故意展示报错的片段）在**围栏上一行**写
        <!-- manual-example: skip -->
    即跳过；跳过是**显式**的，所以"新写的示例编不过"永远会被这条闸门抓住；
  · 编译命令与判据同口径：`build/extc -w -o /dev/null <临时文件>`（0 诊断即通过）。

用法：
    tools/check_manual_examples.py            # 报告
    tools/check_manual_examples.py --gate     # 有任何失败即退出 1（check.sh 用）
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

SKIP = 'manual-example: skip'
EXTC = os.environ.get('EXTC', 'build/extc')


def blocks():
    """(文件, 起始行, 正文, 是否显式跳过) 的列表。"""
    out = []
    for f in sorted(glob.glob('docs/manual/*.md')):
        lines = open(f, encoding='utf-8', errors='replace').read().splitlines()
        inb, tag, buf, start = False, '', [], 0
        for i, l in enumerate(lines, 1):
            if l.strip().startswith('```'):
                if not inb:
                    inb = True
                    tag = l.strip()[3:].strip()
                    start = i
                    buf = []
                    prev = lines[i - 2] if i >= 2 else ''
                    skipped = SKIP in prev
                else:
                    body = '\n'.join(buf)
                    if tag == 'extc' and 'fn main' in body:
                        out.append((f, start, body, skipped))
                    inb = False
            elif inb:
                buf.append(l)
    return out


def main():
    gate = '--gate' in sys.argv
    bs = blocks()
    tmp = tempfile.mkdtemp(prefix='manual_examples.')
    failed, skipped, passed = [], 0, 0
    for f, start, body, skip in bs:
        if skip:
            skipped += 1
            continue
        p = os.path.join(tmp, 'block.extc')
        with open(p, 'w', encoding='utf-8') as fh:
            fh.write(body + '\n')
        r = subprocess.run([EXTC, '-w', '-o', '/dev/null', p], capture_output=True, text=True)
        if r.returncode == 0:
            passed += 1
            continue
        first = (r.stderr.strip().splitlines() or ['(无输出)'])[0]
        first = re.sub(r'^%s' % re.escape(p), '', first).strip()[:110]
        failed.append((f, start, first))
    print('手册完整示例：通过 %d · 跳过 %d（显式标记） · 失败 %d' % (passed, skipped, len(failed)))
    for f, start, msg in failed:
        print('  编不过  %s:%d  %s' % (f, start, msg))
    if failed and not gate:
        print('\n（有意的片段请在围栏上一行写 <!-- %s -->）' % SKIP)
    return 1 if (failed and gate) else 0


if __name__ == '__main__':
    sys.exit(main())

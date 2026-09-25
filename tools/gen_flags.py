#!/usr/bin/env python3
"""由编译器自身的 `--help` 生成手册的驱动开关页（单一真源）。

    tools/gen_flags.py           重新生成 docs/manual/21-flags.md
    tools/gen_flags.py --check   与生成物不一致则失败（check.sh 调用）
"""
import subprocess, sys, os, re

EXE = os.environ.get('EXTC', 'build/extc')
PAGE = 'docs/manual/21-flags.md'

def help_text():
    out = subprocess.run([EXE, '--help'], capture_output=True, text=True)
    return (out.stdout + out.stderr).strip()

def page(text):
    flags = sorted(set(re.findall(r'(?<![\w-])(--?[a-zA-Z][\w-]*)', text)))
    head = [
        '# 驱动开关',
        '',
        '本页由编译器自身的 `--help` 生成（`tools/gen_flags.py`），因此与实现同步：'
        '`check.sh` 在本页与 `extc --help` 不一致时会失败。',
        '',
        '开关共 %d 个：%s' % (len(flags), ' · '.join('`%s`' % f for f in flags)),
        '',
        '```text',
        text,
        '```',
        '',
        '## 环境变量与调试开关',
        '',
        '带 `EXTC_` 前缀的开关只用于诊断，不改变输出；它们的含义以编译器自身打印的说明为准（见上）。',
        '',
    ]
    return '\n'.join(head)

if __name__ == '__main__':
    text = help_text()
    out = page(text)
    if '--check' in sys.argv:
        cur = open(PAGE, encoding='utf-8').read() if os.path.exists(PAGE) else ''
        if cur != out:
            print('docs/manual/21-flags.md 与 `extc --help` 不一致 -- 跑 tools/gen_flags.py'); sys.exit(1)
        print('ok  驱动开关页与 `extc --help` 一致（%d 个开关）' %
              len(set(re.findall(r'(?<![\w-])(--?[a-zA-Z][\w-]*)', text)))); sys.exit(0)
    open(PAGE, 'w', encoding='utf-8').write(out)
    print('已生成', PAGE)

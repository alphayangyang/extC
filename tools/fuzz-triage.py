#!/usr/bin/env python3
"""把 fuzz 产物分诊：按失败原因归类，并指出每条用例变异自哪个语料文件。

用法：python3 tools/fuzz-triage.py /tmp/extc-fuzz [...]
每次战役后手写一遍太费事，所以固化成工具。已知的**误报**原因也会单独列出，方便复查：
  - `输出依赖输入路径`  ：那条 oracle 会为合法的上下文差异报警，已从 fuzz.py 撤掉；
  - ASan 的 `requested allocation size`：超大分配请求，运行期 oracle 已加
    ASAN_OPTIONS=allocator_may_return_null=1（见 HARDENING.md round 3）。
"""
import os, re, sys, collections

KNOWN_FALSE = {
    '输出依赖输入路径': 'oracle 误报（已撤）',
    'requested allocation size': 'ASan 超大分配请求（oracle 已修）',
}

def main(dirs):
    by_reason = collections.Counter()
    real = []
    for root in dirs:
        if not os.path.isdir(root): continue
        for d in sorted(os.listdir(root)):
            if not d.startswith('fail-'): continue
            p = os.path.join(root, d, 'log.txt')
            try: t = open(p, encoding='utf-8', errors='replace').read()
            except OSError: continue
            m = re.search(r'^(extC 崩溃|extC 挂死|生成的 C 非法|sanitizer 报告（UB）|输出依赖输入路径)$',
                          t, re.M)
            why = m.group(1) if m else '未知'
            by_reason[why] += 1
            src = re.search(r'src=(\S+)', t)
            seed = re.search(r'seed=(\d+) it=(\d+)', t)
            detail = ''
            for line in t.splitlines():
                if any(k in line for k in KNOWN_FALSE): detail = line[:90]; break
            if why not in ('输出依赖输入路径',) and not detail:
                real.append((os.path.join(root, d), why, os.path.basename(src.group(1)) if src else '?',
                             seed.group(0) if seed else '?'))
    print('按原因分类：')
    for k, v in by_reason.most_common():
        mark = '  （已知误报：%s）' % next((d for kk, d in KNOWN_FALSE.items() if kk in k), '') if k in KNOWN_FALSE else ''
        print('  %-22s %d%s' % (k, v, mark))
    print('\n需要人看的：%d 条' % len(real))
    for path, why, src, seed in real[:20]:
        print('  %-40s %-16s ← %-28s %s' % (os.path.basename(path), why, src, seed))
    return 1 if real else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:] or ['/tmp/extc-fuzz']))

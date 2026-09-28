#!/usr/bin/env python3
"""bench/runtime/report.py —— raw.txt → RESULTS.md（**别手改 RESULTS.md**）。"""
import sys, os
raw = sys.argv[1] if len(sys.argv) > 1 else 'bench/runtime/raw.txt'
R = {}
for ln in open(raw, encoding='utf-8'):
    p = ln.split()
    if not p: continue
    R.setdefault(p[0], []).append(p[1:])
o = ['# 语言核心运行期 + 产物质量（含矩阵乘法归因）', '',
     '> `raw.txt` 是数据，`report.py` 出这张表（**别手改**）。口径、坑与结论见文末手写小节。', '']
def tbl(title, head, rows, unit=''):
    o.extend([f'## {title}', '', '| ' + ' | '.join(head) + ' |', '|' + '---|'*len(head)])
    for r in rows: o.append('| ' + ' | '.join(r) + ' |')
    o.append('')
NAMES = {'loop':'纯循环（下界）','statemachine':'手写 C 状态机','cpp20':'C++20 协程','extc':'extC 协程'}
tbl('协程 resume：1e6 次（求和 0..999999）', ['实现','时间'],
    [[NAMES.get(r[0],r[0]), f'{r[1]} µs'] for r in R.get('resume',[])])
b = {r[0]: int(r[1]) for r in R.get('bounds',[])}
tbl('边界检查：64 元素 × 800000 轮（51.2M 次访存）', ['版本','时间'],
    [['checked', f"{b.get('checked',0)} µs"], ['`@unchecked`', f"{b.get('unchecked',0)} µs"],
     ['差', f"{b.get('checked',0)-b.get('unchecked',0)} µs（噪声级）"]])
AN = {'extc_fresh':'extC `new`（每次新作用域）','c_calloc':'C `calloc`+`free`',
      'c_malloc_memset':'C `malloc`+`memset`+`free`','extc_reuse':'extC `new`（同一处复用）',
      'c_reuse':'C `malloc`+`memset`（复用）'}
tbl('arena：20000 次 × 16 KiB（零初始化语义不变）', ['实现','时间'],
    [[AN.get(r[0],r[0]), f'{r[1]} µs'] for r in R.get('arena',[])])
MM = {'handc_O2':'手写 C `-O2`（**同次序**，公平基线）','handc_O3native':'手写 C `-O3 -march=native`',
      'extc_O2':'**extC 产物 `-O2`（契约开关）**','extc_O2_unchecked':'extC 产物 `-O2` + `@unchecked`',
      'extc_O3native':'**extC 产物 `-O3 -march=native`**'}
tbl('矩阵乘法 n=768 f32（同 i-k-j 次序，绑 1 个 P 核）', ['产物','GFLOP/s','时间'],
    [[MM.get(r[0],r[0]), r[1], f'{r[2]} µs'] for r in R.get('matmul',[])])
c = R.get('compile',[['?','?']])[0]
o += [f'## 编译速度\n\n- tests/ 下 611 个 `.extc` 中 **{c[0]} 个独立可编译**，共 **{c[1]} ms** ⇒ {int(c[1])/max(int(c[0]),1):.1f} ms/程序。\n']
nb = os.path.join(os.path.dirname(__file__) or '.', 'NOTES.md')
if os.path.exists(nb): o.append(open(nb, encoding='utf-8').read())
dst = os.path.join(os.path.dirname(raw) or '.', 'RESULTS.md')
open(dst, 'w', encoding='utf-8').write('\n'.join(o) + '\n'); print(f'✓ 生成 {dst}')

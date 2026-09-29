#!/usr/bin/env python3
"""npytable.py —— 从 numpy 装着的头文件里读出 C-API 表的**编号与签名**，吐一个 extC 表镜像。

用法：tools/npytable.py <__multiarray_api.h> <libpython 的 soname>

为什么必须有这个工具：numpy 的 C API 是一张运行期拿到的**函数指针表**（`PyArray_API[N]`），
而 N 与签名**由装着的那个 numpy 版本说了算**。实测：手猜一格（槽 1）直接 SIGSEGV ✗ ——
C 的 ABI 不为签名负责，所以编号只能从 `#define PyArray_XXX (*(RET (*)(ARGS)) PyArray_API[N])`
里读出来。这就是"绑定生成器"该干的活（`tools/cbindgen.py` 的又一个形状）。
"""
import re
import sys


def parse(path):
    raw = open(path, encoding='utf-8', errors='replace').read()
    src = re.sub(r'\\\n', ' ', raw)                    # 续行符先粘起来
    pat = re.compile(r'#define\s+(PyArray_\w+)\s+\(\*\((.*?)\)\s+PyArray_API\[(\d+)\]\)')
    return {m.group(1): (m.group(2).strip(), int(m.group(3))) for m in pat.finditer(src)}


def main():
    hdr, soname = sys.argv[1], sys.argv[2]
    e = parse(hdr)
    ver = e['PyArray_GetNDArrayCVersion']              # 槽 0：版本，必须在
    size = e['PyArray_Size']                           # 判据用的那一格
    print('// 由 tools/npytable.py 生成 —— 编号与签名读自 %s' % hdr.split('/')[-1])
    print('// 表长 ≈ %d 格；这里只镜像判据要用的两格（生成器可以吐全部）。' % (max(v[1] for v in e.values()) + 1))
    print('let PY_SONAME: slice<u8> = "%s"' % soname)
    print('')
    print('@frozen struct apiTable {')
    n = size[1]
    for i in range(n + 1):
        if i == 0:
            print('    ver: fn() -> u32                     /* [0]  %s :: %s */' % ('PyArray_GetNDArrayCVersion', ver[0]))
        elif i == n:
            print('    size: fn(?ref void) -> i64           /* [%d] PyArray_Size :: %s */' % (i, size[0]))
        else:
            print('    pad%d: ?ref void' % i)
    print('}')


if __name__ == '__main__':
    main()

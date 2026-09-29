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
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from cbindgen import map_type, Unsupported   # 复用绑定生成器的类型映射（"表"只是它的又一个形状）


def parse(path):
    raw = open(path, encoding='utf-8', errors='replace').read()
    src = re.sub(r'\\\n', ' ', raw)                    # 续行符先粘起来
    # `(.*)` 必须**贪婪**：返回类型本身可能是函数指针（`PyObject * (*)(PyObject *)`），
    # 非贪婪会在它自己那个 `)` 上提前收住，吐出来的签名少一个括号 ✗（实测：映射器当场报
    # "看不懂的函数指针 `PyObject * )`"）。
    pat = re.compile(r'#define\s+(PyArray_\w+)\s+\(\*\((.*)\)\s+PyArray_API\[(\d+)\]\)')
    return {m.group(1): (m.group(2).strip(), int(m.group(3))) for m in pat.finditer(src)}


# numpy 的头文件里那些**不是 C 基本类型**的名字。写成一张小表，交给绑定生成器的映射器
# （`cbindgen.map_type`）⇒ 表里每一格的签名与 extC 声明走同一条映射，不会两套规则。
TYPEDEFS = {
    'npy_intp': 'intptr_t', 'npy_uintp': 'uintptr_t',
    'NPY_ORDER': 'int', 'NPY_CLIPMODE': 'int', 'NPY_CASTING': 'int',
    'NPY_SORTKIND': 'int', 'NPY_SCALARKIND': 'int', 'NPY_ARRAY_ORDER': 'int',
    'npy_bool': 'bool', 'npy_int8': 'int8_t', 'npy_uint8': 'uint8_t',
    'npy_int16': 'int16_t', 'npy_uint16': 'uint16_t',
    'npy_int32': 'int32_t', 'npy_uint32': 'uint32_t',
    'npy_int64': 'int64_t', 'npy_uint64': 'uint64_t',
    'PyTypeObject': 'void', 'PyArray_Descr': 'void', 'PyArrayObject': 'void',
    'PyArray_Dims': 'void', 'PyArrayIterObject': 'void', 'PyObject': 'void',
}
ENUMS = set(TYPEDEFS) | {'int', 'NPY_TYPES', 'NPY_ARRAY_FLAGS'}


def split_sig(sig):
    """`PyObject * (*)(PyObject *, PyObject *)` → (返回类型, [参数类型…])。"""
    depth = 0
    for i in range(len(sig) - 1, -1, -1):
        if sig[i] == ')':
            depth += 1
        elif sig[i] == '(':
            depth -= 1
            if depth == 0:
                head, args = sig[:i].strip(), sig[i + 1:-1].strip()
                break
    else:
        raise Unsupported('看不懂的签名 %s' % sig)
    # head 是 `PyObject * (*)` 这种 **cast 形态**：剥掉括号标记，但**保留指针的 `*`**
    # （`PyObject*` 与 `PyObject` 在映射器那里是两种东西：`?ref void` vs 一个 void 值 ✗）
    ret = head.replace('(*)', ' ').replace('(*', ' ').replace(')', ' ').strip() or 'void'
    params = [] if args in ('', 'void') else [a.strip() for a in args.split(',')]
    return ret, params


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    hdr, soname = args[0], args[1]
    need = []
    for i, a in enumerate(sys.argv):
        if a == '--need':
            need.append(sys.argv[i + 1])
    e = parse(hdr)
    ver = e['PyArray_GetNDArrayCVersion']              # 槽 0：版本，必须在
    size = e['PyArray_Size']
    slots = {0: ('ver', 'fn() -> u32', 'PyArray_GetNDArrayCVersion'),
             size[1]: ('size', 'fn(?ref void) -> i64', 'PyArray_Size')}
    for nm in need:
        if nm not in e:
            sys.exit('[npytable] 头文件里没有 %s' % nm)
        csig, idx = e[nm]
        ret, params = split_sig(csig)
        try:
            ps = ', '.join(map_type(p, ENUMS, False, TYPEDEFS) for p in params)
            rs = map_type(ret, ENUMS, True, TYPEDEFS)
        except Unsupported as exc:
            sys.exit('[npytable] %s 的签名映射不了：%s' % (nm, exc))
                # 字段名 = C 名去掉前缀（`PyArray_Size` → `size`），原名留在注释里
        field = nm[8:] if nm.startswith('PyArray_') else nm
        field = field[0].lower() + field[1:]
        slots[idx] = (field, 'fn(%s) -> %s' % (ps, rs), nm)
    top = max(max(slots), size[1])
    print('// 由 tools/npytable.py 生成 —— 编号与签名读自装着的 %s（不要手改）' % hdr.split('/')[-1])
    print('// 表长 ≈ %d 格；这里镜像 0..%d（没点名的格子是 `?ref void` 占位，不可以调）。' %
          (max(v[1] for v in e.values()) + 1, top))
    print('let PY_SONAME: slice<u8> = "%s"' % soname)
    print('')
    print('@frozen struct apiTable {')
    for i in range(top + 1):
        if i in slots:
            fname, fsig, cname = slots[i]
            print('    %s: %s   /* [%d] %s */' % (fname, fsig, i, cname))
        else:
            print('    pad%d: ?ref void' % i)
    print('}')


if __name__ == '__main__':
    main()

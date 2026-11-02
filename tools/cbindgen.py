#!/usr/bin/env python3
"""cbindgen.py —— 把 C 头文件变成 extC 绑定（一个 `fn` 字段的 struct + 一个 `dl::open` 填充器）。

用法：

    tools/cbindgen.py --include cairo.h -I /usr/include/cairo \\
        --only cairo_paint,cairo_create -o myproj/cairoapi.extc
    tools/cbindgen.py ... -o myproj/cairoapi.extc --check      # 生成物是否还是最新

为什么长这样
------------
**用户要的是"随心所欲 dlopen 任何库、一个字都不签"**，所以这个工具产出的是：

  ① 一个 `struct api`，**字段就是函数**（`fn` 类型）—— 调用写成 `c.paint(cr)`；
  ② 一个 `fn open(lib: slice<u8>) -> ?api`，把 `dlopen` + 每个符号的 `dlsym` + **显式转换**填好；
  ③ **一条 `effects` 都不生成**。这不是偷懒：`effects` 只在"把**帧内**地址就地交给 C"时才需要签
     （`Addr=0 Cont=0`），那种事实只有人能提供；而板/全局/句柄这些"不在帧里"的内存，库里已经用
     `Ret=0` 签过一次了（`C-ABI.md` §9.15）。生成的槽不签名 = 最坏情况 = **安全的那一侧** ✓
     （帧内地址会被拒，而不是悄悄变成悬垂指针）。

**类型映射故意保守**：不认识的结构体指针一律 `ref void`（看不进去），绝不去假装一个布局 ——
布局不一致是**静默错**，而按类型撒谎没有任何检查会拦。想读字段就得自己写 `@frozen` 镜像
（`C-ABI.md` §9.8），那是人的决定，不是工具的决定。

**不支持就报错，不猜**：按值传的结构体、变参、复杂函数指针、位域……都列出来并以非 0 退出。
宁可让人少绑几个函数，也不要生成一个签名错的绑定。

设计约束（用户定的）：工具在 extc 仓库里（`tools/`），但**每个动态库不需要单独建 stdlib 模块** ——
产出写到用户自己的项目路径，`use` 它就行。
"""
import argparse
import json
import os
import shlex
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------- 类型映射

#: C 的标量 → extC。键是 clang 的 `qualType` 去掉 const 之后的写法。
SCALARS = {
    'void': 'void',
    'bool': 'bool', '_Bool': 'bool',
    'char': 'i8', 'signed char': 'i8', 'unsigned char': 'u8',
    'short': 'i16', 'short int': 'i16', 'signed short': 'i16',
    'unsigned short': 'u16', 'unsigned short int': 'u16',
    'int': 'i32', 'signed int': 'i32', 'unsigned int': 'u32', 'unsigned': 'u32',
    'long': 'i64', 'long int': 'i64', 'signed long': 'i64', 'long long': 'i64',
    'long long int': 'i64', 'signed long long': 'i64',
    'unsigned long': 'u64', 'unsigned long int': 'u64',
    'unsigned long long': 'u64', 'unsigned long long int': 'u64',
    'float': 'f32', 'double': 'f64', 'long double': 'f64',
    'size_t': 'u64', 'ssize_t': 'i64', 'ptrdiff_t': 'i64',
    'int8_t': 'i8', 'uint8_t': 'u8', 'int16_t': 'i16', 'uint16_t': 'u16',
    'int32_t': 'i32', 'uint32_t': 'u32', 'int64_t': 'i64', 'uint64_t': 'u64',
    'intptr_t': 'i64', 'uintptr_t': 'u64',
}


class Unsupported(Exception):
    """这一格映射不了 —— 说清楚为什么，别猜。"""


def strip_const(t):
    while t.startswith('const '):
        t = t[len('const '):]
    return t.strip()


def is_pointer(t):
    return t.endswith('*')


def map_pointer(pointee, enum_names):
    """`T *` 怎么过界：一个机器字，指向什么由声明说了算。

    `char *` 是 C 字符串（`ref u8` 那份**原缓冲区**；字面量本来就 NUL 结尾）；`void *` 是
    `ref void`；其余（含不认识的结构体、不透明句柄如 `cairo_t *`）也映射成 `ref void` ——
    保守的意思是"看不进去"，而不是"猜它是 struct X 然后按它的字段读"。
    """
    p = strip_const(pointee)
    if p in ('char', 'signed char', 'unsigned char'):
        return 'u8'                       # C 字符串：`ref u8`
    if p == 'void':
        return 'void'
    return 'void'                         # 其余一律不透明


def resolve(t, typedefs, depth=0):
    """把 typedef 名一路剥到 C 的写法上（`cairo_format_t` → `enum cairo_format_t`）。"""
    t = strip_const(t)
    seen = 0
    while t in typedefs and seen < 16:
        t = strip_const(typedefs[t])
        seen += 1
    return t


def split_ret_args(qual):
    """把函数的 `qualType` 切成 (返回类型, 参数列表)。

    不能简单地在第一个 `(` 处切：返回类型本身可能是函数指针（`void (*)(int)`），所以要从左往右
    数括号深度，取**深度 0 的最后一个** `(` —— 那才是参数表的开头。
    """
    depth, cut = 0, -1
    for i, ch in enumerate(qual):
        if ch == '(':
            if depth == 0:
                cut = i
            depth += 1
        elif ch == ')':
            depth -= 1
    if cut < 0:
        return qual.strip(), ''
    return qual[:cut].strip(), qual[cut + 1:qual.rfind(')')].strip()


def map_type(t, enum_names, is_return, typedefs=None):
    """把 clang 的 `qualType` 映射成 extC 类型。

    Params:
        t          - clang 的 `qualType`（例如 `const cairo_t *`、`unsigned int`）
        enum_names - 头文件里见过的枚举类型名（映射成 `i32`）
        is_return  - 返回位置的 `void` 合法，参数位置的 `void` 只有 `void *` 才合法
    """
    typedefs = typedefs or {}
    raw = strip_const(t)
    t = resolve(raw, typedefs)
    if t in typedefs or (raw != t and not is_pointer(t) and not t.endswith(')')):
        pass
    if t == 'void':
        if is_return:
            return 'void'
        raise Unsupported('`void` 不能当参数')
    if t.endswith(')'):                   # 函数指针：`void (*)(void *, const unsigned char *, unsigned int)`
        return map_fnptr(t, enum_names, typedefs)
    if is_pointer(t):
        # `?ref T`：C 的头文件**不说**可空性（`T *` 就是"可能为空"），而可空是宽容的那一侧
        # （非空 → 可空是隐式的，调用点不用写任何东西）。要非空语义就在自己的绑定里改这一格。
        return '?ref ' + map_pointer(t[:-1], enum_names)
    if t in SCALARS:
        v = SCALARS[t]
        if v == 'void':
            raise Unsupported('`void` 只能当返回类型')
        return v
    if t in enum_names:
        return 'i32'                      # 枚举就是 int（值由常量给出）
    if t.startswith('enum '):
        return 'i32'
    if raw in enum_names:
        return 'i32'                      # typedef 到枚举：`cairo_format_t`
    raise Unsupported('无法映射的类型 `%s`（按值传的结构体要人写 @frozen 镜像）' % t)


def map_fnptr(t, enum_names, typedefs=None):
    """函数指针 → `fn(...) -> R`（`qsort` 的比较器那种形状）。

    返回位置写 `void` 也得写出来：extC 的函数**类型**没有函数体，返回类型不能推断。
    """
    depth = 0
    for i in range(len(t) - 1, -1, -1):
        if t[i] == ')':
            depth += 1
        elif t[i] == '(':
            depth -= 1
            if depth == 0:
                head, args = t[:i].strip(), t[i + 1:-1].strip()
                break
    else:
        raise Unsupported('看不懂的函数指针 `%s`' % t)
    ret = 'void'
    # `void (*)(...)` 的 head 是 `void (*`；取返回类型要把 `(*` 去掉
    if head:
        ret = map_type(head.split('(')[0], enum_names, True, typedefs)
    if args in ('', 'void'):
        params = []
    else:
        params, cur, depth = [], '', 0
        for ch in args:
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
            if ch == ',' and depth == 0:
                params.append(cur)
                cur = ''
            else:
                cur += ch
        params.append(cur)
    mapped = ', '.join(map_type(p, enum_names, False, typedefs) for p in params if p.strip())
    return 'fn(%s) -> %s' % (mapped, ret)


# ---------------------------------------------------------------- 读头文件


def clang_ast(includes, incdirs):
    """让 clang 把 AST 吐成 JSON（工具自己不写 C 解析器：这件事 clang 做得更好）。"""
    src = ''.join('#include <%s>\n' % h for h in includes)
    with tempfile.TemporaryDirectory() as tmp:
        cfile = os.path.join(tmp, 'tu.c')
        with open(cfile, 'w') as f:
            f.write(src)
        cmd = ['clang', '-fsyntax-only', '-Xclang', '-ast-dump=json']
        for d in incdirs:
            cmd += ['-I', d]
        cmd.append(cfile)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 and not r.stdout:
            sys.stderr.write(r.stderr)
            sys.exit('[cbindgen] clang 读不了这份头文件')
        return json.loads(r.stdout)


def eval_constants(includes, incdirs, names):
    """让 clang **求值**这些常量：编一个小程序把它们打印出来，再读回来。

    为什么需要它：clang 的 JSON AST 对**隐式**枚举值什么都不给（`CAIRO_FONT_SLANT_ITALIC,` 后面没有
    `= 1`，节点里就没有 value），而把常量放进数组初始值里 clang 也不折叠（试过：一个 IntegerLiteral
    都没有）。猜 0 就是**悄悄写错值** —— 生成器最不该犯的错。所以：编、跑、读。

    枚举值只需要头文件，不需要链接那个库，所以这一步在任何有头文件的机器上都能跑。
    求不出来的常量**跳过并报告**，绝不给一个猜测值。
    """
    if not names:
        return {}
    src = ''.join('#include <%s>\n' % h for h in includes)
    src += '#include <stdio.h>\nint main(void) {\n'
    for n in names:
        src += '    printf("%s=%%lld\\n", (long long)(%s));\n' % (n, n)
    src += '    return 0;\n}\n'
    with tempfile.TemporaryDirectory() as tmp:
        cfile = os.path.join(tmp, 'vals.c')
        exe = os.path.join(tmp, 'vals')
        with open(cfile, 'w') as f:
            f.write(src)
        cmd = ['cc', '-o', exe]
        for d in incdirs:
            cmd += ['-I', d]
        cmd.append(cfile)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return {}
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode != 0:
            return {}
    vals = {}
    for line in r.stdout.split('\n'):
        if '=' in line:
            k, v = line.split('=', 1)
            vals[k.strip()] = v.strip()
    return vals


def _walk_with_file(node, cur, visit):
    """按源码顺序遍历顶层声明：`loc.file` 只在**换文件**时出现，所以跟一个"当前文件"。

    这样才知道一条声明到底来自我们请求的头文件，还是来自它 `#include` 进来的系统头
    （`<unistd.h>` 的 `readlinkat`、`<sys/select.h>` 的 `select` 都会被 `--all` 抓到，
    在用户那边表现为"生成失败，因为 char *restrict" —— 但那条根本不该被生成）。"""
    if isinstance(node, list):
        for v in node:
            cur = _walk_with_file(v, cur, visit)
        return cur
    if not isinstance(node, dict):
        return cur
    f = node.get('loc', {}).get('file')
    if f:
        cur = f
    visit(node, cur)
    for v in node.get('inner', []) or []:
        cur = _walk_with_file(v, cur, visit)
    return cur


def collect(node, fns, enums, typedefs, wanted_files=None):
    """走一遍 AST，收下函数声明与枚举常量。

    `wanted_files` 非空时只收**这些文件里**声明的函数（`--all` 于是不会把系统头的函数卷进来）。
    clang 记的文件名是"它眼里的那个路径"（可能相对 `-I` 解析前的写法），所以按**真路径或文件名**
    匹配 —— 后者是必要的：`--include demo.h -I tests/cbindgen` 时 AST 里写的是
    `tests/cbindgen/demo.h`，而 `demo.h` 的 realpath 是当前目录下的另一个路径。
    """
    want_paths = {os.path.realpath(p) for p in (wanted_files or ())}
    want_names = {os.path.basename(p) for p in (wanted_files or ())}

    def visit(n, cur):
        kind = n.get('kind')
        if kind == 'FunctionDecl' and n.get('name'):
            if wanted_files is not None:
                if not cur:
                    return
                if os.path.realpath(cur) not in want_paths and os.path.basename(cur) not in want_names:
                    return
            fns.setdefault(n['name'], n)
    _walk_with_file(node, None, visit)
    return _collect_rest(node, enums, typedefs)


def _collect_rest(node, enums, typedefs):
    """枚举与 typedef 仍然全收（它们来自哪个头文件不影响正确性，只影响大小）。"""
    if isinstance(node, dict):
        kind = node.get('kind')
        if kind == 'TypedefDecl' and node.get('name'):
            typedefs.setdefault(node['name'], node.get('type', {}).get('qualType', ''))
        elif kind == 'EnumDecl':
            vals = []
            for c in node.get('inner', []):
                if c.get('kind') == 'EnumConstantDecl':
                    lit = None
                    for sub in c.get('inner', []):
                        if sub.get('kind') == 'ConstantExpr' and 'value' in sub:
                            lit = sub['value']
                    vals.append((c['name'], lit))     # None = 隐式值，等 clang 求值
            if vals:
                enums.setdefault(node.get('name') or '<anonymous>', vals)
        for v in node.values():
            _collect_rest(v, enums, typedefs)
    elif isinstance(node, list):
        for v in node:
            _collect_rest(v, enums, typedefs)


def params_of(fn):
    return [p for p in fn.get('inner', []) if p.get('kind') == 'ParmVarDecl']


# ---------------------------------------------------------------- 生成


def emit(fns_in, enums, libname):
    """生成一个 extC 绑定模块。

    形状：`struct api { 函数字段… }` + `fn open(lib) -> ?api`。
    **不生成 `effects`** —— 见文件头。也**不 dlclose**：表里全是那个库里的代码指针，
    关掉句柄 = use-after-unload（用户想关就自己 `dl::close(a.handle)`，句柄在表里）。
    """
    lines = []
    lines.append('// 由 tools/cbindgen.py 生成 —— 不要手改；重新生成：见文件头那条命令。')
    lines.append('//')
    lines.append('// %s：%d 个函数（头的来源见 run.sh / 命令）。' % (libname, len(fns_in)))
    lines.append('// **没有 effects**：槽不签 = 最坏情况 = 安全的那一侧（帧内地址会被拒）。帧内就地')
    lines.append('// 交给 C 的那几个函数要人签 `Addr=0 Cont=0`，工具不替作者说话（C-ABI.md §9.15）。')
    lines.append('// 打不开/缺符号都是**返回值**（`none`）：那是可预期的失败。要"就此停下"就自己写一句')
    lines.append('// `trap("…")`（`tests/trap*` 那套判据盯着它），例如：')
    lines.append('//     var oc: ?api = api::open("libcairo.so.2")')
    lines.append('//     if oc == none { trap("libcairo 没装上") }')
    lines.append('//     var c: api = oc!')
    lines.append('// 缺符号是**逐个人工判空**的（`api::open` 把缺的名字说出来）：`!` 只断言"有值" ——')
    lines.append('// 签错了是一个带位置的 trap（2026-10-08 起），但它不会告诉你缺的是哪个符号。')
    lines.append('// 结构体指针一律 `ref void`：不假装布局（要读字段就自己写 @frozen 镜像，§9.8）。')
    lines.append('use std::dl')
    lines.append('use std::io')
    lines.append('')
    if enums:
        lines.append('/* 头文件里那些枚举：extC 里就是 `i32` 常量（值由 clang 求出来，一个不猜）。 */')
        for name, vals in enums.items():
            for vname, vval in vals:
                if vval is None:
                    continue          # 求值也没拿到：宁可不生成，也不写一个错的值
                lines.append('let %s: i32 = %s' % (vname, vval))
        lines.append('')
    lines.append('struct api {')
    lines.append('    /* 库句柄：表里全是这个库里的代码指针，所以**别 dlclose** 它（要用 `dl::close(a.handle)` 自己担）。 */')
    lines.append('    handle: ?ref void')
    for short, cname, sig in fns_in:
        note = '' if short == cname else '   /* C: %s */' % cname
        lines.append('    %s: %s%s' % (short, sig, note))
    lines.append('}')
    lines.append('')
    # 取符号与填充都**挂在类型上**（关联函数，与 `ifstream::new` 同一个风格）：
    # 顶层通用名会撞 —— 实测 `use std::io` 之后本模块里的 `fn open` 直接报 "duplicate
    # function `open`"（导入一个模块会把它连同传递导入的顶层名字占住，哪怕调用要写限定名）。
    lines.append('impl api {')
    lines.append('    /* 取一个符号。用**可空引用**形态（`symOrNull`）而不是 `?`：`?` 只能用在返回')
    lines.append('     * `option`/`result` 的函数里，而这里要"缺了就把名字印出来再判空" —— 打得出人话的')
    lines.append('     * 那条路必须是判空。手上有 option 时用 `dl::sym(...)?`（编译器强制处理，见 `open`）。 */')
    lines.append('    @private')
    lines.append('    fn needSym(h: ?ref void, name: slice<u8>) -> ?ref void {')
    lines.append('        var p: ?ref void = dl::symOrNull(h, name)')
    lines.append('        if p == null { io::cout << "cbindgen: 库里没有符号 " << name << "\\n" }')
    lines.append('        return p')
    lines.append('    }')
    lines.append('')
    lines.append('    /* 打开库并填表。缺任何一个符号都返回 `none`（消息见上）。 */')
    lines.append('    fn open(lib: slice<u8>) -> ?api {')
    lines.append('        var h: ref void = dl::open(lib)?      /* `?`：打不开就早退，编译器强制处理 */')
    for i, (short, cname, sig) in enumerate(fns_in):
        lines.append('        var s%d: ?ref void = api::needSym(h, "%s")' % (i, cname))
        lines.append('        if s%d == null { return none }' % i)
    lines.append('        return some(api {')
    lines.append('            handle: h,')
    for i, (short, cname, sig) in enumerate(fns_in):
        comma = ',' if i + 1 < len(fns_in) else ''
        lines.append('            %s: %s(s%d)%s' % (short, sig, i, comma))
    lines.append('        })')
    lines.append('    }')
    lines.append('}')
    return '\n'.join(lines) + '\n'


# 这些形参类型是"一个字节的引用"：C 侧就是 `char *` / `const char *`。
CSTR_PARAM = ('ref u8', '?ref u8')

# 能"用 shim 变成返回值"的出参函数：只处理恰好一个 `T **`、且返回整数状态的那种。
INT_RET = ('i64', 'i32', 'u64', 'u32', 'bool')

# shim 的形参要用**绑定里那个 C 类型**（extC 侧看见的类型 ←→ C 类型），不能直接用头文件里的写法：
# 绑定里 `?ref u8` 生成出来是 `uint8_t *`，而头文件里是 `const char *` —— 两者在 C 里是不同的指针类型。
C_TYPE_OF = {'i64': 'int64_t', 'i32': 'int32_t', 'u32': 'uint32_t', 'u64': 'uint64_t',
             'f64': 'double', 'f32': 'float', 'bool': 'bool', 'void': 'void',
             'ref u8': 'uint8_t *', '?ref u8': 'uint8_t *', '?ref void': 'void *',
             'ref void': 'void *'}


def is_out_pointer(qt):
    """`T **`：一个出参指针（`sqlite3_open(path, &db)` 那种）。"""
    t = ' '.join(strip_const(qt).split())
    return t.endswith('**') or t.endswith('* *')


def _header_abspath(h, incdirs):
    """`--include demo.h -I x` 里那个 `demo.h`：先按当前目录，再按 `-I` 找，返回绝对路径。"""
    if os.path.isabs(h) and os.path.exists(h):
        return h
    if os.path.exists(h):
        return os.path.abspath(h)
    for d in incdirs:
        cand = os.path.join(d, h)
        if os.path.exists(cand):
            return os.path.abspath(cand)
    return os.path.abspath(h)


def emit_out_shims(shims, headers, incdirs):
    """生成 `<out>.shim.c`：把每个 `T **` 出参变成返回值。

    extC 里表达不出 `T **`（`ref` 只作用于值，`ref` 一个引用是非法），所以这一格由 C 补：
    shim 调原函数、把句柄返回、把状态码写进 `int64_t *rc`。生成器同时把它写进 `.link`
    （`ccflag <shim.c>` 与 `-I` 目录），`use` 这个模块的程序自动带上。
    """
    out = ['#include <stdint.h>']
    for h in headers:
        out.append('#include "%s"' % _header_abspath(h, incdirs))
    out.append('/* 由 tools/cbindgen.py 生成 —— 把 `T **` 出参变成返回值，不要手改。 */')
    for shim, cname, pointee, seq in shims:
        # 参数按头文件里的**原顺序**排（出参可能不在末尾），出参那一格换成 `int64_t *rc`。
        args, call = [], []
        for item in seq:
            if item[0] == 'p':
                _k, n, mt, qt = item
                args.append('%s %s' % (C_TYPE_OF.get(mt, mt), n))
                # 绑定的指针类型（`uint8_t *`）与头文件的（`const char *`）在 C 里不是同一个类型 ⇒ 显式转回去。
                call.append('(%s)%s' % (qt, n) if '*' in qt else n)
            else:
                args.append('int64_t *rc')
                call.append('&out')
        out.append('void *%s(%s) {' % (shim, ', '.join(args)))
        out.append('    %s out = 0;' % (pointee if pointee != 'void' else 'void *'))
        out.append('    int64_t r = (int64_t)%s(%s);' % (cname, ', '.join(call)))
        out.append('    *rc = r;')
        out.append('    return (void *)out;')
        out.append('}')
    return '\n'.join(out) + '\n'





def emit_extern(decls, enums, libname, effects):
    """生成**声明式**绑定：直接的 `extern!("<lib>") fn …`，给"链接进二进制"的库用。

    与 `emit`（dl 那条路）的分工：那条路把函数放进表，符号缺失是**返回值**；这条路直接让链接器
    解析符号 —— 缺符号是链接错误，而不是运行期的一次空指针。

    `effects`：`none`（默认，安全的那一侧 —— 调用方不能把本帧地址交出去）或 `trusted`
    （**一条签字**：不保留调用方给的地址、不从参数里读走指针；返回指针时再加 `Ret=0`）。
    `trusted` 是**声明不是证明** —— 包的 claims 台账里应该有一条判据去证伪它（`extpkg verify`）。
    """
    lines = []
    shims = []
    lines.append('// 由 tools/cbindgen.py 生成 —— 不要手改。')
    lines.append('//')
    lines.append('// %s：%d 个函数（声明式绑定，符号由链接器解析）。' % (libname, len(decls)))
    if effects == 'trusted':
        lines.append('// **effects 是签字，不是证明**：每一条都写着"不保留地址、不读走指针"（返回指针的再加')
        lines.append('// `Ret=0`）。包的 claims 台账要有一条判据去证伪它；改上游语义时这条判据会红。')
    else:
        lines.append('// **没有 effects**：不签 = 最坏情况 = 安全的那一侧（调用方不能把本帧地址交过来）。')
        lines.append('// 需要"帧内地址就地交给 C"时，人写在包里的一个薄 helper 上签那一句（C-ABI.md §9.15）。')
    lines.append('')
    if enums:
        for name, vals in enums.items():
            for vname, vval in vals:
                if vval is not None:
                    lines.append('let %s: i32 = %s' % (vname, vval))
        lines.append('')
    for cname, ret, params, seq, outs, note in decls:
        if note:
            lines.append('// 没绑：%s' % note)
        if outs:
            outs_qt = outs[0][1]
            pointee = ' '.join(outs_qt.replace('const', '').split())[:-1].strip()
            shim = 'extc_shim_%s' % cname
            shims.append((shim, cname, pointee, seq))
            args = ', '.join('%s: %s' % (n, t) for n, t in params)            # extern 的形参：照 C 的来
            wrap_args = ', '.join('%s: %s' % (n, ('slice<u8>' if t in CSTR_PARAM else t))
                                  for n, t in params)                        # 包装器收 slice<u8>
            call = ''.join(('cstr(%s)' if t in CSTR_PARAM else '%s') % n + ', '
                           for n, t in params)
            lines.append('/* `%s`：C 用 `%s` 当出参；同目录的 shim 把它变成返回值（`%s`）。 */'
                         % (cname, ' '.join(outs_qt.split()), shim))
            lines.append('extern!("%s") fn %s(%s%s) -> ?ref void' % (
                libname, shim, (args + ', ') if args else '', 'rc: mut ref i64'))
            lines.append('    effects Addr=0 Cont=0 Ret=0')
            lines.append('')
            lines.append('/* 状态码不是 0 就当"没拿到"（`null`）；要区分具体错误码就自己写一层。 */')
            lines.append('fn %s_out(%s) -> ?ref void {' % (cname, wrap_args))
            lines.append('    var rc: i64 = 0')
            lines.append('    var h: ?ref void = %s(%sref rc)' % (shim, call))
            lines.append('    if rc != 0 { return null }')
            lines.append('    return h')
            lines.append('}')
            continue
        sig = 'extern!("%s") fn %s(%s) -> %s' % (
            libname, cname,
            ', '.join('%s: %s' % (n, t) for n, t in params) or '',
            ret)
        lines.append(sig)
        if effects == 'trusted':
            eff = 'Addr=0 Cont=0' + (' Ret=0' if ret not in ('void',) and 'ref' in ret else '')
            lines.append('    effects %s' % eff)
    if any(t in CSTR_PARAM for _n, _r, ps, _cp, _o, _nt in decls for _pn, t in ps):
        lines.append('')
        lines.extend(emit_cstr_helpers(1))
    return '\n'.join(lines) + '\n', shims


# C 的 `char *` / `const char *` 在绑定里是 `ref u8`（一个字节的引用），调用点要 `ref buf[i]`。
# 下面这两个 helper 把那点仪式收掉：给一段 `slice<u8>`，它拷进模块自己的缓冲区并补 NUL，
# 返回首字节的引用。
CSTR_SLOTS = 4
CSTR_SLOT_BYTES = 1024


def emit_cstr_helpers(count):
    """生成 C 字符串 helper（只在绑定里真的出现 `ref u8` 形参时才发）。

    `cstr(s)` 用 0 号槽，`cstrAt(i, s)` 用第 i 个槽（一次调用里有多个字符串时按序给不同的 i）。
    指针在**下一次调用 cstr/cstrAt 之前**有效 —— 这正是 `effects Addr=0 Cont=0` 签的那句话；
    库要是把指针存起来，签字就是假的，包的判据该红。
    """
    return [
        '/* ---- 便利函数（自动生成）：把 `slice<u8>` 交给 C 的 `char *` ---- */',
        '// 模块自己的一块拷贝区（%d 槽 × %d 字节）。指针只在"下一次 cstr/cstrAt 之前"有效；' % (CSTR_SLOTS, CSTR_SLOT_BYTES),
        '// 这条限制就是 `effects Addr=0 Cont=0` 签的那句话。一次调用里有几个字符串，就按序用不同的槽。',
        '@private var CN_BUF: [%d]u8' % (CSTR_SLOTS * CSTR_SLOT_BYTES),
        '',
        'fn cstr(s: slice<u8>) -> ref u8 {',
        '    return cstrAt(0, s)',
        '}',
        '',
        'fn cstrAt(slot: i64, s: slice<u8>) -> ref u8 {',
        '    if slot < 0 || slot >= %d { trap("cstrAt: 槽越界（只有 %d 个）") }' % (CSTR_SLOTS, CSTR_SLOTS),
        '    if s.len + 1 > %d { trap("cstrAt: 字符串太长（一槽 %d 字节）") }' % (CSTR_SLOT_BYTES, CSTR_SLOT_BYTES),
        '    var base: i64 = slot * %d' % CSTR_SLOT_BYTES,
        '    var i: i64 = 0',
        '    while i < s.len {',
        '        CN_BUF[base + i] = s[i]',
        '        i = i + 1',
        '    }',
        '    CN_BUF[base + s.len] = u8(0)',
        '    return ref CN_BUF[base]',
        '}',
    ]


def main():
    ap = argparse.ArgumentParser(description='C 头文件 → extC 绑定（dl 那条路，零 effects）')
    # `@file`：把长命令放进一个参数文件（run.sh 里重生成 + --check 用同一份，不会漂）
    if len(sys.argv) > 1 and sys.argv[1].startswith('@'):
        lines = [l for l in open(sys.argv[1][1:]).read().split('\n') if not l.strip().startswith('#')]
        # 文件里的参数 + 命令行上跟在 `@file` 后面的（头文件路径按机器不同，由调用方给）
        sys.argv = ([sys.argv[0]] + shlex.split(' '.join(lines)) + sys.argv[2:])
    ap.add_argument('--include', action='append', default=[], help='要读的头文件（可多次）')
    ap.add_argument('-I', dest='incdirs', action='append', default=[], help='头文件搜索目录')
    ap.add_argument('--only', default='', help='要绑的函数（逗号分隔）；不给就得给 --all')
    ap.add_argument('--allow-skips', action='store_true',
                    help='`--all` 时默认打开：跳过的形状写进产物头部注释（--only 仍然是严格的：'
                         '点名要的绑不出来就失败）')
    ap.add_argument('--all', action='store_true', help='绑所有非 deprecated 的函数（大库会很大）')
    ap.add_argument('--no-enums', action='store_true', help='不生成枚举常量')
    ap.add_argument('--name', default='library', help='文档注释里的库名')
    ap.add_argument('--strip-prefix', default='auto',
                    help='字段名脱掉的前缀（`auto` = 最长公共前缀，如 cairo_）；C 原名留在注释里')
    ap.add_argument('-o', dest='out', required=True, help='产出的 .extc 路径')
    ap.add_argument('--check', action='store_true', help='只比较：生成物是否与 -o 一致')
    ap.add_argument('--extern', dest='externlib', default='',
                    help='声明式绑定：生成 `extern!("<lib>") fn …`（链接进二进制，不 dlopen）。'
                         '给的是 `extern!` 里的库名（如 extc-sqlite）')
    ap.add_argument('--effects', choices=('none', 'trusted'), default='none',
                    help='`--extern` 模式的签字策略：none（默认）或 trusted（一条"不留地址"的签字）')
    ap.add_argument('--link', action='append', default=[],
                    help='顺手写 `<out>.link` 的链接要求：`ccflag:<flag>` / `lib:<name>` / `pkgconfig:<name>`，'
                         '可重复。调用的程序因此一行链接参数都不用写')
    args = ap.parse_args()

    if not args.include:
        sys.exit('[cbindgen] 至少给一个 --include <头文件>')
    if not args.only and not args.all:
        sys.exit('[cbindgen] 给 --only a,b,c 或 --all')

    ast = clang_ast(args.include, args.incdirs)
    fns_all, enums_all, typedefs = {}, {}, {}
    collect(ast, fns_all, enums_all, typedefs,
            {os.path.realpath(p) for p in args.include})
    enum_names = set(enums_all)
    for tname, target in typedefs.items():
        if resolve(target, typedefs).startswith('enum'):
            enum_names.add(tname)
    enum_of_typedef = {t: resolve(v, typedefs).split(' ')[-1] for t, v in typedefs.items()
                       if resolve(v, typedefs).startswith('enum')}

    wanted = [n for n in args.only.split(',') if n] if args.only else sorted(fns_all)
    # 字段名：脱掉公共前缀（`cairo_paint` → `paint`），C 原名逐条留在注释里 —— 调用点更短，
    # 而 grep 还是能找到对应的 C 函数。
    prefix = ''
    if args.strip_prefix == 'auto' and len(wanted) > 1:
        cands = [n for n in wanted if '_' in n]
        if len(cands) == len(wanted):
            pre = os.path.commonprefix(wanted)
            prefix = pre[:pre.rfind('_') + 1] if '_' in pre else ''
    used_enums, out_fns, problems = [], [], []
    for name in wanted:
        fn = fns_all.get(name)
        if fn is None:
            problems.append('%s：头文件里没有这个声明' % name)
            continue
        if fn.get('variadic'):
            # 变参过不了 C 的 ABI（extC 的调用是静态的、参数个数编译期固定）⇒ 不猜、不静默丢 `...`
            problems.append('%s：变参函数（`...`）没能声明的形状 —— 包一层定参 helper，或从 --only 里去掉' % name)
            continue
        try:
            rq, _ = split_ret_args(fn.get('type', {}).get('qualType', ''))
            ret = map_type(rq, enum_names, True, typedefs)
            ps = []
            for p in params_of(fn):
                pt = p.get('type', {}).get('qualType', '')
                for e in enum_names:                      # 这个签名用到了哪些枚举
                    if e in pt:
                        used_enums.append(enum_of_typedef.get(e, e))
                ps.append((p.get('name') or 'arg%d' % len(ps),
                           map_type(pt, enum_names, False, typedefs)))
        except Unsupported as exc:
            problems.append('%s：%s' % (name, exc))
            continue
        # 参数名带进注释里没有意义（extC 的 fn 类型只有类型），但**校验名字合法**：
        # 生成物要能编译，参数名里有 `_n` 这类没关系（我们用不到），所以只留类型。
        sig = 'fn(%s) -> %s' % (', '.join(t for _, t in ps), ret)
        short = name[len(prefix):] if prefix and name.startswith(prefix) else name
        # 模块自己的名字（`open` 填充器、`need` 取符号、`api` 表）不能被字段名撞掉 ——
        # `demo_open` 脱前缀正好会变成 `open`，实测报 "duplicate function `open`"。
        if short in ('open', 'need', 'api'):
            short = name
        out_fns.append((short, name, sig))

    enums = {}
    if not args.no_enums and used_enums:
        for e in dict.fromkeys(used_enums):
            if e in enums_all:
                enums[e] = enums_all[e]

    if not args.no_enums and args.all:
        enums = enums_all
    # 隐式值（JSON 里没有）交给 clang 求值；还拿不到就留 None（emit 里跳过 —— 不猜）。
    missing = [n for _e, vals in enums.items() for n, v in vals if v is None]
    if missing:
        got = eval_constants(args.include, args.incdirs, missing)
        enums = {e: [(n, (v if v is not None else got.get(n))) for n, v in vals]
                 for e, vals in enums.items()}
        for n in missing:
            if n not in got:
                sys.stderr.write('[cbindgen] 枚举常量 %s 的值没求出来：跳过（不猜）\n' % n)

    if args.externlib:
        decls = []
        for short, cname, _sig in out_fns:
            fn = fns_all[cname]
            rq, _ = split_ret_args(fn.get('type', {}).get('qualType', ''))
            ret = map_type(rq, enum_names, True, typedefs)
            params, seq, outs, note = [], [], [], None
            for p in params_of(fn):
                pt = p.get('type', {}).get('qualType', '')
                nm = p.get('name') or 'arg%d' % len(params)
                if is_out_pointer(pt):
                    outs.append((nm, pt))
                    seq.append(('o', nm, pt))
                    continue
                mt = map_type(pt, enum_names, False, typedefs)
                params.append((nm, mt))
                seq.append(('p', nm, mt, pt))  # shim 的形参用绑定里的 C 类型，调用处再按头文件的写法转回去
            # 出参只能走 shim：恰好一个 `T **` 且返回整数状态；其余形状说清楚为什么没绑。
            if outs and not (len(outs) == 1 and ret in INT_RET):
                note = '%s：有 %d 个 `T **` 出参、返回 %s —— 自动 shim 只处理"一个出参 + 整数返回"' \
                       % (cname, len(outs), ret)
                outs = []
            decls.append((cname, ret, params, seq, outs, note))
        text, shims = emit_extern(decls, enums, args.externlib, args.effects)
    else:
        text = emit(out_fns, enums, args.name)
        shims = []
    for p in problems:
        sys.stderr.write('[cbindgen] 跳过：%s\n' % p)

    if args.allow_skips or args.all:
        skipped = [p for p in problems if '头文件里没有这个声明' not in p]
        if skipped:
            text = text.replace('// 由 tools/cbindgen.py 生成 —— 不要手改。\n',
                                '// 由 tools/cbindgen.py 生成 —— 不要手改。\n'
                                '//\n// 没绑的形状（%d 个，需要时手写 shim）：\n' % len(skipped)
                                + ''.join('//   · %s\n' % p for p in skipped))
            problems = [p for p in problems if '头文件里没有这个声明' in p]

    if args.check:
        try:
            cur = open(args.out).read()
        except FileNotFoundError:
            sys.exit('[cbindgen] %s 不存在；先跑一次生成' % args.out)
        if cur != text:
            sys.exit('[cbindgen] %s 与头文件不一致：重新生成' % args.out)
        print('ok  %s 与头文件一致（%d 个函数）' % (args.out, len(out_fns)))
        return
    if problems:
        sys.exit('[cbindgen] %d 个函数没法生成（上面列了原因）：修或从 --only 里去掉' % len(problems))
    with open(args.out, 'w') as f:
        f.write(text)
    if shims:
        base = args.out[:-5] if args.out.endswith('.extc') else args.out
        with open(base + '.shim.c', 'w') as f:
            f.write(emit_out_shims(shims, args.include, args.incdirs))
        # shim 的 `.c` 要排在库前面：cc 从左到右扫，静态库要先看到引用它的目标文件。
        args.link = (['ccflag:-I%s' % os.path.abspath(d) for d in args.incdirs]
                     + ['ccflag:%s' % os.path.abspath(base + '.shim.c')]
                     + list(args.link))
        print('生成 %s.shim.c：%d 个出参函数' % (base, len(shims)))
    if args.link:
        if not args.externlib:
            sys.exit('[cbindgen] `--link` 只用于 `--extern`（声明式绑定）：dl 那条路由 `api::open` 自己找库')
        base = args.out[:-5] if args.out.endswith('.extc') else args.out
        with open(base + '.link', 'w') as f:
            f.write('# 由 tools/cbindgen.py 生成 —— 这是 `use` 这个模块的程序会自动带上的链接要求。\n')
            for spec in args.link:
                kind, _, arg = spec.partition(':')
                if kind not in ('ccflag', 'lib', 'pkgconfig') or not arg:
                    sys.exit('[cbindgen] --link 要写成 `ccflag:<flag>` / `lib:<name>` / `pkgconfig:<name>`，收到 `%s`' % spec)
                f.write('%s %s\n' % (kind, arg))
        print('生成 %s.link：%d 条链接要求' % (base, len(args.link)))
    print('生成 %s：%d 个函数%s' % (args.out, len(out_fns),
                                '，%d 组枚举' % len(enums) if enums else ''))


if __name__ == '__main__':
    main()

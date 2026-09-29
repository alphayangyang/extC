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


def collect(node, fns, enums, typedefs):
    """走一遍 AST，收下函数声明与枚举常量。"""
    if isinstance(node, dict):
        kind = node.get('kind')
        if kind == 'FunctionDecl' and node.get('name'):
            fns.setdefault(node['name'], node)
        elif kind == 'TypedefDecl' and node.get('name'):
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
            collect(v, fns, enums, typedefs)
    elif isinstance(node, list):
        for v in node:
            collect(v, fns, enums, typedefs)


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
    lines.append('// 缺符号是**逐个人工判空**的：extC 的 `!` 是纯编译期签名（没有运行期检查，实测 `null!`')
    lines.append('// 拿到 0），靠它就会得到一张全是空指针的表、第一次调用跳 null。')
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
    ap.add_argument('--all', action='store_true', help='绑所有非 deprecated 的函数（大库会很大）')
    ap.add_argument('--no-enums', action='store_true', help='不生成枚举常量')
    ap.add_argument('--name', default='library', help='文档注释里的库名')
    ap.add_argument('--strip-prefix', default='auto',
                    help='字段名脱掉的前缀（`auto` = 最长公共前缀，如 cairo_）；C 原名留在注释里')
    ap.add_argument('-o', dest='out', required=True, help='产出的 .extc 路径')
    ap.add_argument('--check', action='store_true', help='只比较：生成物是否与 -o 一致')
    args = ap.parse_args()

    if not args.include:
        sys.exit('[cbindgen] 至少给一个 --include <头文件>')
    if not args.only and not args.all:
        sys.exit('[cbindgen] 给 --only a,b,c 或 --all')

    ast = clang_ast(args.include, args.incdirs)
    fns_all, enums_all, typedefs = {}, {}, {}
    collect(ast, fns_all, enums_all, typedefs)
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

    text = emit(out_fns, enums, args.name)
    for p in problems:
        sys.stderr.write('[cbindgen] 跳过：%s\n' % p)

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
    print('生成 %s：%d 个函数%s' % (args.out, len(out_fns),
                                '，%d 组枚举' % len(enums) if enums else ''))


if __name__ == '__main__':
    main()

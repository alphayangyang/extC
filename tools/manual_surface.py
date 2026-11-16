#!/usr/bin/env python3
"""列出标准库的**公开面**：应当被手册逐项覆盖的成员。

来源是每个模块的 `struct` 体、`impl` 块与顶层声明：成员**缺省公开**，源码写 `@private` 才是内部。
输出 `tools/manual-surface.txt`（`模块<TAB>类型<TAB>成员<TAB>种类<TAB>visibility`），按模块排序。

    tools/manual_surface.py           重新生成
    tools/manual_surface.py --check   与清单不一致则失败（check.sh 调用）
    tools/manual_surface.py --worklist   列出"名字像内部、但源码没写 `@private`"的成员（收口清单）
"""
import re, sys, glob, os

# 名称启发式：**只**给 `--worklist` 用。它找出"按名字看是内部齿轮、但源码没写 `@private`"的成员，
# 供一次性收口；分类的真凭据是源码里的 `@private`，这里不做判断。
INTERNAL_PREFIX = re.compile(r'^p[A-Z]')
INTERNAL_NAMES = {'bn', 'childSlot', 'atDense', 'borrowLeft', 'borrowRight', 'accDigit',
                  'kd', 'leafOf', 'nth',
                  'buf', 'small', 'big',
                  'pid', 'pidGen', 'vals', 'slots', 'ent', 'cursor', 'holeHead', 'epoch',
                  'nnodes', 'root', 'kids', 'keys', 'tags', 'n', 'cap', 'freeHead'}
INTERNAL_MACHINERY = set("""
kn vn nx nk cnt cntOf setCnt setKey setVal setKid setNext setN newNode freeNode dropChild
nodeCount nodes growStores ncap firstKey lastKey leaf leafFor lowerIn mergeLeft mergeRight
side subMinKey height kidsCnt bucketOfDense keyAtDense valAtDense dead deadOf
denseLen liveAt pidOf tagCensus ep gen gather handleAtDense chunk got fillState isSpaceByte
newlineAfter nextI64State pairsReady errWrite flushCout fmtI64 fmtI64Fast rawbuf saved inc""".split())


# 手册里**明确写成"存储字段、不是 API"**的那一类：它们是公开的（子系统代码按位置读写、
# 文档示例也会读），但名字看起来像内部齿轮。`--worklist` 不再提示它们。
DOCUMENTED_STORAGE = {'vals', 'depth', 'chunk', 'len', 'pos', 'slot', 'gen', 'epoch',
                      'fd', 'buf', 'n', 'cap', 'pid', 'pidGen', 'big', 'small'}


# 容器子系统内部共用、且手册"存储字段"那段一并交代过的成员（P2 合并容器时会消失）：
SHARED_CONTAINER_INTERNALS = {'atDense', 'setAtDense', 'valAtDense', 'liveAt', 'handleAtDense',
                              'bucketOfDense', 'keyAtDense', 'pairsReady', 'nextI64State', 'flushCout', 'fillBytes', 'padText', 'fmtF', 'fmtI64Fast', 'fmtU64Fast', 'fmtU64', 'errWrite', 'pPadText', 'pFillBytes'}


def looks_internal(name):
    if name in DOCUMENTED_STORAGE or name in SHARED_CONTAINER_INTERNALS:
        return False
    return bool(INTERNAL_PREFIX.match(name)) or name in INTERNAL_NAMES or name in INTERNAL_MACHINERY


def blank_comments(src):
    src = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), src, flags=re.S)
    return re.sub(r'//[^\n]*', lambda m: ' ' * (m.end() - m.start()), src)


def line_of(src, idx):
    return src.count('\n', 0, idx) + 1


def has_private_before(src, idx):
    """`@private` 贴在声明同一行，或独占上一行。"""
    line_start = src.rfind('\n', 0, idx) + 1
    same = src[line_start:idx]
    if re.fullmatch(r'[ \t]*@private[ \t]+', same) or re.fullmatch(r'[ \t]*@private[ \t]*', same):
        return True
    if same.strip():
        return False
    prev_end = line_start - 1
    prev_start = src.rfind('\n', 0, prev_end) + 1
    return src[prev_start:prev_end].strip() == '@private'


def body_of(src, open_brace):
    """从 `{` 处配对到它的 `}`，返回 (起始下标, 体区间结束下标)。

    跳过字符串/字符字面量：源码里 `f"{x}"` 的大括号会打断朴素的括号计数，一旦打断，
    结构体的"体"会延伸到后面的函数体，那些结构体字面量的字段就会被当成声明。
    """
    i, depth = open_brace + 1, 1
    while i < len(src) and depth:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < len(src) and src[i] != q:
                i += 2 if src[i] == '\\' else 1
            i += 1
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        i += 1
    return open_brace + 1, i - 1


def top_level_lines(src, start, end):
    """体区间里**深度 0**的行（即类型体的直接成员，而不是方法体里的字面量/局部）。

    这里看的是**行首**深度，不是行尾：一个多行函数的签名行以 `{` 结尾，行尾深度已经是 1，
    若按行尾判就会被整条漏掉（单行函数体 `{ return x }` 反而会被记下）—— 那会让公开面清单
    只认单行成员。字符串/字符字面量里的括号照旧不计数。

    返回 [(行首下标, 该行文本)]。
    """
    out = []
    i, depth, line_start, line_depth = start, 0, start, 0
    while i < end:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < end and src[i] != q:
                i += 2 if src[i] == chr(92) else 1
            i += 1
            continue
        if c == chr(10):
            if line_depth == 0:
                out.append((line_start, src[line_start:i]))
            line_start = i + 1
            line_depth = depth
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        i += 1
    if line_depth == 0 and line_start < end:
        out.append((line_start, src[line_start:end]))
    return out


def members(path, rel):
    src = blank_comments(open(path, encoding='utf-8').read())
    out = []
    # struct 体：字段与方法，只看体区间的**直接**行（跳过方法体里的字面量）
    for m in re.finditer(r'^struct\s+(\w+)', src, re.M):
        # 从 `{` 的**下一个**字符开始扫描：否则这个开括号会被数两次，结构体的"体"就一直
        # 延伸到下一个同层 `}` —— 后面 struct 之前的自由函数、甚至下一个 struct 的字段
        # 都会被算成它的成员（实测：`std::json` 的 `parse`/`member` 挂到了 `parser` 上）。
        ob = src.index('{', m.end())
        start, end = body_of(src, ob)
        for idx, text in top_level_lines(src, start, end):
            fm = re.match(r'[ \t]*(@private[ \t]+)?([a-z]\w*)\s*:\s*(?!:)', text)
            if fm:
                priv = bool(fm.group(1)) or has_private_before(src, idx)
                out.append((m.group(1), fm.group(2), 'field', priv, line_of(src, idx)))
                continue
            mm = re.match(r'[ \t]*(@private[ \t]+)?fn\s+(\w+)\s*\(', text)
            if mm:
                priv = bool(mm.group(1)) or has_private_before(src, idx)
                out.append((m.group(1), mm.group(2), 'method', priv, line_of(src, idx)))
    # impl 块：只可能是方法
    for m in re.finditer(r'^(?:@private[ \t]+)?impl\s+([\w:]+)\s*\{', src, re.M):
        ob = src.index('{', m.end())
        start, end = body_of(src, ob)
        for idx, text in top_level_lines(src, start, end):
            mm = re.match(r'[ \t]*(@private[ \t]+)?fn\s+(\w+)\s*\(', text)
            if mm:
                priv = bool(mm.group(1)) or has_private_before(src, idx)
                out.append((m.group(1), mm.group(2), 'method', priv, line_of(src, idx)))
    # 顶层**自由函数**：同样是可供使用者调用的公开面（`sort<T>`、`copyInto<T>` 一类）。
    # 约定：结构体体与 impl 块内的成员缩进书写，因此列首的 `fn` 即顶层声明。
    for fm in re.finditer(r'(?m)^(@private[ \t]+)?fn\s+(\w+)\s*\(', src):
        idx = fm.start()
        priv = bool(fm.group(1)) or has_private_before(src, idx)
        out.append(('-', fm.group(2), 'fn', priv, line_of(src, idx)))
    return [(rel, t, n, k, 'internal' if p else 'public', ln) for t, n, k, p, ln in out]


rows = []
for f in sorted(glob.glob('stdlib/**/*.extc', recursive=True)):
    rows += members(f, os.path.relpath(f, 'stdlib')[:-5].replace(os.sep, '::'))
rows.sort()

if '--worklist' in sys.argv:
    bad = [r for r in rows if r[4] == 'public' and looks_internal(r[2])]
    for rel, _t, name, kind, _v, ln in bad:
        print('stdlib/%s:%d\t%s\t%s' % (rel.replace('::', '/'), ln, name, kind))
    print('看起来是内部但没写 `@private` 的成员：%d 个' % len(bad), file=sys.stderr)
    sys.exit(0)

text = ('# Generated by tools/manual_surface.py -- do not edit by hand.\n'
        '# 标准库成员按源码里的 `@private` 分类：public 应被手册覆盖，internal 列入透明清单。\n'
        '# module<TAB>type<TAB>member<TAB>kind<TAB>visibility\n'
        + ''.join('%s\t%s\t%s\t%s\t%s\n' % r[:5] for r in rows))


def internals_page():
    by = {}
    for mod, ty, name, kind, vis, _ln in rows:
        if vis != 'internal':
            continue
        by.setdefault(mod, []).append((ty, name, kind))
    out = ['# 内部成员一览（透明清单）', '',
           '标准库成员**缺省公开**，源码写 `@private` 才是内部。本页列出这些内部成员：它们跨模块不可见，',
           '也不受兼容性承诺保护，重构时不预告。', '',
           '生成方式：`tools/manual_surface.py`；`check.sh` 保证本页与源码一致。', '',
           '公开面共 %d 个成员，其中内部 %d 个：' % (len(rows), sum(1 for r in rows if r[4] == 'internal')), '']
    for mod in sorted(by):
        out.append('## %s' % mod)
        out.append('')
        for ty, name, kind in sorted(by[mod]):
            out.append('- `%s.%s`（%s）' % (ty, name, '字段' if kind == 'field' else '方法'))
        out.append('')
    return '\n'.join(out) + '\n'


page = internals_page()
PAGE = 'docs/usage/surface.md'
if '--check' in sys.argv:
    cur = open('tools/manual-surface.txt', encoding='utf-8').read() if os.path.exists('tools/manual-surface.txt') else ''
    curp = open(PAGE, encoding='utf-8').read() if os.path.exists(PAGE) else ''
    if cur != text or curp != page:
        print('公开面清单或 docs/usage/surface.md 与源码不一致 -- 跑 tools/manual_surface.py')
        sys.exit(1)
    print('ok  清单与源码一致（%d 个成员：公开 %d · 内部 %d）' % (
        len(rows), sum(1 for r in rows if r[4] == 'public'), sum(1 for r in rows if r[4] == 'internal')))
    sys.exit(0)
open('tools/manual-surface.txt', 'w', encoding='utf-8').write(text)
open(PAGE, 'w', encoding='utf-8').write(page)

bymod = {}
for rel, _t, _n, _k, v, _ln in rows:
    if v == 'public':
        bymod[rel] = bymod.get(rel, 0) + 1
pub = sum(bymod.values())
print('成员共 %d 个：公开 %d · 内部 %d' % (len(rows), pub, len(rows) - pub))
for rel, c in sorted(bymod.items(), key=lambda kv: -kv[1])[:8]:
    print('  %-16s %3d 公开' % (rel, c))

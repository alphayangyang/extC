#!/usr/bin/env python3
"""extC 的变异 fuzzer：oracle = ①编译器不崩 ②不挂死 ③说成功就必须产出合法 C ④跑的过的过 sanitizer。

用法：
    python3 tools/fuzz.py --iters 2000 --seed 1 [--jobs 4] [--keep-going]
失败会落到 --out（默认 `~/extc-fuzz`），每个失败一个目录：case.extc / case.c / log.txt。
同一个 seed 必然复现同一条用例（变异序列由 seed 与迭代号共同决定）。

**不要放在 /tmp**：那里常常是 tmpfs（内存盘），几轮战役就能把它写满，而写满之后连 bash
都起不来（它的暂存也在 /tmp）—— 2026-09-28 就这样卡住过一次，见
docs/topics/HARDENING.md 第四节。
"""
import argparse, os, random, re, shutil, subprocess, sys, hashlib
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODE = ['mutate']   # 由 --mode 设置；线程里只读
EXTC = os.environ.get('EXTC', os.path.join(ROOT, 'build', 'extc'))

def corpus(limit):
    out = []
    for d in ('examples', 'tests', 'bench'):
        for dirpath, _, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if f.endswith('.extc'):
                    p = os.path.join(dirpath, f)
                    try:
                        if os.path.getsize(p) < 4000: out.append(p)
                    except OSError: pass
    out.sort()
    return out[:limit] if limit else out

TOK = re.compile(r'[A-Za-z_][A-Za-z_0-9]*|[0-9]+(?:\.[0-9]+)?|==|!=|<=|>=|->|::|\.\.|\S')
def tokens(s): return TOK.findall(s)

def split_src(s):
    """把源码切成 (间隔, token) 序列 —— 间隔里保留换行与缩进，所以变异只动 token 本身，
    行结构不被破坏（一开始用空格重排，几乎全变成语法错，根本走不到 codegen）。"""
    parts, last = [], 0
    for m in TOK.finditer(s):
        parts.append([s[last:m.start()], m.group(0)])
        last = m.end()
    parts.append([s[last:], ''])
    return parts

def join_src(parts):
    return ''.join(g + t for g, t in parts)

TYPES = ('i64','i32','i16','i8','u64','u32','u16','u8','bool','f64','f32','void')
LITS  = ('0','1','2','3','255','256','-1','9223372036854775807','18446744073709551615','1e400')

def mutate(src, rng, rounds):
    """变异偏向"原地换 token"（保持可编译，从而打到语义/codegen 深处），少量结构性改动。
    返回 (变异后的源码, 变异次数)。"""
    parts = split_src(src)
    toks  = [t for _, t in parts]
    idents = [t for t in toks if re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', t)]
    n = 0
    for _ in range(rounds):
        if len(parts) < 4: break
        op = rng.randrange(10)
        i  = rng.randrange(len(parts) - 1)
        if op < 3 and idents:                                    # 标识符换成文件里另一个
            parts[i][1] = rng.choice(idents)
        elif op < 5:                                             # 类型名互换
            parts[i][1] = rng.choice(TYPES)
        elif op < 7:                                             # 字面量换成边界值
            parts[i][1] = rng.choice(LITS)
        elif op == 7:                                            # 删 token
            parts[i][1] = ''
        elif op == 8:                                            # 复制 token（就地）
            parts.insert(i, [' ', parts[i][1]])
        else:                                                    # 就地插入关键字/符号
            parts.insert(i, [' ', rng.choice(
                ['@unchecked','ref','mut','?','(',')','{','}','[',']',',',';','return','if',
                 'while','match','impl','trait','dyn','yield','new','null','0',''])])
        n += 1
    return join_src(parts), n


# ---------------------------------------------------------------- 生成式（--mode gen）
#
# 变异 fuzzer 只能走到"已有语料附近"的地方；生成式从语法出发造**又大又合法**的程序，专门压
# codegen 与优化通道。round 10 起形状**随机化**（结构体/枚举数量与字段、trait 与 impl 的组合、
# 是否用 dyn / 池 / 协程、函数体语句数都按种子变化），不再是固定骨架 —— 固定骨架跑几轮就把
# 组合空间走完了。

SCALARS = ['i64', 'i32', 'u8', 'f64', 'bool']
INT_T   = ['i64', 'i32', 'u8']
SMALL   = ['0', '1', '2', '3', '7', '42', '255']

def gen_expr(rng, vars_, depth=0):
    """表达式：优先已声明的变量，其次字面量/二元/比较/字段。字面量保持在窄类型范围内，
    免得一上来就 trap（trap 不是问题，但会遮住后面的代码）。"""
    ch = ['lit', 'lit']
    if vars_: ch += ['var', 'var', 'var']
    if depth < 2: ch += ['bin', 'cmp']
    k = rng.choice(ch)
    if k == 'lit':
        t = rng.choice(INT_T)
        return '%s(%s)' % (t, rng.choice(SMALL))
    if k == 'var':
        return rng.choice(vars_)
    if k == 'bin':
        return '%s %s %s' % (gen_expr(rng, vars_, depth + 1),
                             rng.choice(['+', '-', '*']), gen_expr(rng, vars_, depth + 1))
    return '%s %s %s' % (gen_expr(rng, vars_, depth + 1),
                         rng.choice(['<', '<=', '>', '>=', '==', '!=']),
                         gen_expr(rng, vars_, depth + 1))

def gen_body(rng, vars_, ind, depth=0):
    pad = '    ' * ind
    out = []
    for _ in range(rng.randrange(1, 4)):
        k = rng.randrange(7)
        if k <= 1 and vars_:
            out.append('%s%s = %s' % (pad, rng.choice(vars_), gen_expr(rng, vars_)))
        elif k == 2:
            t = rng.choice(INT_T); nm = 'v%d' % rng.randrange(10000)
            out.append('%svar %s: %s = %s' % (pad, nm, t, gen_expr(rng, vars_)))
            vars_.append(nm)
        elif k == 3 and vars_ and depth < 2:
            out.append('%sif %s {' % (pad, gen_expr(rng, vars_)))
            inner = list(vars_)
            out += gen_body(rng, inner, ind + 1, depth + 1)
            out.append('%s}' % pad)
        elif k == 4 and depth < 2:
            c = 'c%d' % rng.randrange(10000)
            out += ['%svar %s: i64 = 0' % (pad, c),
                    '%swhile %s < i64(%d) {' % (pad, c, rng.randrange(1, 4))]
            inner = list(vars_)
            out += gen_body(rng, inner, ind + 1, depth + 1)
            out += ['%s    %s = %s + i64(1)' % (pad, c, c), '%s}' % pad]
            vars_.append(c)
        elif k == 5:
            out.append('%sio::cout << %s << "\\n"' % (pad, gen_expr(rng, vars_)))
        else:
            out.append('%sfor i%d in 0..%d { %s = %s + i64(1) }'
                       % (pad, rng.randrange(10000), rng.randrange(1, 4),
                          rng.choice(vars_) if vars_ else 'v0',
                          rng.choice(vars_) if vars_ else 'v0'))
    return out

def gen_program(rng):
    """按种子随机组合：结构体（含泛型）、trait 与多个 impl（含内建实例）、枚举 + match、
    协程（带参数）、可选地 dyn 与池、若干函数与 main。"""
    L = ['use std::io']
    nstruct = rng.randrange(1, 3)
    names = ['s%d' % i for i in range(nstruct)]
    for nm in names:
        nf = rng.randrange(1, 4)
        L.append('struct %s { %s }' % (nm, '  '.join('%s: %s' % (chr(97 + i), rng.choice(SCALARS))
                                                     for i in range(nf))))
    L.append('struct box<T> { v: T }')
    L.append('impl<T> box<T> { fn get(self: ref box<T>) -> T { return self.v } }')
    if rng.random() < 0.7:
        L.append('impl slice<u8> { fn total(self: ref slice<u8>) -> i64 {')
        L += ['    var s: i64 = 0', '    for i in 0..self.len { s = s + i64(self[i]) }',
              '    return s } }']
    ntraits = rng.randrange(0, 2)
    traits = []
    implOn0 = False
    for ti in range(ntraits):
        tn = 'T%d' % ti
        traits.append(tn)
        L.append('trait %s { fn tf%d(self: ref Self) -> i64 }' % (tn, ti))
        for nm in rng.sample(names, rng.randrange(1, len(names) + 1)):
            L.append('impl %s for %s { fn tf%d(self: ref %s) -> i64 { return i64(%d) } }'
                     % (tn, nm, ti, nm, rng.randrange(0, 10)))
            if nm == names[0]: implOn0 = True
    if rng.random() < 0.6:
        L.append('type e0 = | none | some(i64)')
        L.append('fn pick(x: e0) -> i64 { match x { none => { return i64(0) }')
        L.append('    some(v) => { return v } } }')
    nfun = rng.randrange(1, 4)
    for fi in range(nfun):
        body = gen_body(rng, ['a', 'b'], 1)
        L += ['fn fn%d(a: i64, b: i64) -> i64 {' % fi] + body + ['    return a + b', '}']
    L.append('fn counter(n: i64) -> coroutine<i64> { var i: i64 = 0')
    L.append('    while i < n { yield i * i64(2)')
    L.append('        i = i + i64(1) } }')
    M = ['fn main() -> i32 {']
    for i, nm in enumerate(names):
        M.append('    var sx%d: %s' % (i, nm))
    M.append('    var g: box<i64>')
    M.append('    g.v = i64(5)')
    if implOn0:
        M.append('    var t0: i64 = sx0.tf0()')
    M.append('    var arr: [3]i64 = [1, 2, 3]')
    M.append('    var sl: slice<i64> = arr[..]')
    M.append('    var acc: i64 = i64(sl.len) + g.get() + fn0(i64(1), i64(2))')
    M.append('    var c: coroutine<i64> = counter(i64(3))')
    M.append('    while c.next() { acc = acc + c.value() }')
    if rng.random() < 0.6:
        M.append('    match e0::some(acc) { none => { acc = i64(0) }')
        M.append('        some(v) => { acc = v + pick(e0::some(v)) } }')
    if rng.random() < 0.4 and ntraits:
        M.append('    var d: dyn %s = dyn %s(sx0)' % (traits[0], traits[0]))
        M.append('    acc = acc + d.tf0()')
    M.append('    io::cout << acc << "\\n"')
    M.append('    return 0')
    M.append('}')
    return '\n'.join(L + M) + '\n'

def run(cmd, timeout, **kw):
    return subprocess.run(cmd, capture_output=True, timeout=timeout, **kw)

def one(args):
    path, seed, it, out = args
    rng = random.Random((seed << 20) ^ it)
    try: src = open(path, encoding='utf-8', errors='replace').read()
    except OSError: return None
    case = gen_program(rng) if MODE[0] == 'gen' else mutate(src, rng, rng.randrange(1, 6))[0]
    d = os.path.join(out, 'w%05d' % it)   # 每次迭代独占目录：32 个共享目录会让并发互相删掉对方的产物
    os.makedirs(d, exist_ok=True)
    if MODE[0] == 'modules':
        # 模块级：把源文件**所在目录**整个拷进来（`use a::b` 要能找到兄弟文件），只变异其中一个。
        # 模块解析是独立的一条代码路径，单文件变异永远走不到。
        shutil.copytree(os.path.dirname(path), d, dirs_exist_ok=True)
        extc_f = os.path.join(d, os.path.basename(path))
        open(extc_f, 'w', encoding='utf-8').write(case)
    else:
        extc_f = os.path.join(d, 'case.extc')
        open(extc_f, 'w', encoding='utf-8').write(case)
    c_f = os.path.join(d, 'case.c'); exe = os.path.join(d, 'case')
    why = None; log = []
    try:
        r = run([EXTC, '-w', '--no-line-map', '-o', c_f, extc_f], 20)
    except subprocess.TimeoutExpired:
        why = 'extC 挂死'; log.append('timeout 20s')
    if why is None:
        if r.returncode < 0 or r.returncode >= 128:
            why = 'extC 崩溃'; log.append('rc=%d' % r.returncode); log.append(r.stderr.decode('utf-8','replace')[-2000:])
        elif r.returncode == 0:
            log.append('extC OK')
            g = run(['gcc', '-std=c11', '-fsyntax-only', c_f], 20)
            if g.returncode != 0:
                why = '生成的 C 非法'; log.append(g.stderr.decode('utf-8','replace')[-2000:])
            else:
                ge = run(['gcc', '-O0', '-g', '-std=c11', '-fwrapv', '-fsanitize=address,undefined',
                          '-fno-sanitize-recover=all', '-o', exe, c_f], 60)
                if ge.returncode == 0:
                    try:
                        # `allocator_may_return_null=1`: ASan aborts on a **huge allocation
                        # request** by default, so a program that deliberately asks for 1PB to
                        # make `malloc` fail (`tests/traps/arena_oom.extc`) never gets the NULL the
                        # runtime is supposed to handle -- and the oracle reported the compiler for
                        # ASan's own complaint. With this the runtime traps as designed.
                        env = dict(os.environ, ASAN_OPTIONS='allocator_may_return_null=1')
                        p = run([exe], 5, env=env)
                        err = p.stderr.decode('utf-8', 'replace')
                        # `ERROR: AddressSanitizer` 才算，`WARNING:` 不算 —— 后者是
                        # "failed to allocate N bytes"（配 allocator_may_return_null=1 就是
                        # 正常现象），曾经把 tests/traps/arena_oom.extc 又误报了一次。
                        hit = [l for l in err.splitlines()
                               if 'runtime error' in l or 'ERROR: AddressSanitizer' in l]
                        if hit:
                            why = 'sanitizer 报告（UB）'; log.append(hit[0])
                    except subprocess.TimeoutExpired:
                        pass          # 死循环的变异很常见，不算问题
    compiled_ok = (why is None) or (why not in ('extC 崩溃', 'extC 挂死', '生成的 C 非法'))
    if why is None:
        shutil.rmtree(d, ignore_errors=True)
        return (None, compiled_ok)
    # 目录名带上 mode 与 seed：只用迭代号会让**不同片之间撞名互相覆盖**（战役5 日志里
    # 3 片各有 1 条失败，却只存下 2 个用例，丢的那个就是这么没的）。
    keep = os.path.join(out, 'fail-%s-s%d-%05d' % (MODE[0], seed, it))
    os.makedirs(keep, exist_ok=True)
    shutil.move(extc_f, os.path.join(keep, 'case.extc'))
    for f, n in ((c_f, 'case.c'),):
        if os.path.exists(f): shutil.move(f, os.path.join(keep, n))
    open(os.path.join(keep, 'log.txt'), 'w').write('mode=%s seed=%d it=%d src=%s\n%s\n%s\n' %
                                                   (MODE[0], seed, it, path, why, '\n'.join(log)))
    shutil.rmtree(d, ignore_errors=True)
    return ((it, path, why), compiled_ok)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iters', type=int, default=200)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--mode', choices=['mutate', 'gen', 'modules'], default='mutate')
    ap.add_argument('--dry-run', type=int, default=0, help='只打印 N 个生成结果，不调用编译器')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--limit', type=int, default=0, help='语料文件数上限')
    ap.add_argument('--out', default=os.path.expanduser('~/extc-fuzz'),
                    help='失败用例的落盘目录（默认 ~/extc-fuzz；别用 /tmp，见模块文档）')
    ap.add_argument('--timeout', type=int, default=1800)
    a = ap.parse_args()
    MODE[0] = a.mode
    if a.dry_run:
        for i in range(a.dry_run):
            print(''.join('=' for _ in range(20)), 'sample', i, ''.join('=' for _ in range(20)))
            print(gen_program(random.Random(a.seed + i)))
        return 0
    files = corpus(a.limit)
    if not files: print('没有语料'); return 2
    os.makedirs(a.out, exist_ok=True)
    print('语料 %d 个 · 迭代 %d · seed %d · 并发 %d ⇒ %s' % (len(files), a.iters, a.seed, a.jobs, a.out))
    t = ThreadPoolExecutor(max_workers=a.jobs)
    todo = [(files[(a.seed + i) % len(files)], a.seed, i, a.out) for i in range(a.iters)]
    done = 0; fails = []
    compiled = 0
    for res in t.map(one, todo):
        done += 1
        if res is None: continue
        payload, ok = res
        if ok: compiled += 1
        if payload: fails.append(payload); print('  ✗ it=%d %s ← %s' % payload, flush=True)
        if done % 100 == 0:
            print('  ... %d/%d · 变异后仍编译成功 %d (%.0f%%) · 失败 %d'
                  % (done, a.iters, compiled, 100.0*compiled/max(done,1), len(fails)), flush=True)
    print('完成 %d 次迭代 · 变异后仍编译成功 %d (%.0f%%) · 失败 %d 条'
          % (done, compiled, 100.0*compiled/max(done,1), len(fails)))
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())

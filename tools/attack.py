#!/usr/bin/env python3
"""定向攻击套件：每组题都带**期望**（ok / reject / trap），再叠三条硬 oracle：
编译器不许崩 ✗、说成功就必须产出合法 C ✗、能跑的过 ASan+UBSan ✗。

和 tools/fuzz.py 的分工：fuzzer 撒网找未知，这里**按已知实现结构点名攻击**。第一组是
**泛型**（单态化 / ttSubstitute / 每实例方法集 / 延迟检查 / 实例缓存与命名）。

用法：
    python3 tools/attack.py [组名…]        # 默认全部；组名如 generics
失败退出码非零，并打印每条的期望、实得与细节。
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTC = os.environ.get('EXTC', os.path.join(ROOT, 'build', 'extc'))
WORK = os.environ.get('EXTC_ATTACK_WORK', os.path.expanduser('~/extc-work/attack'))

# kind: ok = 必须编译并成功运行；reject = 必须被编译期挡住（可给 want 子串）；
#       trap = 必须运行期 trap（带源码位置）；ok_or_reject = 两种都接受，只禁止崩/非法C/UB
GENERICS = [
    # ---- A. 实例化身份与命名 ----
    ('A1 同一实例两处使用只生成一份', 'ok',
     'struct box<T> { v: T }\nfn f(a: box<i64>) -> i64 { return a.v }\nfn g(b: box<i64>) -> i64 { return b.v }\n'
     'fn main() -> i32 { var x: box<i64>\n  x.v = i64(1)\n  return i32(f(x) + g(x)) }'),
    ('A2 嵌套实例 box<box<i64>>', 'ok',
     'struct box<T> { v: T }\nfn main() -> i32 { var i: box<i64>\n  i.v = i64(1)\n  var o: box<box<i64>>\n'
     '  o.v = i\n  return i32(o.v.v) }'),
    ('A3 三层嵌套', 'ok',
     'struct box<T> { v: T }\nfn main() -> i32 { var a: box<i64>\n  a.v = i64(2)\n  var b: box<box<i64>>\n'
     '  b.v = a\n  var c: box<box<box<i64>>>\n  c.v = b\n  return i32(c.v.v.v) }'),
    ('A4 十层嵌套（终止性）', 'ok',
     'struct box<T> { v: T }\nfn main() -> i32 { var a: box<i64>\n  a.v = i64(1)\n'
     '  var b: box<box<box<box<box<box<box<box<box<box<i64>>>>>>>>>>\n'
     '  b.v.v.v.v.v.v.v.v.v.v = a.v\n'      # 题目自己踩过坑：声明了 b 却不用 ⇒ -Werror 报 unused
     '  return i32(b.v.v.v.v.v.v.v.v.v.v) }'),
    ('A5 用户类型名与实例 C 名撞车', 'ok_or_reject',
     'struct pair<T> { a: T }\nstruct pair_i64 { a: i64 }\nfn main() -> i32 { var p: pair<i64>\n'
     '  p.a = i64(1)\n  var q: pair_i64\n  q.a = i64(2)\n  return i32(p.a + q.a) }'),
    ('A6 两个参数同型 pair<i64,i64>', 'ok',
     'struct pair<A, B> { a: A  b: B }\nfn main() -> i32 { var p: pair<i64, i64>\n  p.a = i64(3)\n'
     '  p.b = i64(4)\n  return i32(p.a + p.b) }'),
    ('A7 参数顺序不同 = 两个实例', 'ok',
     'struct pair<A, B> { a: A  b: B }\nfn main() -> i32 { var p: pair<i64, u8>\n  p.a = i64(1)\n'
     '  p.b = u8(2)\n  var q: pair<u8, i64>\n  q.a = u8(3)\n  q.b = i64(4)\n'
     '  return i32(p.a + q.b) + i32(p.b) + i32(q.a) }'),
    ('A8 实例做实例的实参', 'ok',
     'struct box<T> { v: T }\nstruct pair<A, B> { a: A  b: B }\nfn main() -> i32 {'
     ' var inner: pair<i64, u8>\n  inner.a = i64(5)\n  inner.b = u8(6)\n'
     '  var outer: box<pair<i64, u8>>\n  outer.v = inner\n  return i32(outer.v.a) }'),
    ('A9 类型参数与全局类型同名', 'ok_or_reject',
     'struct T { v: i64 }\nfn f<T>(x: T) -> T { return x }\nfn main() -> i32 { return i32(f(i64(1))) }'),

    # ---- B. 替换与签名 ----
    ('B1 T 出现两次（同型实参）', 'ok',
     'fn dup<T>(a: T, b: T) -> T { return a }\nfn main() -> i32 { return i32(dup(i64(1), i64(2))) }'),
    ('B2 T 出现两次（异型实参）', 'reject', 'dup(i64(1), u8(2))',
     'fn dup<T>(a: T, b: T) -> T { return a }\nfn main() -> i32 { return i32(dup(i64(1), u8(2))) }'),
    ('B3 T 只在返回类型（显式实参）', 'ok',
     'fn zero<T>() -> i64 { return i64(0) }\nfn main() -> i32 { return i32(zero<i64>()) }'),
    ('B4 T 在 slice<T> 里', 'ok',
     'fn first<T>(a: slice<T>) -> T { return a[0] }\nfn main() -> i32 { var arr: [2]i64 = [7, 8]\n'
     '  return i32(first(arr[..])) }'),
    ('B5 T 在 [2]T 里', 'ok_or_reject',
     'fn sum2<T>(a: [2]T) -> T { return a[0] }\nfn main() -> i32 { var arr: [2]i64 = [1, 2]\n'
     '  return i32(sum2(arr)) }'),
    ('B6 T 在 ?ref T 里', 'ok_or_reject',
     'struct node<T> { next: ?ref node<T>  v: T }\nfn main() -> i32 { var n: node<i64>\n  n.v = i64(3)\n'
     '  n.next = null\n  return i32(n.v) }'),
    ('B7 T 在 coroutine<T> 里', 'ok',
     'fn gen<T>(x: T) -> coroutine<T> { yield x }\nfn main() -> i32 { var c: coroutine<i64> = gen(i64(9))\n'
     '  var s: i64 = 0\n  while c.next() { s = s + c.value() }\n  return i32(s) }'),
    ('B8 T 在 alloc<T> 里', 'ok',
     'fn make<T>(x: T) -> mut ref T { var p: mut ref T = alloc<T>(1)\n  *p = x\n  return p }\n'
     'fn main() -> i32 { var p: mut ref i64 = make(i64(4))\n  return i32(*p) }'),
    ('B9 T 在 new T 里', 'ok_or_reject',
     'struct box<T> { v: T }\nfn mk<T>() -> mut ref box<T> { var p: mut ref box<T> = new box<T>\n'
     '  return p }\nfn main() -> i32 { var p: mut ref box<i64> = mk<i64>()\n  p.v = i64(5)\n  return i32(p.v) }'),
    ('B10 T 不出现（显式实参）', 'ok',
     'fn f<T>(x: i64) -> i64 { return x + i64(1) }\nfn main() -> i32 { return i32(f<i64>(i64(1))) }'),
    ('B11 泛型方法（impl<T> 上挂）', 'ok',
     'struct box<T> { v: T }\nimpl<T> box<T> { fn get(self: ref box<T>) -> T { return self.v } }\n'
     'fn main() -> i32 { var b: box<i64>\n  b.v = i64(6)\n  return i32(b.get()) }'),
    ('B12 显式实参元数写错', 'reject', '',
     'fn id<T>(x: T) -> T { return x }\nfn main() -> i32 { return i32(id<i64, u8>(i64(1))) }'),
    ('B13 实参是未知类型', 'reject', '',
     'fn id<T>(x: T) -> T { return x }\nfn main() -> i32 { return i32(id<Nope>(i64(1))) }'),
    ('B14 嵌套泛型里用外层参数', 'ok',
     'struct box<T> { v: T }\nfn wrap<T>(x: T) -> box<T> { var b: box<T>\n  b.v = x\n  return b }\n'
     'fn main() -> i32 { return i32(wrap(i64(8)).v) }'),
    ('B15 泛型函数调用泛型函数', 'ok',
     'fn inner<T>(x: T) -> T { return x }\nfn outer<T>(x: T) -> T { return inner(x) }\n'
     'fn main() -> i32 { return i32(outer(i64(2))) }'),

    # ---- C. 延迟检查（H7 家族）----
    ('B8 泛型协程在**泛型函数体内**被驱动（延迟路径）', 'ok',
     'fn gen<T>(x: T) -> coroutine<T> { yield x }\n'
     'fn run<T>(v: T) -> i64 { var c = gen(v)\n  var s: i64 = 0\n'
     '  while c.next() { s = s + c.value() }\n  return s }\n'
     'fn main() -> i32 { return i32(run(i64(4))) }'),

    ('C1 T == T（i64）', 'ok',
     'fn eq<T>(a: T, b: T) -> bool { return a == b }\nfn main() -> i32 { if eq(i64(1), i64(1)) { return 0 }\n'
     '  return 1 }'),
    ('C2 T + T（i64）', 'ok',
     'fn add<T>(a: T, b: T) -> T { return a + b }\nfn main() -> i32 { return i32(add(i64(1), i64(2))) }'),
    ('C3 T < T（i64）', 'ok',
     'fn lt<T>(a: T, b: T) -> bool { return a < b }\nfn main() -> i32 { if lt(i64(1), i64(2)) { return 0 }\n'
     '  return 1 }'),
    ('C4 T 实例化为数组后与标量比较', 'reject', '',
     'fn eqx<T>(a: T, b: i64) -> bool { return a == b }\nfn main() -> i32 { var arr: [1]i64 = [1]\n'
     '  if eqx(arr, i64(1)) { return 0 }\n  return 1 }'),
    ('C5 T 实例化为数组后与同型数组比较', 'ok_or_reject',
     'fn eqa<T>(a: T, b: T) -> bool { return a == b }\nfn main() -> i32 { var x: [1]i64 = [1]\n'
     '  var y: [1]i64 = [1]\n  if eqa(x, y) { return 0 }\n  return 1 }'),
    ('C6 T 实例化为 bool 后比大小', 'reject', '',
     'fn lt<T>(a: T, b: T) -> bool { return a < b }\nfn main() -> i32 { if lt(true, false) { return 0 }\n'
     '  return 1 }'),
    ('C7 T 实例化为无 == 的结构体', 'reject', '',
     'struct s { v: i64 }\nfn eq<T>(a: T, b: T) -> bool { return a == b }\nfn main() -> i32 {'
     ' var x: s\n  x.v = i64(1)\n  if eq(x, x) { return 0 }\n  return 1 }'),
    ('C8 T 实例化为有 == 的结构体', 'ok_or_reject',
     'struct s { v: i64 }\nimpl s { fn ==(self: ref s, o: ref s) -> bool { return self.v == o.v } }\n'
     'fn eq<T>(a: T, b: T) -> bool { return a == b }\nfn main() -> i32 { var x: s\n  x.v = i64(1)\n'
     '  if eq(x, x) { return 0 }\n  return 1 }'),

    # ---- D. 每实例方法集隔离 ----
    ('D1 泛型 impl 的方法只在实例上', 'ok',
     'struct pair<T> { a: T }\nimpl<T> pair<T> { fn get(self: ref pair<T>) -> T { return self.a } }\n'
     'fn main() -> i32 { var p: pair<i64>\n  p.a = i64(4)\n  return i32(p.get()) }'),
    ('D2 具体实例专属方法不许外泄', 'reject', '',
     'struct pair<T> { a: T }\nimpl pair<i64> { fn only(self: ref pair<i64>) -> i64 { return self.a } }\n'
     'fn main() -> i32 { var p: pair<u8>\n  p.a = u8(1)\n  return i32(p.only()) }'),
    ('D3 具体实例专属方法在它自己上可用', 'ok',
     'struct pair<T> { a: T }\nimpl pair<i64> { fn only(self: ref pair<i64>) -> i64 { return self.a } }\n'
     'fn main() -> i32 { var p: pair<i64>\n  p.a = i64(5)\n  return i32(p.only()) }'),
    ('D4 泛型 trait impl', 'ok',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct pair<T> { a: T }\n'
     'impl<T> Tag for pair<T> { fn tag(self: ref pair<T>) -> i64 { return i64(1) } }\n'
     'fn main() -> i32 { var p: pair<i64>\n  p.a = i64(0)\n  return i32(p.tag()) }'),
    ('D5 同一 trait 的泛型 impl 与具体 impl 冲突', 'reject', '',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct pair<T> { a: T }\n'
     'impl<T> Tag for pair<T> { fn tag(self: ref pair<T>) -> i64 { return i64(1) } }\n'
     'impl Tag for pair<i64> { fn tag(self: ref pair<i64>) -> i64 { return i64(2) } }\n'
     'fn main() -> i32 { var p: pair<i64>\n  p.a = i64(0)\n  return i32(p.tag()) }'),
    ('D6 同一个泛型 impl 写两遍', 'reject', '',
     'struct pair<T> { a: T }\nimpl<T> pair<T> { fn get(self: ref pair<T>) -> T { return self.a } }\n'
     'impl<T> pair<T> { fn get(self: ref pair<T>) -> T { return self.a } }\n'
     'fn main() -> i32 { return 0 }'),
    ('D7 内建泛型上的泛型 impl', 'ok_or_reject',
     'impl<T> slice<T> { fn first(self: ref slice<T>) -> T { return self[0] } }\n'
     'fn main() -> i32 { var a: [2]i64 = [3, 4]\n  return i32(a[..].first()) }'),
    ('D8 内建实例 impl 与泛型 impl 并存', 'ok_or_reject',
     'impl slice<u8> { fn half(self: ref slice<u8>) -> i64 { return i64(self.len) / i64(2) } }\n'
     'impl<T> slice<T> { fn first(self: ref slice<T>) -> T { return self[0] } }\n'
     'fn main() -> i32 { var s: slice<u8> = "abcd"\n  var a: [1]i64 = [9]\n'
     '  return i32(s.half()) + i32(a[..].first()) }'),

    # ---- E. 递归与终止 ----
    ('E1 递归数据结构', 'ok',
     'struct node<T> { v: T  next: ?ref node<T> }\nfn main() -> i32 { var a: node<i64>\n  a.v = i64(1)\n'
     '  a.next = null\n  return i32(a.v) }'),
    ('E2 互相递归的泛型函数（必须终止）', 'ok_or_reject',
     'fn a<T>(x: T) -> i64 { return b(x) }\nfn b<T>(x: T) -> i64 { return a(x) }\n'
     'fn main() -> i32 { return i32(a(i64(1))) }'),
    ('E3 自递归实例化（必须终止，不许挂死）', 'ok_or_reject',
     'struct box<T> { v: T }\nfn grow<T>(x: T) -> i64 { var b: box<T>\n  b.v = x\n  return grow(b) }\n'
     'fn main() -> i32 { return i32(grow(i64(1))) }'),

    # ---- F. 与本族能力交叉 ----
    # 同族新缺口（第 8→9 轮修 F1 时发现，单列追踪）：**泛型实例的方法体在忽略 `self` 时**
    # 没走 `markUnusedParams` 那趟（它只按 g.funcs 找候选，实例方法不在里面）⇒ 生成物在
    # -Werror=unused-parameter 下编译失败。
    ('F1b 泛型实例的方法忽略 self', 'ok',
     'trait Tag2 { fn tag(self: ref Self) -> i64 }\n'
     'struct p2<T> { a: T }\n'
     'impl<T> Tag2 for p2<T> { fn tag(self: ref p2<T>) -> i64 { return i64(7) } }\n'
     'fn main() -> i32 { var p: p2<i64>\n  p.a = i64(0)\n'
     '  var d: dyn Tag2 = dyn Tag2(p)\n  return i32(d.tag()) - 7 }'),

    ('F1 dyn 打在泛型实例上', 'ok',
     'trait Tag { fn tag(self: ref Self) -> i64 }\n'
     'struct pair<T> { a: T }\n'
     'impl<T> Tag for pair<T> { fn tag(self: ref pair<T>) -> i64 { return self.a } }\n'
     'fn main() -> i32 { var p: pair<i64>\n  p.a = i64(7)\n'
     '  var d: dyn Tag = dyn Tag(p)\n  return i32(d.tag()) - 7 }'),
    ('F2 trait Codec<T> + 泛型 receiver', 'ok',
     'trait Codec<T> { fn enc(self: ref Self, v: T) -> i64 }\nstruct pair<U> { a: U }\n'
     'impl Codec<i64> for pair<u8> { fn enc(self: ref pair<u8>, v: i64) -> i64 { return v + i64(self.a) } }\n'
     'fn main() -> i32 { var p: pair<u8>\n  p.a = u8(1)\n  return i32(p.enc(i64(2))) }'),
    ('F3 泛型类型上的泛型方法（P5，已知缺口）', 'reject', '',
     'struct pair<T> { a: T }\nimpl<T> pair<T> { fn zip<U>(self: ref pair<T>, u: U) -> U { return u } }\n'
     'fn main() -> i32 { var p: pair<i64>\n  p.a = i64(1)\n  return i32(p.zip(i64(2))) }'),
    ('F4 泛型函数里开协程并驱动', 'ok',
     'fn run<T>(v: T) -> i64 { var c: coroutine<T> = gen(v)\n  var n: i64 = 0\n'
     '  while c.next() { n = n + i64(1) }\n  return n }\n'
     'fn gen<T>(x: T) -> coroutine<T> { yield x }\n'
     'fn main() -> i32 { return i32(run(i64(1))) }'),
]

# ---------------------------------------------------------------- 协程 × 池 / arena
# 攻击面：跨 suspend 的值与视图、帧的归属（home zone）、任务表的生命周期、两个帧交错、
# 耗尽后继续驱动、以及"帧里存的引用/切片到底能不能跨 yield 活着"。
CORO = [
    ('K1 基本驱动（while + next/value）', 'ok',
     'use std::io\nfn counter(n: i64) -> coroutine<i64> {\n  var i: i64 = 0\n'
     '  while i < n { yield i\n    i = i + 1 } }\n'
     'fn main() -> i32 { var s: i64 = 0\n  var c = counter(i64(4))\n'
     '  while c.next() { s = s + c.value() }\n  io::cout << s << "\\n"\n  return i32(s) }'),
    ('K2 for 驱动与显式驱动结果一致', 'ok',
     'use std::io\nfn counter(n: i64) -> coroutine<i64> {\n  var i: i64 = 0\n'
     '  while i < n { yield i\n    i = i + 1 } }\n'
     'fn main() -> i32 { var a: i64 = 0\n  var b: i64 = 0\n'
     '  var c1 = counter(i64(5))\n  while c1.next() { a = a + c1.value() }\n'
     '  var c2 = counter(i64(5))\n  for x in c2 { b = b + x }\n'
     '  io::cout << a << " " << b << "\\n"\n  return i32(b - a) }'),
    ('K3 耗尽后继续调 next 必须为 false（不许 trap）', 'ok',
     'fn one() -> coroutine<i64> { yield i64(7) }\n'
     'fn main() -> i32 { var c = one()\n  var n: i64 = 0\n'
     '  while c.next() { n = n + i64(1) }\n'
     '  if c.next() { return 1 }\n  if c.next() { return 2 }\n  return i32(n) }'),
    ('K4 两个帧交错驱动互不串扰', 'ok',
     'use std::io\nfn ids(base: i64) -> coroutine<i64> {\n  var i: i64 = 0\n'
     '  while i < i64(3) { yield base + i\n    i = i + i64(1) } }\n'
     'fn main() -> i32 { var a = ids(i64(10))\n  var b = ids(i64(100))\n'
     '  var s: i64 = 0\n  while a.next() { s = s + a.value()\n'
     '    if b.next() { s = s + b.value() } }\n  io::cout << s << "\\n" }'),
    ('K5 局部数组跨 yield（必须进帧）', 'ok',
     'fn gen() -> coroutine<i64> { var buf: [3]i64 = [7, 8, 9]\n'
     '  var i: i64 = 0\n  while i < i64(3) { yield buf[i]\n    i = i + i64(1) } }\n'
     'fn main() -> i32 { var s: i64 = 0\n  var c = gen()\n'
     '  while c.next() { s = s + c.value() }\n  return i32(s) }'),
    ('K6 suspend 前后各写一次同一局部', 'ok',
     'fn gen() -> coroutine<i64> { var x: i64 = i64(1)\n  yield x\n  x = x + i64(41)\n'
     '  yield x }\nfn main() -> i32 { var c = gen()\n  var last: i64 = 0\n'
     '  while c.next() { last = c.value() }\n  return i32(last) }'),
    ('K7 协程体内分配（引用不能跨 yield ⇒ 设计上拒绝）', 'reject',
     'fn gen() -> coroutine<i64> { var i: i64 = 0\n'
     '  while i < i64(3) { var p: mut ref i64 = alloc<i64>(1)\n    *p = i * i64(2)\n'
     '    yield *p\n    i = i + i64(1) } }\n'
     'fn main() -> i32 { var s: i64 = 0\n  var c = gen()\n'
     '  while c.next() { s = s + c.value() }\n  return i32(s) }'),
    ('K8 视图在 yield 之后仍可用（帧内数据）', 'ok_or_reject',
     'fn gen() -> coroutine<i64> { var buf: [4]i64 = [1, 2, 3, 4]\n'
     '  var v: slice<i64> = buf[..]\n  yield i64(1)\n  return_v(v) }\n'
     'fn return_v(v: slice<i64>) -> i64 { return v[2] }\n'
     'fn main() -> i32 { var c = gen()\n  var r: i64 = 0\n'
     '  while c.next() { r = c.value() }\n  return i32(r) }'),
    ('K9 跨 yield 取局部地址（应被拒或安全）', 'ok_or_reject',
     'fn gen() -> coroutine<i64> { var x: i64 = i64(5)\n  var p: ref i64 = ref x\n'
     '  yield i64(1)\n  return *p }\n'
     'fn main() -> i32 { var c = gen()\n  var r: i64 = 0\n'
     '  while c.next() { r = c.value() }\n  return i32(r) }'),
    ('K10 send：把值交给下一次 resume', 'ok_or_reject',
     'fn echo() -> coroutine<i64> { var got: i64 = yield i64(1)\n  yield got\n  yield got }' '\n'
     'fn main() -> i32 { var c = echo()\n  if !c.next() { return 1 }\n'
     '  var v: i64 = c.value()\n  if !c.send(i64(77)) { return 2 }\n'
     '  return i32(c.value() - i64(77)) + i32(v) - 1 }'),
    ('K11 协程驱动另一个协程', 'ok',
     'fn inner() -> coroutine<i64> { yield i64(2)\n  yield i64(3) }\n'
     'fn outer() -> coroutine<i64> { var c = inner()\n'
     '  while c.next() { yield c.value() * i64(10) } }\n'
     'fn main() -> i32 { var s: i64 = 0\n  var o = outer()\n'
     '  while o.next() { s = s + o.value() }\n  return i32(s) }'),
    ('K12 帧在循环里反复创建（不串扰）', 'ok',
     'fn ids(base: i64) -> coroutine<i64> { yield base\n  yield base + i64(1) }\n'
     'fn main() -> i32 { var s: i64 = 0\n  var k: i64 = 0\n'
     '  while k < i64(4) { var c = ids(k)\n    while c.next() { s = s + c.value() }\n'
     '    k = k + i64(1) }\n  return i32(s) }'),
    ('K13 协程与池对象共存', 'ok_or_reject',
     'fn gen() -> coroutine<i64> { var i: i64 = 0\n'
     '  while i < i64(2) { yield i\n    i = i + i64(1) } }\n'
     'fn main() -> i32 { let rid = syspool::extc_pool_new(i64(64))\n'
     '  var c = gen()\n  var s: i64 = 0\n'
     '  while c.next() { s = s + c.value() }\n'
     '  syspool::extc_pool_give(rid)\n  return i32(s) }'),
    ('K14 协程内建池切片并 yield 其长度', 'ok_or_reject',
     'fn gen() -> coroutine<i64> { let rid = syspool::extc_pool_new(i64(64))\n'
     '  let s: mut slice<u8> = syspool::extc_pool_slice(rid)\n'
     '  yield i64(s.len)\n  syspool::extc_pool_give(rid) }\n'
     'fn main() -> i32 { var c = gen()\n  var n: i64 = 0\n'
     '  while c.next() { n = n + c.value() }\n  return i32(n) }'),
    ('K15 协程帧句柄存进数组再驱动', 'ok_or_reject',
     'fn ids(base: i64) -> coroutine<i64> { yield base }\n'
     'fn main() -> i32 { var a = ids(i64(3))\n  var b = ids(i64(4))\n'
     '  var s: i64 = 0\n  while a.next() { s = s + a.value() }\n'
     '  while b.next() { s = s + b.value() }\n  return i32(s) }'),
    ('K16 协程存进局部切片后驱动（视图 vs 帧）', 'ok_or_reject',
     'fn ids(base: i64) -> coroutine<i64> { yield base }\n'
     'fn main() -> i32 { var a = ids(i64(9))\n  var arr: [1]i64 = [0]\n'
     '  arr[0] = i64(1)\n  var s: i64 = 0\n  while a.next() { s = s + a.value() + arr[0] }\n'
     '  return i32(s) }'),
]

# ---------------------------------------------------------------- 模块系统
# 一个文件 = 一个模块；跨模块引用写限定名；`@private` 藏起来。攻击面：名字可见性、
# mangling 撞车（模块前缀那一族）、跨模块的实例共享、循环 import、模块名与类型名互撞。
MODULES = [
    ('M1 基本跨模块调用与常量', 'ok', {
        'greet.extc': 'let LIMIT: i32 = 42\nfn shout(n: i32) -> i32 { return n + n }',
        'main.extc': 'use greet\nfn main() -> i32 { return greet::shout(i32(21)) - i32(42) }'}),
    ('M2 @private 不可跨模块', 'reject', {
        'greet.extc': '@private fn twice(n: i32) -> i32 { return n + n }\nfn shout(n: i32) -> i32 { return twice(n) }',
        'main.extc': 'use greet\nfn main() -> i32 { return greet::twice(i32(1)) }'}),
    ('M3 两个模块的同名私有函数互不干扰', 'ok', {
        'a.extc': '@private fn helper(n: i32) -> i32 { return n + i32(1) }\nfn fa(n: i32) -> i32 { return helper(n) }',
        'b.extc': '@private fn helper(n: i32) -> i32 { return n + i32(2) }\nfn fb(n: i32) -> i32 { return helper(n) }',
        'main.extc': 'use a\nuse b\nfn main() -> i32 { return a::fa(i32(0)) + b::fb(i32(0)) }'}),
    ('M4 两个模块的同名公开函数各归各的', 'ok', {
        'a.extc': 'fn f() -> i32 { return i32(1) }',
        'b.extc': 'fn f() -> i32 { return i32(2) }',
        'main.extc': 'use a\nuse b\nfn main() -> i32 { return a::f() * i32(10) + b::f() }'}),
    ('M5 模块名与类型名撞车（mangling 家族）', 'ok_or_reject', {
        'pair.extc': 'struct pair<A, B> { a: A  b: B }\nfn make() -> i32 { return i32(1) }',
        'main.extc': 'use pair\nstruct pair_i64 { v: i64 }\n'
                     'fn main() -> i32 { var p: pair::pair<i64, u8>\n  p.a = i64(1)\n  p.b = u8(2)\n'
                     '  var q: pair_i64\n  q.v = i64(3)\n  return pair::make() + i32(p.a) + i32(q.v) }'}),
    ('M6 跨模块共享泛型实例（一份还是两份都对）', 'ok', {
        'gen.extc': 'fn id<T>(x: T) -> T { return x }\nfn useIt() -> i64 { return id(i64(5)) }',
        'main.extc': 'use gen\nfn main() -> i32 { return i32(gen::useIt() + gen::id(i64(2))) - 7 }'}),
    ('M7 跨模块的结构体与方法', 'ok', {
        'point.extc': 'struct point { x: i64  y: i64 }\n'
                      'impl point { fn sum(self: ref point) -> i64 { return self.x + self.y } }',
        'main.extc': 'use point\nfn main() -> i32 { var p: point::point\n  p.x = i64(3)\n  p.y = i64(4)\n'
                     '  return i32(p.sum()) - 7 }'}),
    ('M8 循环 import（不许挂死/不许重复定义）', 'ok_or_reject', {
        'a.extc': 'use b\nfn fa() -> i32 { return i32(1) }\nfn callB() -> i32 { return b::fb() }',
        'b.extc': 'use a\nfn fb() -> i32 { return i32(2) }\nfn callA() -> i32 { return a::fa() }',
        'main.extc': 'use a\nfn main() -> i32 { return a::callB() + i32(1) - 3 }'}),
    ('M9 use 一个不存在的模块', 'reject', {
        'main.extc': 'use nosuchmodule\nfn main() -> i32 { return 0 }'}),
    ('N1 跨模块 trait impl + dyn', 'ok', {
        'tag.extc': 'trait Tag { fn tag(self: ref Self) -> i64 }\nfn dynTag(t: dyn Tag) -> i64 { return t.tag() }',
        'thing.extc': 'use tag\nstruct thing { v: i64 }\n'
                     'impl Tag for thing { fn tag(self: ref thing) -> i64 { return self.v } }',
        'main.extc': 'use tag\nuse thing\nfn main() -> i32 { var t: thing::thing\n  t.v = i64(6)\n'
                     '  return i32(tag::dynTag(dyn Tag(t))) - 6 }'}),   # `dyn` 收无限定的 trait 名：`use tag` 已把 Tag 带进作用域；`dyn tag::Tag(...)` 不是合法语法（题目自己踩过）
    ('N2 @private 类型从公开函数签名漏出', 'ok_or_reject', {
        'secret.extc': '@private struct secret { v: i64 }\n'
                      'fn make() -> secret { var s: secret\n  s.v = i64(9)\n  return s }\n'
                      'fn get(s: ref secret) -> i64 { return s.v }',
        'main.extc': 'use secret\nfn main() -> i32 { let s = secret::make()\n'
                     '  return i32(secret::get(ref s)) - 9 }'}),
    ('N3 模块文件与同名子目录并存', 'ok_or_reject', {
        'foo.extc': 'fn f() -> i32 { return i32(1) }',
        'main.extc': 'use foo\nfn main() -> i32 { return foo::f() - 1 }'}),
    ('N4 跨模块类型闭环', 'ok_or_reject', {
        'a.extc': 'use b\nstruct A { other: ?ref b::B  v: i64 }\nfn mkA() -> A { var x: A\n  x.v = i64(1)\n  x.other = null\n  return x }',
        'b.extc': 'use a\nstruct B { other: ?ref a::A  v: i64 }',
        'main.extc': 'use a\nfn main() -> i32 { let x = a::mkA()\n  return i32(x.v) - 1 }'}),
    ('N5 模块名撞 std 模块名（io.extc）', 'ok_or_reject', {
        'io.extc': 'fn shout() -> i32 { return i32(5) }',
        'main.extc': 'use io\nfn main() -> i32 { return io::shout() - 5 }'}),
    ('N6 同一泛型在两个模块各实例化一次', 'ok', {
        'gen.extc': 'fn id<T>(x: T) -> T { return x }\nfn a() -> i64 { return id(i64(3)) }',
        'main.extc': 'use gen\nfn main() -> i32 { return i32(gen::a() + gen::id(i64(4))) - 7 }'}),
    ('N7 @private 泛型经公开包装跨模块用', 'ok_or_reject', {
        'box.extc': '@private fn wrap<T>(x: T) -> T { return x }\n'
                    'fn pub1(x: i64) -> i64 { return wrap(x) }\n'
                    'fn pub2(x: u8) -> u8 { return wrap(x) }',
        'main.extc': 'use box\nfn main() -> i32 { return i32(box::pub1(i64(2))) + i32(box::pub2(u8(3))) - 5 }'}),
    ('N8 两个模块同名结构体各实例化', 'ok_or_reject', {
        'p.extc': 'struct item<T> { v: T }\nfn mk() -> item<i64> { var x: item<i64>\n  x.v = i64(1)\n  return x }',
        'q.extc': 'struct item<T> { v: T }\nfn mk() -> item<u8> { var x: item<u8>\n  x.v = u8(2)\n  return x }',
        'main.extc': 'use p\nuse q\nfn main() -> i32 { return i32(p::mk().v) + i32(q::mk().v) - 3 }'}),
    ('N9 模块函数遮蔽内置/标准名', 'ok_or_reject', {
        'sh.extc': 'fn printlnInt(n: i64) -> i64 { return n }',
        'main.extc': 'use sh\nuse std::io\nfn main() -> i32 { io::cout << sh::printlnInt(i64(1)) << "\\n" }'}),
    ('N10 @private 常量经公开常量漏出', 'ok_or_reject', {
        'c.extc': '@private let HIDDEN: i64 = 41\nlet SHOWN: i64 = HIDDEN + 1',
        'main.extc': 'use c\nfn main() -> i32 { return i32(c::SHOWN) - 42 }'}),
    ('N11 三模块链（a → b → c）', 'ok', {
        'c.extc': 'fn base() -> i64 { return i64(7) }',
        'b.extc': 'use c\nfn mid() -> i64 { return c::base() + i64(1) }',
        'a.extc': 'use b\nfn top() -> i64 { return b::mid() + i64(1) }',
        'main.extc': 'use a\nfn main() -> i32 { return i32(a::top()) - 9 }'}),
    ('N12 跨模块 impl 一个别处的 trait', 'ok_or_reject', {
        'tag.extc': 'trait Tag { fn tag(self: ref Self) -> i64 }',
        'wrap.extc': 'struct w { v: i64 }\nimpl Tag for w { fn tag(self: ref w) -> i64 { return self.v } }',
        'main.extc': 'use tag\nuse wrap\nfn main() -> i32 { var x: wrap::w\n  x.v = i64(4)\n'
                     '  return i32(x.tag()) - 4 }'}),
    ('N13 模块名形似关键字', 'ok_or_reject', {
        'self_mod.extc': 'fn f() -> i32 { return i32(2) }',
        'main.extc': 'use self_mod\nfn main() -> i32 { return self_mod::f() - 2 }'}),
    ('M10 模块内 main 不夺走入口', 'ok_or_reject', {
        'other.extc': 'fn main() -> i32 { return i32(99) }\nfn helper() -> i32 { return i32(1) }',
        'main.extc': 'use other\nfn main() -> i32 { return other::helper() - 1 }'}),
]

# ---------------------------------------------------------------- 视图与切片
# 攻击面：mut/只读两视图的同一 C 结构（第 6 轮统一过 unitFind 的那条）、空切片与边界、
# 越界必须**带位置 trap**、切片元素是结构体/泛型实例、从字面量取的可写视图、别名与嵌套、
# 跨函数/跨协程/跨池的视图生命周期。
VIEWS = [
    ('V1 基本切片读写与长度', 'ok',
     'fn main() -> i32 { var a: [4]i64 = [1, 2, 3, 4]\n  var s: slice<i64> = a[..]\n'
     '  return i32(s[0] + s[3] + i64(s.len)) }'),
    ('V2 mut 与只读视图是同一个 C 结构', 'ok',
     'fn sum(s: slice<i64>) -> i64 { var t: i64 = 0\n  var i: i64 = 0\n'
     '  while i < i64(s.len) { t = t + s[i]\n    i = i + i64(1) }\n  return t }\n'
     'fn bump(s: mut slice<i64>) { s[0] = s[0] + i64(10) }\n'
     'fn main() -> i32 { var a: [3]i64 = [1, 2, 3]\n  bump(a[..])\n'
     '  return i32(sum(a[..])) - 16 }'),
    ('V3 空切片：长度 0、不许 trap、不许越界读', 'ok',
     'fn main() -> i32 { var a: [3]i64 = [1, 2, 3]\n  var e: slice<i64> = a[0..0]\n'
     '  var n: i64 = 0\n  for x in e { n = n + x }\n  return i32(n) + i32(e.len) }'),
    ('V4 越界索引必须带位置 trap', 'trap',
     'fn main() -> i32 { var a: [3]i64 = [1, 2, 3]\n  var s: slice<i64> = a[..]\n'
     '  return i32(s[3]) }'),
    ('V5 切片元素是结构体（字段可写）', 'ok',
     'struct pt { x: i64  y: i64 }\nfn main() -> i32 { var a: [2]pt\n'
     '  a[0].x = i64(1)\n  a[0].y = i64(2)\n  a[1].x = i64(3)\n  a[1].y = i64(4)\n'
     '  var s: mut slice<pt> = a[..]\n  s[1].x = s[1].x + i64(10)\n'
     '  return i32(a[0].x + a[0].y + a[1].x + a[1].y) }'),
    ('V6 切片元素是泛型实例', 'ok_or_reject',
     'struct box<T> { v: T }\nfn main() -> i32 { var a: [2]box<i64>\n'
     '  a[0].v = i64(5)\n  a[1].v = i64(6)\n  var s: slice<box<i64>> = a[..]\n'
     '  return i32(s[0].v + s[1].v) - 11 }'),
    ('V7 切片从函数返回（逃逸规则）', 'ok_or_reject',
     'var g: [3]i64 = [7, 8, 9]\nfn get() -> slice<i64> { return g[..] }\n'
     'fn main() -> i32 { var s: slice<i64> = get()\n  return i32(s[1]) - 8 }'),
    ('V8 new T[n] 得到可写切片', 'ok',
     'fn main() -> i32 { var s: mut slice<i64> = new i64[4]\n  var i: i64 = 0\n'
     '  while i < i64(s.len) { s[i] = i * i\n    i = i + i64(1) }\n'
     '  return i32(s[3]) - 9 }'),
    ('V9 把 [4]i64 当 slice<u8> 重解释', 'reject',
     'fn main() -> i32 { var a: [4]i64 = [1, 2, 3, 4]\n  var b: mut slice<u8> = a[..]\n'
     '  return i32(b[0]) }'),
    ('V10 同一数组两个可写视图（别名）', 'ok_or_reject',
     'fn main() -> i32 { var a: [2]i64 = [1, 2]\n  var x: mut slice<i64> = a[..]\n'
     '  var y: mut slice<i64> = a[..]\n  x[0] = i64(5)\n  return i32(y[0]) - 5 }'),
    ('V11 切片的切片（嵌套视图）', 'ok_or_reject',
     'fn main() -> i32 { var a: [6]i64 = [1, 2, 3, 4, 5, 6]\n  var s: slice<i64> = a[..]\n'
     '  var t: slice<i64> = s[2..5]\n  return i32(t[0] + t[2]) - 8 }'),
    ('V12 从字符串字面量取可写视图（应被拒）', 'reject',
     'fn main() -> i32 { var s: mut slice<u8> = "abc"\n  s[0] = u8(65)\n  return i32(s[0]) }'),
    ('V13 结构体里存视图（第 6 轮 unitFind 那条）', 'ok_or_reject',
     'struct reader { chunk: mut slice<u8> }\n'
     'fn main() -> i32 { var a: [4]u8 = [1, 2, 3, 4]\n  var r: reader\n  r.chunk = a[..]\n'
     '  r.chunk[1] = u8(9)\n  return i32(a[1]) - 9 }'),
    ('V14 视图传进协程（跨 suspend）', 'ok_or_reject',
     'fn gen(s: slice<i64>) -> coroutine<i64> { var i: i64 = 0\n'
     '  while i < i64(s.len) { yield s[i]\n    i = i + i64(1) } }\n'
     'fn main() -> i32 { var a: [3]i64 = [2, 3, 4]\n  var c = gen(a[..])\n  var t: i64 = 0\n'
     '  while c.next() { t = t + c.value() }\n  return i32(t) - 9 }'),
    ('V15 池切片的读写', 'ok_or_reject',
     'fn main() -> i32 { let rid = syspool::extc_pool_new(i64(64))\n'
     '  let s: mut slice<u8> = syspool::extc_pool_slice(rid)\n  s[0] = u8(7)\n'
     '  var v: i64 = i64(s[0])\n  syspool::extc_pool_give(rid)\n  return i32(v) - 7 }'),
    ('V16 for 遍历切片与下标循环一致', 'ok',
     'fn main() -> i32 { var a: [5]i64 = [1, 2, 3, 4, 5]\n  var s: slice<i64> = a[..]\n'
     '  var x: i64 = 0\n  for v in s { x = x + v }\n  var y: i64 = 0\n  var i: i64 = 0\n'
     '  while i < i64(s.len) { y = y + s[i]\n    i = i + i64(1) }\n  return i32(x - y) }'),
]

# ---------------------------------------------------------------- dyn 表
# 攻击面：表的构造与派发、对象安全性边界（无接收者 / 泛型 / 返回 Self）、表里的 NULL 槽、
# dyn 值存容器/结构体/协程、生命周期（载荷先死）、跨模块的 impl、内建类型上的 impl。
DYN = [
    ('D1 基本构造与派发', 'ok',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct s { v: i64 }\n'
     'impl Tag for s { fn tag(self: ref s) -> i64 { return self.v } }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(7)\n  var d: dyn Tag = dyn Tag(a)\n'
     '  return i32(d.tag()) - 7 }'),
    ('D2 无接收者的方法不能进 dyn（对象安全）', 'reject',
     'trait Bad { fn nope() -> i64 }\nstruct s { v: i64 }\n'
     'impl Bad for s { fn nope() -> i64 { return i64(1) } }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(0)\n  var d: dyn Bad = dyn Bad(a)\n'
     '  return i32(d.nope()) }'),
    ('D3 返回 Self 的方法不能进 dyn', 'reject',
     'trait Clone2 { fn dup(self: ref Self) -> Self }\nstruct s { v: i64 }\n'
     'impl Clone2 for s { fn dup(self: ref s) -> s { var r: s\n  r.v = self.v\n  return r } }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(1)\n  var d: dyn Clone2 = dyn Clone2(a)\n'
     '  return i32(d.dup().v) }'),
    ('D4 dyn 值存进变量并在函数间传递', 'ok',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct s { v: i64 }\n'
     'impl Tag for s { fn tag(self: ref s) -> i64 { return self.v } }\n'
     'fn get(d: dyn Tag) -> i64 { return d.tag() }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(3)\n  var d: dyn Tag = dyn Tag(a)\n'
     '  var e: dyn Tag = d\n  return i32(get(e)) - 3 }'),
    ('D5 两种载荷派发到各自的实现', 'ok',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct s { v: i64 }\nstruct t { w: i64 }\n'
     'impl Tag for s { fn tag(self: ref s) -> i64 { return self.v } }\n'
     'impl Tag for t { fn tag(self: ref t) -> i64 { return self.w * i64(2) } }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(1)\n  var b: t\n  b.w = i64(2)\n'
     '  var d1: dyn Tag = dyn Tag(a)\n  var d2: dyn Tag = dyn Tag(b)\n'
     '  return i32(d1.tag() + d2.tag()) - 5 }'),
    ('D6 内建类型上的 impl 进 dyn', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\n'
     'impl Tag for i64 { fn tag(self: ref i64) -> i64 { return *self + i64(1) } }\n'
     'fn main() -> i32 { var v: i64 = 5\n  var d: dyn Tag = dyn Tag(v)\n'
     '  return i32(d.tag()) - 6 }'),
    ('D7 带参数的方法经 dyn 派发', 'ok',
     'trait Add2 { fn plus(self: ref Self, n: i64) -> i64 }\nstruct s { v: i64 }\n'
     'impl Add2 for s { fn plus(self: ref s, n: i64) -> i64 { return self.v + n } }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(10)\n  var d: dyn Add2 = dyn Add2(a)\n'
     '  return i32(d.plus(i64(5))) - 15 }'),
    ('D8 实现里改自己的字段（派发到真身）', 'ok',
     'trait Bump { fn bump(self: mut ref Self) }\nstruct s { v: i64 }\n'
     'impl Bump for s { fn bump(self: mut ref s) { self.v = self.v + i64(1) } }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(1)\n  var d: dyn Bump = dyn Bump(a)\n'
     '  d.bump()\n  d.bump()\n  return i32(a.v) - 3 }'),
    ('D9 泛型 impl 的实例进 dyn（F1 那条）', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct pair<T> { a: T }\n'
     'impl<T> Tag for pair<T> { fn tag(self: ref pair<T>) -> i64 { return i64(7) } }\n'
     'fn main() -> i32 { var p: pair<i64>\n  p.a = i64(0)\n  var d: dyn Tag = dyn Tag(p)\n'
     '  return i32(d.tag()) - 7 }'),
    ('D10 dyn 值从函数返回（句柄活着）', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nvar g: i64 = 4\n'
     'struct s { v: i64 }\nimpl Tag for s { fn tag(self: ref s) -> i64 { return self.v } }\n'
     'fn make() -> dyn Tag { var a: s\n  a.v = i64(4)\n  return dyn Tag(a) }\n'
     'fn main() -> i32 { var d: dyn Tag = make()\n  return i32(d.tag()) - 4 }'),
    ('D11 dyn 值放进数组（两种载荷）', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct s { v: i64 }\nstruct t { w: i64 }\n'
     'impl Tag for s { fn tag(self: ref s) -> i64 { return self.v } }\n'
     'impl Tag for t { fn tag(self: ref t) -> i64 { return self.w } }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(1)\n  var b: t\n  b.w = i64(2)\n'
     '  var arr: [2]dyn Tag = [dyn Tag(a), dyn Tag(b)]\n'
     '  return i32(arr[0].tag() + arr[1].tag()) - 3 }'),
    ('D12 载荷先死：dyn **复制**载荷 ⇒ 安全（题目原先期望 reject 是错的）', 'ok',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct s { v: i64 }\n'
     'impl Tag for s { fn tag(self: ref s) -> i64 { return self.v } }\n'
     'fn main() -> i32 { var d: dyn Tag = makeBad()\n  var x: i32 = 0\n'
     '  { var a: s\n    a.v = i64(1)\n    d = dyn Tag(a) }\n  return i32(d.tag()) + x }\n'
     'fn makeBad() -> dyn Tag { var z: s\n  z.v = i64(0)\n  return dyn Tag(z) }'),
    ('D13 dyn 值存进协程并跨 suspend 用', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct s { v: i64 }\n'
     'impl Tag for s { fn tag(self: ref s) -> i64 { return self.v } }\n'
     'fn gen(a: s) -> coroutine<i64> { var d: dyn Tag = dyn Tag(a)\n  yield d.tag()\n'
     '  yield d.tag() }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(6)\n  var c = gen(a)\n  var t: i64 = 0\n'
     '  while c.next() { t = t + c.value() }\n  return i32(t) - 12 }'),
    ('D14 跨模块的 impl 经 dyn 派发', 'ok', {
        'tag.extc': 'trait Tag { fn tag(self: ref Self) -> i64 }\nfn go(d: dyn Tag) -> i64 { return d.tag() }',
        'thing.extc': 'use tag\nstruct thing { v: i64 }\n'
                     'impl Tag for thing { fn tag(self: ref thing) -> i64 { return self.v + i64(1) } }',
        'main.extc': 'use tag\nuse thing\nfn main() -> i32 { var t: thing::thing\n  t.v = i64(8)\n'
                     '  return i32(tag::go(dyn Tag(t))) - 9 }'}),
    ('D15 两个方法，其中一个没实现（完整性）', 'reject',
     'trait Two { fn a(self: ref Self) -> i64\n  fn b(self: ref Self) -> i64 }\n'
     'struct s { v: i64 }\nimpl Two for s { fn a(self: ref s) -> i64 { return self.v } }\n'
     'fn main() -> i32 { var x: s\n  x.v = i64(1)\n  var d: dyn Two = dyn Two(x)\n'
     '  return i32(d.a() + d.b()) }'),
    ('D16 dyn 值当结构体字段', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct s { v: i64 }\n'
     'impl Tag for s { fn tag(self: ref s) -> i64 { return self.v } }\n'
     'struct holder { d: dyn Tag }\n'
     'fn main() -> i32 { var a: s\n  a.v = i64(5)\n  var h: holder\n  h.d = dyn Tag(a)\n'
     '  return i32(h.d.tag()) - 5 }'),
]

# ---------------------------------------------------------------- arena / 逃逸
# 这一组**避开** tests/arena-soundness 已收录的族，专打"按级别规则**应当安全**"的形状：
# 返回 new 出来的值、跨调用提升、循环里保留最后一次、泛型函数返回、协程里持有 new 切片、
# 结构体里装 new 值再返回、alloc 在辅助函数里。判据是硬 oracle：ASan/UBSan 零报告 ✓。
ARENA = [
    ('R1 局部 new 在块内使用', 'ok',
     'fn main() -> i32 { var s: i64 = 0\n  { var p: mut ref i64 = new i64\n    *p = i64(7)\n'
     '    s = *p }\n  return i32(s) - 7 }'),
    ('R2 new 出来的值从函数返回（提升）', 'ok',
     'fn make() -> mut ref i64 { var p: mut ref i64 = new i64\n  *p = i64(9)\n  return p }\n'
     'fn main() -> i32 { var q: mut ref i64 = make()\n  return i32(*q) - 9 }'),
    ('R3 跨两层调用提升', 'ok',
     'fn inner() -> mut ref i64 { var p: mut ref i64 = new i64\n  *p = i64(5)\n  return p }\n'
     'fn outer() -> mut ref i64 { return inner() }\n'
     'fn main() -> i32 { var q: mut ref i64 = outer()\n  return i32(*q) - 5 }'),
    ('R4 new 切片从函数返回', 'ok',
     'fn make(n: i64) -> mut slice<i64> { var s: mut slice<i64> = new i64[3]\n'
     '  s[0] = n\n  s[1] = n + i64(1)\n  s[2] = n + i64(2)\n  return s }\n'
     'fn main() -> i32 { var s: mut slice<i64> = make(i64(10))\n'
     '  return i32(s[0] + s[1] + s[2]) - 33 }'),
    ('R5 循环里分配、保留最后一次', 'ok',
     'fn main() -> i32 { var keep: mut ref i64 = new i64\n  *keep = i64(0)\n  var i: i64 = 0\n'
     '  while i < i64(4) { var p: mut ref i64 = new i64\n    *p = i\n    keep = p\n'
     '    i = i + i64(1) }\n  return i32(*keep) - 3 }'),
    ('R6 泛型函数返回 new 出来的值', 'ok_or_reject',
     'fn mk<T>(v: T) -> mut ref T { var p: mut ref T = alloc<T>(1)\n  *p = v\n  return p }\n'
     'fn main() -> i32 { var p: mut ref i64 = mk(i64(8))\n  return i32(*p) - 8 }'),
    ('R7 结构体装 new 值再返回', 'ok',
     'struct holder { v: mut ref i64 }\n'
     # 含引用的结构体不能**零初始化**（零值引用会是 NULL ⇒ 语言直接拒绝）⇒ 用字面量一次写全。
     'fn make() -> holder { var h: holder = holder { v: new i64 }\n  *h.v = i64(3)\n  return h }\n'
     'fn main() -> i32 { let h = make()\n  return i32(*h.v) - 3 }'),
    ('R8 辅助函数里的 alloc 用于外层', 'ok_or_reject',
     'fn fill(dst: mut slice<i64>, v: i64) { var i: i64 = 0\n'
     '  while i < i64(dst.len) { dst[i] = v + i\n    i = i + i64(1) } }\n'
     'fn main() -> i32 { var s: mut slice<i64> = new i64[3]\n  fill(s, i64(5))\n'
     '  return i32(s[2]) - 7 }'),
    ('R9 协程里持有 new 切片跨 yield', 'ok_or_reject',
     'fn gen(n: i64) -> coroutine<i64> { var s: mut slice<i64> = new i64[2]\n  s[0] = n\n'
     '  s[1] = n + i64(1)\n  var i: i64 = 0\n  while i < i64(2) { yield s[i]\n'
     '    i = i + i64(1) } }\n'
     'fn main() -> i32 { var c = gen(i64(4))\n  var t: i64 = 0\n'
     '  while c.next() { t = t + c.value() }\n  return i32(t) - 9 }'),
    ('R10 alloc 计数为 0', 'ok_or_reject',
     'fn main() -> i32 { var p: mut ref i64 = alloc<i64>(0)\n  return 0 }'),
    ('R11 嵌套结构体里装 new 值并返回', 'ok_or_reject',
     'struct inner { v: mut ref i64 }\nstruct outer2 { c: inner }\n'
     'fn make() -> outer2 { var o: outer2\n  o.c.v = new i64\n  *o.c.v = i64(6)\n  return o }\n'
     'fn main() -> i32 { let o = make()\n  return i32(*o.c.v) - 6 }'),
    ('R12 new 值经两层容器返回', 'ok_or_reject',
     'struct inner { v: i64 }\nstruct outer2 { c: inner }\n'
     'fn make() -> outer2 { var o: outer2\n  o.c.v = i64(4)\n  return o }\n'
     'fn main() -> i32 { let o = make()\n  return i32(o.c.v) - 4 }'),
]

# ---------------------------------------------------------------- extern / C 互操作
# 这一面按设计是"**纯信任**"（签了 `effects Addr=0 Cont=0` 的声明若在 C 那侧撒谎，分析自然失效
# —— FFI 皆如此 ✓，属设计边界）。所以打的是**可验证的那半**：保守规则到底有没有被执行。
EXT = [
    ('E1 未签字的 extern 不许收本帧地址', 'reject',
     'extern!("libc") fn strlen(s: ref u8) -> i64\n'
     'fn main() -> i32 { var buf: [4]u8 = [65, 0, 0, 0]\n'
     '  return i32(strlen(buf[..].data)) }'),
    ('E2 跨边界传 slice（不许静默）', 'reject',
     'extern!("libc") fn write(fd: i32, buf: slice<u8>, n: i64) -> i64\n'
     'fn main() -> i32 { var b: [2]u8 = [1, 2]\n  return i32(write(i32(1), b[..], i64(2))) }'),
    ('E3 跨边界传 struct', 'reject',
     'struct pt { x: i64  y: i64 }\n'
     'extern!("libc") fn take(p: pt) -> i64\n'
     'fn main() -> i32 { var p: pt\n  p.x = i64(1)\n  p.y = i64(2)\n  return i32(take(p)) }'),
    ('E4 签字后可以传本帧地址（write 到 stdout）', 'ok',
     'extern!("libc") fn write(fd: i32, buf: ref u8, n: i64) -> i64 effects Addr=0 Cont=0\n'
     'fn main() -> i32 { var b: [2]u8 = [65, 10]\n  let n = write(i32(1), b[..].data, i64(2))\n'
     '  return i32(n) - 2 }'),
    ('E6 调元数写错', 'reject',
     'extern!("libc") fn getpid() -> i32\nfn main() -> i32 { return getpid(i32(1)) }'),
    ('E7 void 返回值当值用', 'reject',
     'extern!("libc") fn srand(seed: u32) -> void effects Addr=0 Cont=0\n'
     'fn main() -> i32 { return i32(srand(u32(1))) }'),
    ('E8 声明了但从不调用（死代码消除要对）', 'ok',
     'extern!("libc") fn getpid() -> i32\n'
     'extern!("libc") fn getppid() -> i32\n'
     'fn main() -> i32 { return 0 }'),
    ('E9 extern 名与 extC 侧函数同名', 'ok_or_reject',
     'extern!("libc") fn getpid() -> i32\nfn main() -> i32 { return 0 }'),
    ('E10 effects 子句写成别的形状', 'reject',
     'extern!("libc") fn getpid() -> i32 effects Nonsense=1\n'
     'fn main() -> i32 { return i32(getpid()) }'),
    ('E11 extern 调用放进协程体', 'ok_or_reject',
     'extern!("libc") fn write(fd: i32, buf: ref u8, n: i64) -> i64 effects Addr=0 Cont=0\n'
     'fn gen() -> coroutine<i64> { var b: [1]u8 = [66]\n  yield write(i32(1), b[0], i64(1))\n'
     '  yield i64(0) }\n'
     'fn main() -> i32 { var c = gen()\n  var t: i64 = 0\n'
     '  while c.next() { t = t + c.value() }\n  return i32(t) - 1 }'),
    ('E12 extern 返回值直接算进 i32', 'ok',
     'extern!("libc") fn getpid() -> i32\n'
     'fn main() -> i32 { let p = getpid()\n  if p > i32(0) { return 0 }\n  return 1 }'),
    ('E13 签字后把全局地址交出去', 'ok_or_reject',
     'extern!("libc") fn write(fd: i32, buf: ref u8, n: i64) -> i64 effects Addr=0 Cont=0\n'
     'var g: [4]u8 = [88, 10, 0, 0]\n'
     'fn main() -> i32 { let n = write(i32(1), g[..].data, i64(2))\n  return i32(n) - 2 }'),
    ('E14 extern 指针参数收 null', 'ok_or_reject',
     'extern!("libc") fn free(p: ref u8) -> void effects Addr=0 Cont=0\n'
     'fn main() -> i32 { return 0 }'),
]

# ---------------------------------------------------------------- fs / io
# 攻击面：句柄的生命周期（关两次、关后使用、逃出作用域）、错误路径、以及文档里那条
# "close 幂等靠句柄自己的标志，不许碰别人的 fd"（tests/fs/close-twice.extc 的设计）。
FSG = [
    # 题目写法一直在调整（openOut/openIn + fs::fin >> line 的官方形状），先按 ok_or_reject 计，
    # 免得把'题目没写对'记成编译器缺陷；写法定稿后再收紧成 ok。
    ('S1 写文件 → 关闭 → 再读回来', 'ok_or_reject',
     'use std::fs\nuse std::io\n'
     'fn main() -> i32 { {\n    var f = fs::openOut("build/atk-s1.out")!\n'
     '    fs::fout << "hello"\n    f.close()!\n  }\n'
     '  var g = fs::openIn("build/atk-s1.out")!\n'
     '  var line: mut slice<u8> = new u8[64]\n'
     '  fs::fin >> line\n  g.close()!\n'
     '  if line[0] == u8(104) { return 0 }\n  return 1 }'),
    ('S2 关两次：第二次不许碰别人的号', 'ok_or_reject',
     'use std::fs\n'
     'fn main() -> i32 { var a = fs::openWrite("build/atk-s2a.out")!\n'
     '  let fa = a.fd\n  a.close()!\n  a.close()!\n'
     '  var b = fs::openWrite("build/atk-s2b.out")!\n'
     '  fs::fout << "still fine" << "\\n"\n  b.close()!\n  return 0 }'),
    ('S3 关闭之后再用同一个句柄', 'ok_or_reject',
     'use std::fs\n'
     'fn main() -> i32 { var a = fs::openWrite("build/atk-s3.out")!\n'
     '  a.close()!\n  fs::fout << "after close" << "\\n"\n  return 0 }'),
    ('S4 打开一个不存在的文件（错误路径）', 'ok_or_reject',
     'use std::fs\n'
     'fn main() -> i32 { var g = fs::openRead("build/atk-does-not-exist.out")\n'
     '  return 0 }'),
    ('S5 句柄存进结构体', 'ok_or_reject',
     'use std::fs\nstruct holder { f: fs::file }\n'
     'fn main() -> i32 { var h: holder\n  h.f = fs::openWrite("build/atk-s5.out")!\n'
     '  fs::fout << "in struct" << "\\n"\n  h.f.close()!\n  return 0 }'),
    ('S6 句柄传进函数', 'ok_or_reject',
     'use std::fs\nfn writeIt(f: mut ref fs::file) { fs::fout << "via fn" << "\\n" }\n'
     'fn main() -> i32 { var f = fs::openWrite("build/atk-s6.out")!\n  writeIt(ref f)\n'
     '  f.close()!\n  return 0 }'),
    ('S7 两个句柄读同一个文件互不影响', 'ok_or_reject',
     'use std::fs\n'
     'fn main() -> i32 { var a = fs::openRead("tests/fs/data.txt")!\n'
     '  var b = fs::openRead("tests/fs/data.txt")!\n  a.close()!\n  b.close()!\n  return 0 }'),
    ('S8 循环里开一千次（fd 不许漂）', 'ok_or_reject',
     'use std::fs\n'
     'fn main() -> i32 { var first: i32 = i32(-1)\n  var i: i64 = 0\n'
     '  while i < 1000 {\n    var f = fs::openWrite("build/atk-s8.out")!\n'
     '    if i == 0 { first = f.fd }\n    if f.fd != first { return 1 }\n'
     '    f.close()!\n    i = i + 1 }\n  return 0 }'),
    ('S9 句柄逃出它所在的作用域', 'reject',
     'use std::fs\n'
     'fn main() -> i32 { var g: fs::file = fs::openWrite("build/atk-s9.out")!\n'
     '  { var f = fs::openWrite("build/atk-s9b.out")!\n    g = f }\n'
     '  fs::fout << "escaped" << "\\n"\n  g.close()!\n  return 0 }'),
    ('S10 读一个空文件 / 读到 EOF', 'ok_or_reject',
     'use std::fs\n'
     'fn main() -> i32 { { var w = fs::openWrite("build/atk-s10.out")!\n    w.close()! }\n'
     '  var g = fs::openRead("build/atk-s10.out")!\n  var line: string = ""\n'
     '  fs::fin >> line\n  g.close()!\n  return 0 }'),
    ('S11 写完不关就退出（析构/泄漏路径）', 'ok_or_reject',
     'use std::fs\n'
     'fn main() -> i32 { var f = fs::openWrite("build/atk-s11.out")!\n'
     '  fs::fout << "no close" << "\\n"\n  return 0 }'),
    ('S12 读目录当文件', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 { var g = fs::openRead("tests/fs")\n  return 0 }'),
]

# ---------------------------------------------------------------- nocopy / ctor / ops
# 攻击面：移动语义（@noCopy）、构造函数糖 `T(args)`、算符重载（含 `!=` 的派生与异构重载）。
NOC = [
    ('P1 @noCopy 值被移走后不能再 use', 'reject',
     '@noCopy struct c { n: i64 }\nfn take(x: c) -> i64 { return x.n }\n'
     'fn main() -> i32 { var a: c = { n: i64(1) }\n  var v = take(a)\n'
     '  return i32(v) + i32(a.n) }'),
    ('P2 @noCopy 按 ref 传（允许）', 'ok',
     '@noCopy struct c { n: i64 }\nfn peek(x: ref c) -> i64 { return x.n }\n'
     'fn main() -> i32 { var a: c = { n: i64(7) }\n  return i32(peek(ref a)) - 7 }'),
    ('P3 @noCopy 读字段 / 调方法（允许）', 'ok',
     '@noCopy struct c { n: i64\n  fn bump(self: mut ref c) -> i64 { self.n = self.n + i64(1)\n'
     '    return self.n } }\n'
     'fn main() -> i32 { var a: c = { n: i64(1) }\n  let x = a.bump()\n'
     '  return i32(x + a.n) - 4 }'),
    # 语言没有"从局部变量移动出去"：值位置使用一律算拷贝（官方 tests/nocopy/errors 的
    # bind_copy/assign_copy 就是这么判的）⇒ `return a` 同样是拷贝，必须拒；要返回新值就直接
    # 返回字面量（官方 ctor 用例的写法）。
    ('P4 @noCopy 从局部变量 return（值位置 = 拷贝 ⇒ 应拒）', 'reject',
     '@noCopy struct c { n: i64 }\nfn make() -> c { var a: c = { n: i64(5) }\n  return a }\n'
     'fn main() -> i32 { let a = make()\n  return i32(a.n) - 5 }'),
    ('P4b @noCopy 直接返回新字面量（允许）', 'ok',
     '@noCopy struct c { n: i64 }\nfn make() -> c { return { n: i64(5) } }\n'
     'fn main() -> i32 { let a = make()\n  return i32(a.n) - 5 }'),
    ('P5 @noCopy 赋给另一个变量后再用源（应拒）', 'reject',
     '@noCopy struct c { n: i64 }\n'
     'fn main() -> i32 { var a: c = { n: i64(1) }\n  var b: c = a\n  return i32(a.n + b.n) }'),
    ('P6 @noCopy 在结构体字面量里被复制（应拒）', 'reject',
     '@noCopy struct c { n: i64 }\nstruct boxer { v: c }\n'
     'fn main() -> i32 { var a: c = { n: i64(1) }\n  var b: boxer = { v: a }\n'
     '  return i32(a.n) + i32(b.v.n) }'),
    ('P7 构造糖 T(args) 走 new', 'ok',
     'struct pt { x: i64  y: i64\n  fn new(x: i64, y: i64) -> pt { return { x: x, y: y } } }\n'
     'fn main() -> i32 { let p = pt(3, 4)\n  return i32(p.x + p.y) - 7 }'),
    ('P8 构造糖元数写错（应拒）', 'reject',
     'struct pt { x: i64\n  fn new(x: i64) -> pt { return { x: x } } }\n'
     'fn main() -> i32 { let p = pt(1, 2)\n  return i32(p.x) }'),
    ('P9 new 返回别的类型（应拒）', 'reject',
     'struct pt { x: i64\n  fn new(x: i64) -> i64 { return x } }\n'
     'fn main() -> i32 { let p = pt(1)\n  return 0 }'),
    ('P10 可失败的构造（`!`/`?` 路径）', 'ok_or_reject',
     'struct pt { x: i64\n  fn new(x: i64) -> pt? {\n    if x < i64(0) { return null }\n'
     '    return { x: x } } }\n'
     'fn main() -> i32 { var p = pt(3)?\n  return i32(p.x) - 3 }'),
    ('P11 泛型结构的构造糖', 'ok_or_reject',
     'struct pair2<T> { a: T\n  fn new(a: T) -> pair2<T> { return { a: a } } }\n'
     'fn main() -> i32 { let p = pair2<i64>(7)\n  return i32(p.a) - 7 }'),
    ('P12 == 与派生的 !=', 'ok',
     'struct onlyEq { n: i64\n  fn ==(self: ref onlyEq, o: ref onlyEq) -> bool { return self.n == o.n } }\n'
     'fn main() -> i32 { var a: onlyEq = { n: i64(5) }\n  var b: onlyEq = { n: i64(5) }\n'
     '  if a == b && !(a != b) { return 0 }\n  return 1 }'),
    ('P13 == 的右操作数类型不对（应拒）', 'reject',
     'struct onlyEq { n: i64\n  fn ==(self: ref onlyEq, o: ref onlyEq) -> bool { return self.n == o.n } }\n'
     'fn main() -> i32 { var a: onlyEq = { n: i64(5) }\n  if a == i64(5) { return 0 }\n  return 1 }'),
    ('P14 同一算符的两个异构重载', 'ok_or_reject',
     'struct v2 { n: i64\n'
     '  fn ==(self: ref v2, o: ref v2) -> bool { return self.n == o.n }\n'
     '  fn ==(self: ref v2, o: i64) -> bool { return self.n == o } }\n'
     'fn main() -> i32 { var a: v2 = { n: i64(5) }\n  if a == a && a == i64(5) { return 0 }\n  return 1 }'),
    ('P15 比较算符返回非 bool（应拒）', 'reject',
     'struct w { n: i64\n  fn <(self: ref w, o: ref w) -> i64 { return self.n - o.n } }\n'
     'fn main() -> i32 { var a: w = { n: i64(1) }\n  if a < a { return 0 }\n  return 1 }'),
    ('P16 @noCopy 放进数组字面量', 'ok_or_reject',
     '@noCopy struct c { n: i64 }\n'
     'fn main() -> i32 { var a: c = { n: i64(1) }\n  var arr: [1]c = [a]\n'
     '  return i32(arr[0].n) - 1 }'),
]

# ---------------------------------------------------------------- parallel::run（第 ③ 步：内建 + trampoline + worker 安全检查）
# 正例：分区正确性（worker 拿到的是切好的那一段，下标从 0 数）、安全辅助函数链、大 n × 8 线程。
# 反例：签名不符、worker 里 println / new / 模块级 var、参数个数、第一个实参不是函数名，
#       以及**传递**反例（经辅助函数间接碰 println / 全局）。
PARG = [
    ('P1 分区正确性（8 线程各写自己那段）', 'ok',
     'use std::parallel\nfn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {\n  var i: i64 = lo\n  while i < hi { out[i - lo] = i * i64(2)  i = i + i64(1) }\n  return 0 }\nfn main() -> i32 {\n  var out: mut slice<i64> = new i64[64]\n  if parallel::run(w, out, i64(64), i64(8)) != i32(0) { return i32(1) }\n  return i32(out[63]) - 126 }'),
    ('P2 worker 签名不符（应拒）', 'reject',
     'use std::parallel\nfn w(id: i64, lo: i64, hi: i64) -> i32 { return 0 }\nfn main() -> i32 { var out: mut slice<i64> = new i64[64]\n  return parallel::run(w, out, i64(64), i64(8)) }'),
    ('P3 worker 里 println（应拒）', 'reject',
     'use std::parallel\nuse std::io\nfn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {\n  println("x")\n  out[lo] = i64(1)\n  return 0 }\nfn main() -> i32 { var out: mut slice<i64> = new i64[64]\n  return parallel::run(w, out, i64(64), i64(8)) }'),
    ('P4 worker 里 new（走线程本地 arena，③c 已完成）', 'ok',
     'use std::parallel\nfn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {\n  var t: mut slice<i64> = new i64[8]\n  out[0] = t[0]   /* out 是分区后的 view：下标从 0 数（实测踩过两次） */\n  return 0 }\nfn main() -> i32 { var out: mut slice<i64> = new i64[64]\n  return parallel::run(w, out, i64(64), i64(8)) }'),
    ('P5 worker 读模块级 var（应拒）', 'reject',
     'use std::parallel\nvar G: i64 = 7\nfn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 { out[lo] = G\n  return 0 }\nfn main() -> i32 { var out: mut slice<i64> = new i64[64]\n  return parallel::run(w, out, i64(64), i64(8)) }'),
    ('P6 参数个数不对（应拒）', 'reject',
     'use std::parallel\nfn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 { return 0 }\nfn main() -> i32 { var out: mut slice<i64> = new i64[64]\n  return parallel::run(w, out, i64(64)) }'),
    ('P7 第一个实参不是函数名（应拒）', 'reject',
     'use std::parallel\nfn main() -> i32 { var out: mut slice<i64> = new i64[64]\n  return parallel::run(i64(3), out, i64(64), i64(8)) }'),
    ('P8 安全辅助函数链（允许 ✓）', 'ok',
     'use std::parallel\nfn helper(x: i64) -> i64 { return x + i64(1) }\nfn helper2(x: i64) -> i64 { return helper(x) * i64(2) }\nfn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {\n  var i: i64 = lo\n  while i < hi { out[i - lo] = helper2(i)  i = i + i64(1) }\n  return 0 }\nfn main() -> i32 { var out: mut slice<i64> = new i64[64]\n  return parallel::run(w, out, i64(64), i64(8)) }'),
    ('P9 经辅助函数间接碰 println（应拒）', 'reject',
     'use std::parallel\nuse std::io\nfn helper(x: i64) -> i64 { println("deep")  return x }\nfn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {\n  var i: i64 = lo\n  while i < hi { out[i - lo] = helper(i)  i = i + i64(1) }\n  return 0 }\nfn main() -> i32 { var out: mut slice<i64> = new i64[64]\n  return parallel::run(w, out, i64(64), i64(8)) }'),
    ('P10 经辅助函数间接读全局（应拒）', 'reject',
     'use std::parallel\nvar G: i64 = 7\nfn helper(x: i64) -> i64 { return x + G }\nfn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {\n  var i: i64 = lo\n  while i < hi { out[i - lo] = helper(i)  i = i + i64(1) }\n  return 0 }\nfn main() -> i32 { var out: mut slice<i64> = new i64[64]\n  return parallel::run(w, out, i64(64), i64(8)) }'),
    ('P11 大 n × 8 线程（功能 ✓）', 'ok',
     'use std::parallel\nfn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {\n  var i: i64 = lo\n  while i < hi { out[i - lo] = i  i = i + i64(1) }\n  return 0 }\nfn main() -> i32 {\n  let n: i64 = i64(4096)\n  var out: mut slice<i64> = new i64[n]\n  if parallel::run(w, out, n, i64(8)) != i32(0) { return i32(1) }\n  return i32(out[n - i64(1)]) - 4095 }'),
    ('P12 只读共享输入（允许 ✓）', 'ok',
     'use std::parallel\nfn w(id: i64, lo: i64, hi: i64, a: slice<i64>, out: mut slice<i64>) -> i32 {\n  var i: i64 = lo\n  while i < hi { out[i - lo] = a[i] + a[i] * i64(2)  i = i + i64(1) }\n  return 0 }\nfn main() -> i32 {\n  let n: i64 = i64(64)\n  var a: mut slice<i64> = new i64[n]\n  var out: mut slice<i64> = new i64[n]\n  var i: i64 = 0\n  while i < n { a[i] = i  i = i + i64(1) }\n  if parallel::run(w, a[..], out, n, i64(8)) != i32(0) { return i32(1) }\n  return i32(out[63]) - 189 }'),
    ('P13 两个 mut 视图（应拒）', 'reject',
     'use std::parallel\nfn w(id: i64, lo: i64, hi: i64, a: mut slice<i64>, b: mut slice<i64>) -> i32 { return 0 }\nfn main() -> i32 { var a: mut slice<i64> = new i64[8]  var b: mut slice<i64> = new i64[8]\n  return parallel::run(w, a, b, i64(8), i64(2)) }'),
    ('P14 只读输入元素类型不符（应拒）', 'reject',
     'use std::parallel\nfn w(id: i64, lo: i64, hi: i64, a: slice<f32>, out: mut slice<i64>) -> i32 { return 0 }\nfn main() -> i32 { var a: mut slice<i64> = new i64[8]  var out: mut slice<i64> = new i64[8]\n  return parallel::run(w, a, out, i64(8), i64(2)) }'),
    ('P15 标量当只读视图（应拒）', 'reject',
     'use std::parallel\nfn w(id: i64, lo: i64, hi: i64, k: slice<i64>, out: mut slice<i64>) -> i32 { return 0 }\nfn main() -> i32 { var out: mut slice<i64> = new i64[8]\n  return parallel::run(w, i64(3), out, i64(8), i64(2)) }'),
]

# ---------------------------------------------------------------- @inline × 代码消除
# 与刚修过三条 bug 的死代码族相邻：@inline 的函数被消除 / 只被死代码调用 / 递归 / 泛型实例 /
# 方法 / 协程 / 返回泛型实例 / 两个互调 / 落在 main 或 extern 上（反例）。
INL = [
    ('I1 @inline 被调用的普通函数', 'ok',
     '@inline fn sq(x: i64) -> i64 { return x * x }\nfn main() -> i32 { return i32(sq(i64(7))) - 49 }'),
    ('I2 @inline 但没人调用（应被消除）', 'ok',
     '@inline fn unused(x: i64) -> i64 { return x * x }\nfn main() -> i32 { return 0 }'),
    ('I3 @inline 递归函数', 'ok_or_reject',
     '@inline fn fact(n: i64) -> i64 { if n <= i64(1) { return i64(1) }\n  return n * fact(n - i64(1)) }\nfn main() -> i32 { return i32(fact(i64(5))) - 120 }'),
    ('I4 @inline 只被死代码调用', 'ok_or_reject',
     '@inline fn twice(x: i64) -> i64 { return x + x }\nfn dead() -> i64 { return twice(i64(3)) }\nfn main() -> i32 { return 0 }'),
    ('I5 @inline 泛型函数被调用', 'ok',
     '@inline fn id<T>(v: T) -> T { return v }\nfn main() -> i32 { return i32(id(i64(4))) - 4 }'),
    ('I6 @inline 泛型实例没人调用', 'ok',
     '@inline fn id<T>(v: T) -> T { return v }\nfn main() -> i32 { return 0 }'),
    ('I7 @inline 方法（结构体体内）', 'ok',
     'struct pt { x: i64\n  @inline fn get(self: ref pt) -> i64 { return self.x } }\nfn main() -> i32 { var p: pt\n  p.x = i64(5)\n  return i32(p.get()) - 5 }'),
    ('I8 @private @inline 同文件使用', 'ok_or_reject',
     '@private @inline fn twice(x: i64) -> i64 { return x + x }\nfn main() -> i32 { return i32(twice(i64(3))) - 6 }'),
    ('I9 @inline 落在 main 上', 'ok_or_reject',
     '@inline fn main() -> i32 { return 0 }'),
    ('I10 @inline 协程函数', 'ok_or_reject',
     '@inline fn gen(n: i64) -> coroutine<i64> { yield n }\nfn main() -> i32 { var c = gen(i64(2))\n  var s: i64 = 0\n  while c.next() { s = s + c.value() }\n  return i32(s) - 2 }'),
    ('I11 @inline 返回泛型实例', 'ok_or_reject',
     'struct box<T> { v: T }\n@inline fn mk<T>(v: T) -> box<T> { return { v: v } }\nfn main() -> i32 { return i32(mk(i64(3)).v) - 3 }'),
    ('I12 两个 @inline 函数互调', 'ok_or_reject',
     '@inline fn odd(n: i64) -> bool { if n == i64(0) { return false }\n  return even(n - i64(1)) }\n@inline fn even(n: i64) -> bool { if n == i64(0) { return true }\n  return odd(n - i64(1)) }\nfn main() -> i32 { if even(i64(4)) { return 0 }\n  return 1 }'),
    ('I13 @inline 落在 extern 上（应拒）', 'reject',
     '@inline extern!("libc") fn getpid() -> i32\nfn main() -> i32 { return 0 }'),
]

# ---------------------------------------------------------------- 递归 / 互递归类型（时机族之后的新面）
# 缘起：match 面第一题就撞出 H16（自引用枚举让三个类型遍历函数无限递归 ⇒ SIGSEGV）⇒ 这一族值得再打：
# 互递归枚举、载荷是自身的 option/数组、三跳互递归、递归遍历函数、?ref 自引用（设计意图）、
# 泛型枚举自引用、深层嵌套实例（TYPE_DEPTH_LIMIT 那条路）、结构体与枚举互指、递归类型只在泛型函数里出现。
REC = [
    ('R1 互递归枚举', 'ok_or_reject',
     'type a = | a1(b) | a0\ntype b = | b1(a) | b0\nfn main() -> i32 { var x: a\n  x = a.a0\n  return 0 }'),
    ('R2 自引用枚举 + 零初始化（应拒）', 'reject',
     'type list = | cons(i64, list) | nil\nfn main() -> i32 { var l: list\n  return 0 }'),
    ('R3 载荷是自身的 option', 'ok_or_reject',
     'type l = | c(i64, l?) | n\nfn main() -> i32 { var x: l\n  x = l.n\n  return 0 }'),
    ('R4 载荷是自身的数组', 'ok_or_reject',
     'type t = | arr([2]t) | stop\nfn main() -> i32 { var x: t\n  x = t.stop\n  return 0 }'),
    ('R5 三跳互递归', 'ok_or_reject',
     'type a = | a1(b) | a0\ntype b = | b1(c) | b0\ntype c = | c1(a) | c0\nfn main() -> i32 { var x: a\n  x = a.a0\n  return 0 }'),
    ('R6 递归函数遍历自引用枚举', 'ok_or_reject',
     'type list = | cons(i64, list) | nil\nfn len(l: list) -> i64 { match l { cons(h, t) => { return i64(1) + len(t) }\n    nil => { return i64(0) } } }\nfn main() -> i32 { var l: list\n  l = list.nil\n  return i32(len(l)) }'),
    ('R7 ?ref 自引用（设计意图：链表 null）', 'ok_or_reject',
     'struct node { v: i64  next: ref node? }\nfn main() -> i32 { var n: node\n  n.v = i64(1)\n  n.next = null\n  return i32(n.v) - 1 }'),
    ('R8 泛型枚举自引用', 'ok_or_reject',
     'type nest<T> = | more(nest<T>) | stop\nfn main() -> i32 { var x: nest<i64>\n  x = nest<i64>.stop\n  return 0 }'),
    ('R9 深层嵌套实例（box^20）', 'ok_or_reject',
     'struct box<T> { v: T }\nfn main() -> i32 { var b: box<box<box<box<box<box<box<box<box<box<i64>>>>>>>>>>\n  return 0 }'),
    ('R10 结构体与枚举互指', 'ok_or_reject',
     'type st = | has(holder) | none\nstruct holder { s: st }\nfn main() -> i32 { var h: holder\n  h.s = st.none\n  return 0 }'),
    ('R11 递归类型只出现在泛型函数里', 'ok_or_reject',
     'type list = | cons(i64, list) | nil\nfn make<T>(v: T) -> list { var l: list\n  l = list.nil\n  return l }\nfn main() -> i32 { var l = make(i64(1))\n  return 0 }'),
    ('R12 自引用枚举的 match 穷尽性（应拒）', 'reject',
     'type list = | cons(i64, list) | nil\nfn main() -> i32 { var l: list\n  l = list.nil\n  match l { nil => { return 0 } }\n  return 1 }'),
]

# ---------------------------------------------------------------- match / 枚举穷尽性
# 语法：`type why = | neg(i64) | other` 声明；`match w { neg(n) => {...}  other => {...} }`。
# 打：全变体匹配、漏变体（穷尽性）、兜底臂、载荷绑定、载荷是泛型实例（与 time 族交叉）、
# 嵌套 match、option/result 的 match、各臂返回值、整数字面量、自引用枚举、泛型函数里 match、枚举进容器。
MATCH = [
    ('M1 枚举全变体都匹配', 'ok',
     'type why = | neg(i64) | other\nfn main() -> i32 { var w: why\n  w = why.neg(i64(3))\n  match w { neg(n) => { if n != i64(3) { return 1 } }\n    other => { return 2 } }\n  return 0 }'),
    ('M2 漏掉一个变体（应拒）', 'reject',
     'type why = | neg(i64) | other\nfn main() -> i32 { var w: why\n  w = why.neg(i64(3))\n  match w { neg(n) => { return 0 } }\n  return 1 }'),
    ('M3 用通配/兜底臂', 'ok_or_reject',
     'type why = | neg(i64) | other\nfn main() -> i32 { var w: why\n  w = why.other\n  match w { neg(n) => { return 1 }\n    _ => { return 0 } } }'),
    ('M4 载荷绑定并使用', 'ok',
     'type why = | neg(i64) | other\nfn main() -> i32 { var w: why\n  w = why.neg(i64(9))\n  match w { neg(n) => { return i32(n) - 9 }\n    other => { return 1 } } }'),
    ('M5 载荷是泛型实例', 'ok_or_reject',
     'struct box<T> { v: T }\ntype wrap = | has(box<i64>) | none\nfn main() -> i32 { var x: wrap\n  x = wrap.has(box<i64> { v: i64(4) })\n  match x { has(b) => { return i32(b.v) - 4 }\n    none => { return 1 } } }'),
    ('M6 嵌套 match', 'ok_or_reject',
     'type why = | neg(i64) | other\nfn main() -> i32 { var w: why\n  w = why.neg(i64(1))\n  match w { neg(n) => { match w { neg(m) => { return i32(m) - 1 }\n      other => { return 2 } } }\n    other => { return 3 } } }'),
    ('M7 match option（null / x）', 'ok_or_reject',
     'fn main() -> i32 { var o: i64? = null\n  match o { null => { return 0 }\n    x => { return 1 } } }'),
    ('M8 match result（success / failure）', 'ok_or_reject',
     'type why = | neg(i64) | other\nfn f(n: i64) -> result<i64, why> { if n < i64(0) { return failure(why.neg(n)) }\n  return success(n) }\nfn main() -> i32 { match f(i64(2)) {\n    success(v) => { return i32(v) - 2 }\n    failure(e) => { return 1 } } }'),
    ('M9 两个臂绑同一个名字（应拒或允许）', 'ok_or_reject',
     'type why = | neg(i64) | other\nfn main() -> i32 { var w: why\n  w = why.other\n  match w { neg(n) => { return 1 }\n    other => { return 0 } } }'),
    ('M10 各臂返回值', 'ok_or_reject',
     'type why = | neg(i64) | other\nfn pick(w: why) -> i64 { match w { neg(n) => { return n }\n    other => { return i64(0) } } }\nfn main() -> i32 { return i32(pick(why.neg(i64(5)))) - 5 }'),
    ('M11 match 整数字面量', 'ok_or_reject',
     'fn main() -> i32 { var n: i64 = i64(1)\n  match n { 1 => { return 0 }\n    _ => { return 1 } } }'),
    ('M12 自引用枚举的载荷', 'ok_or_reject',
     'type list = | cons(i64, list) | nil\nfn main() -> i32 { var l: list\n  l = list.nil\n  return 0 }'),
    ('M13 泛型函数里 match 枚举', 'ok_or_reject',
     'type why = | neg(i64) | other\nfn pick<T>(w: why) -> i64 { match w { neg(n) => { return n }\n    other => { return i64(0) } } }\nfn main() -> i32 { return i32(pick(i64(1), why.neg(i64(7)))) - 7 }'),
    ('M14 枚举实例进容器', 'ok_or_reject',
     'use stl::vector\ntype why = | neg(i64) | other\nfn main() -> i32 { var v = vector<why>::new()\n  v.push(why.neg(i64(1)))\n  let dv = v.toSlice()\n  match dv[i64(0)] { neg(n) => { return i32(n) - 1 }\n    other => { return 1 } } }'),
]

# ---------------------------------------------------------------- 时机族：实例只出现在泛型体内
# W9（第 19 轮修）是「局部声明里的类型实例 intern 太晚」，修法只覆盖 ST_VAR 的类型标注；
# 这一组专打**其它**「实例只出现在泛型体内」的形状。拼写要点：泛型体内**不能**写
# `box<T> { … }`（参数不能当显式实参）⇒ 要么用上下文定型的裸字面量 `{ v: v }`，
# 要么先 `var b: box<T>` 再赋字段。
TIM = [
    ('X1 泛型函数返回泛型实例', 'ok',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> box<T> { return { v: v } }\nfn main() -> i32 { let c = mk(i64(2))\n  return i32(c.v) - 2 }'),
    ('X2 嵌套泛型实例 box<box<T>>', 'ok_or_reject',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> box<box<T>> { var inner: box<T>\n  inner.v = v\n  return { v: inner } }\nfn main() -> i32 { let c = mk(i64(3))\n  return i32(c.v.v) - 3 }'),
    ('X3 容器元素是泛型实例', 'ok_or_reject',
     'use stl::vector\nstruct box<T> { v: T }\nfn mk<T>(v: T) -> i64 { var vec = vector<box<T>>::new()\n  var b: box<T>\n  b.v = v\n  vec.push(b)\n  let dv = vec.toSlice()\n  return i64(0) }\nfn main() -> i32 { return i32(mk(i64(1))) }'),
    ('X4 数组元素是泛型实例', 'ok_or_reject',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> i64 { var b: box<T>\n  b.v = v\n  var arr: [2]box<T> = [b, b]\n  return i64(0) }\nfn main() -> i32 { return i32(mk(i64(1))) }'),
    ('X5 new 一个泛型实例数组', 'ok_or_reject',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> i64 { var arr: mut slice<box<T>> = new box<T>[2]\n  return i64(0) }\nfn main() -> i32 { return i32(mk(i64(1))) }'),
    ('X6 泛型实例的 option（match 载荷）', 'ok_or_reject',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> i64 { var o: box<T>? = null\n  match o { null => { return i64(0) }\n    x => { return i64(1) } } }\nfn main() -> i32 { return i32(mk(i64(1))) }'),
    ('X7 泛型实例作为别的泛型的字段', 'ok_or_reject',
     'struct box<T> { v: T }\nstruct pair2<A, B> { x: A  y: B }\nfn mk<T>(v: T) -> i64 { var inner: box<T>\n  inner.v = v\n  var p: pair2<box<T>, i64>\n  p.x = inner\n  p.y = i64(0)\n  return i64(0) }\nfn main() -> i32 { return i32(mk(i64(1))) }'),
    ('X8 协程 yield 泛型实例', 'ok_or_reject',
     'struct box<T> { v: T }\nfn gen<T>(v: T) -> coroutine<box<T>> { var b: box<T>\n  b.v = v\n  yield b }\nfn main() -> i32 { var c = gen(i64(4))\n  var s: i64 = 0\n  while c.next() { s = s + c.value().v }\n  return i32(s) - 4 }'),
    ('X9 泛型实例作切片元素', 'ok_or_reject',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> i64 { var b: box<T>\n  b.v = v\n  var arr: [1]box<T> = [b]\n  let sl = arr[..]\n  return i64(0) }\nfn main() -> i32 { return i32(mk(i64(1))) }'),
    ('X10 泛型实例传给另一个泛型函数', 'ok',
     'struct box<T> { v: T }\nfn use1<T>(b: box<T>) -> i64 { return b.v }\nfn mk<T>(v: T) -> i64 { var b: box<T>\n  b.v = v\n  return use1(b) }\nfn main() -> i32 { return i32(mk(i64(7))) - 7 }'),
    ('X11 泛型实例的返回值直接使用', 'ok',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> box<T> { return { v: v } }\nfn main() -> i32 { return i32(mk(i64(5)).v) - 5 }'),
    ('X12 两层泛型函数间的实例传递', 'ok',
     'struct box<T> { v: T }\nfn inner<T>(v: T) -> box<T> { return { v: v } }\nfn outer<T>(v: T) -> box<T> { return inner(v) }\nfn main() -> i32 { return i32(outer(i64(6)).v) - 6 }'),
    # --- 第三批（第 36 轮）：实例只出现在泛型体内的**其它**形状——match 载荷绑定、方法返回类型、
    # 枚举载荷、嵌套数组、协程帧局部、dyn 载荷、option 载荷、同体内两个不同实例。
    ('Y1 实例只出现在 match 载荷绑定', 'ok_or_reject',
     'struct box<T> { v: T }\ntype w = | has(box<i64>) | none\nfn pick<T>(x: T) -> i64 { var y: w\n  y = w.has(box<i64> { v: i64(1) })\n  match y { has(b) => { return b.v }\n    none => { return i64(0) } } }\nfn main() -> i32 { return i32(pick(i64(1))) - 1 }'),
    ('Y2 实例只作为方法返回类型', 'ok_or_reject',
     'struct box<T> { v: T }\nfn make<T>(v: T) -> box<i64> { return box<i64> { v: i64(9) } }\nfn main() -> i32 { return i32(make(i64(0)).v) - 9 }'),
    ('Y3 实例只出现在枚举载荷里', 'ok_or_reject',
     'struct box<T> { v: T }\ntype w = | has(box<i64>) | none\nfn mk<T>(v: T) -> w { return w.has(box<i64> { v: i64(2) }) }\nfn main() -> i32 { match mk(i64(0)) { has(b) => { return i32(b.v) - 2 }\n    none => { return 1 } } }'),
    ('Y4 实例只作为嵌套数组元素', 'ok_or_reject',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> i64 { var a: [2][2]box<i64> = [[box<i64> { v: i64(1) }, box<i64> { v: i64(1) }],\n    [box<i64> { v: i64(1) }, box<i64> { v: i64(1) }]]\n  return i64(0) }\nfn main() -> i32 { return i32(mk(i64(0))) }'),
    ('Y5 实例只出现在协程帧的局部', 'ok_or_reject',
     'struct box<T> { v: T }\nfn gen<T>(v: T) -> coroutine<i64> { var b: box<i64>\n  b.v = i64(3)\n  yield b.v }\nfn main() -> i32 { var c = gen(i64(0))\n  var s: i64 = 0\n  while c.next() { s = s + c.value() }\n  return i32(s) - 3 }'),
    ('Y6 实例只作为 dyn 载荷', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct box<T> { v: T }\nimpl Tag for box<i64> { fn tag(self: ref box<i64>) -> i64 { return self.v } }\nfn mk<T>(v: T) -> i64 { var b: box<i64>\n  b.v = i64(4)\n  var d: dyn Tag = dyn Tag(b)\n  return d.tag() }\nfn main() -> i32 { return i32(mk(i64(0))) - 4 }'),
    ('Y7 实例只出现在 option 载荷', 'ok_or_reject',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> i64 { var o: box<i64>? = null\n  return i64(0) }\nfn main() -> i32 { return i32(mk(i64(0))) }'),
    ('Y8 一个泛型体内两个不同实例', 'ok_or_reject',
     'struct box<T> { v: T }\nfn mk<T>(v: T) -> i64 { var a: box<i64>\n  a.v = i64(1)\n  var b: box<i32>\n  b.v = i32(2)\n  return i64(0) }\nfn main() -> i32 { return i32(mk(i64(0))) }'),
]

DEEP = [
    ('W1 泛型协程实例装箱后驱动', 'ok',
     'fn gen<T>(x: T) -> coroutine<T> { yield x }\nfn main() -> i32 { var c = gen(i64(3))\n  var s: i64 = 0\n  while c.next() { s = s + c.value() }\n  return i32(s) - 3 }'),
    ('W2 泛型协程在泛型函数里被驱动（B8 形状）', 'ok',
     'fn gen<T>(x: T) -> coroutine<T> { yield x }\nfn run<T>(v: T) -> i64 { var c = gen(v)\n  var s: i64 = 0\n  while c.next() { s = s + c.value() }\n  return s }\nfn main() -> i32 { return i32(run(i64(4))) - 4 }'),
    ('W3 泛型协程 yield 结构体', 'ok_or_reject',
     'struct pt { x: i64  y: i64 }\nfn gen<T>(v: T) -> coroutine<pt> { yield pt { x: i64(1), y: i64(2) }\n  yield pt { x: i64(3), y: i64(4) } }\nfn main() -> i32 { var c = gen(i64(0))\n  var s: i64 = 0\n  while c.next() { s = s + c.value().x + c.value().y }\n  return i32(s) - 10 }'),
    ('W4 同一泛型协程两个类型实例同程序', 'ok_or_reject',
     'fn gen<T>(x: T) -> coroutine<T> { yield x }\nfn main() -> i32 { var a = gen(i64(2))\n  var b = gen(i32(3))\n  var sa: i64 = 0\n  while a.next() { sa = sa + a.value() }\n  var sb: i32 = 0\n  while b.next() { sb = sb + b.value() }\n  return i32(sa) + i32(sb) - 5 }'),
    ('W5 泛型协程 for-in 驱动', 'ok_or_reject',
     'fn gen<T>(x: T) -> coroutine<T> { yield x\n  yield x }\nfn main() -> i32 { var s: i64 = 0\n  for v in gen(i64(5)) { s = s + v }\n  return i32(s) - 10 }'),
    ('W6 泛型协程用 send 喂值', 'ok_or_reject',
     'fn gen<T>(x: T) -> coroutine<i64> { yield i64(1)\n  yield i64(2) }\nfn main() -> i32 { var c = gen(i64(0))\n  var s: i64 = 0\n  while c.next() { s = s + c.value() }\n  return i32(s) - 3 }'),
    ('W7 同一 trait 的两个泛型实例经 dyn 派发', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct pair<T> { a: T }\nimpl<T> Tag for pair<T> { fn tag(self: ref pair<T>) -> i64 { return i64(1) } }\nstruct single<T> { b: T }\nimpl<T> Tag for single<T> { fn tag(self: ref single<T>) -> i64 { return i64(2) } }\nfn main() -> i32 { var p: pair<i64>\n  p.a = i64(0)\n  var s: single<i64>\n  s.b = i64(0)\n  var d1: dyn Tag = dyn Tag(p)\n  var d2: dyn Tag = dyn Tag(s)\n  return i32(d1.tag() + d2.tag()) - 3 }'),
    ('W8 泛型 impl 的方法带参数', 'ok_or_reject',
     'trait Add2 { fn add2(self: ref Self, k: i64) -> i64 }\nstruct box<T> { v: T }\nimpl<T> Add2 for box<T> { fn add2(self: ref box<T>, k: i64) -> i64 { return k + i64(1) } }\nfn main() -> i32 { var b: box<i64>\n  b.v = i64(0)\n  var d: dyn Add2 = dyn Add2(b)\n  return i32(d.add2(i64(4))) - 5 }'),
    ('W9 泛型 impl 的实例在泛型函数里进 dyn', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct pair<T> { a: T }\nimpl<T> Tag for pair<T> { fn tag(self: ref pair<T>) -> i64 { return i64(9) } }\nfn mk<T>(v: T) -> i64 { var p: pair<T>\n  p.a = v\n  var d: dyn Tag = dyn Tag(p)\n  return d.tag() }\nfn main() -> i32 { return i32(mk(i64(1))) - 9 }'),
    ('W10 死局部变量的右值是 `?` 解包', 'ok_or_reject',
     'fn f() -> i64? { return i64(1) }\nfn main() -> i32 { let v = f()?\n  return i32(v) - 1 }'),
    ('W11 死局部变量的右值是方法调用链', 'ok_or_reject',
     'use stl::vector\nfn main() -> i32 { var v = vector<i32>::new()\n  v.push(i32(1))\n  let n = v.len()\n  return i32(n) - 1 }'),
    ('W12 相邻两个死局部变量', 'ok_or_reject',
     'fn f() -> i64 { return i64(1) }\nfn main() -> i32 { let a = f()\n  let b = f()\n  return 0 }'),
    ('W13 协程实例存进容器', 'ok_or_reject',
     'use stl::vector\nfn gen<T>(x: T) -> coroutine<T> { yield x }\nfn main() -> i32 { var c = gen(i64(2))\n  var s: i64 = 0\n  while c.next() { s = s + c.value() }\n  return i32(s) - 2 }'),
    ('W14 dyn 值在协程体里使用', 'ok_or_reject',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct pt { a: i64 }\nimpl Tag for pt { fn tag(self: ref pt) -> i64 { return self.a } }\nfn drive() -> coroutine<i64> { var p: pt\n  p.a = i64(7)\n  var d: dyn Tag = dyn Tag(p)\n  yield d.tag() }\nfn main() -> i32 { var c = drive()\n  var s: i64 = 0\n  while c.next() { s = s + c.value() }\n  return i32(s) - 7 }'),
    # 第 41 轮（回答主人「let n = 1 + 2 能不能行」时顺手撞到的）：字面量默认推成 i32，而两个大
    # 字面量的**常量**运算在生成的 C 里溢出 ⇒ gcc 的编译期诊断（-Werror 下直接失败 ✗）：
    # `int32_t n = (2000000000 + 2000000000);` ⇒ "integer overflow in expression of type 'int'"。
    # 运行期的 -fwrapv 救不了常量折叠 ⇒ 判据是「说成功就必须产出合法 C」这一条 oracle。
    ('W15 大整数字面量的常量加法（i32 溢出）', 'ok_or_reject',
     'fn main() -> i32 { let n = 2000000000 + 2000000000\n  println(n)\n  return 0 }'),
    ('W16 大整数字面量的常量乘法', 'ok_or_reject',
     'fn main() -> i32 { let n = 1000000 * 1000000\n  println(n)\n  return 0 }'),
]

# ---------------------------------------------------------------- 事件 / 并发（extern! 路线）
# 事件层只能用 extern!("extc") 触达（没有 stdlib 包装）；codegen 靠**名字前缀**
# （extc_epoll_ / extc_sock_）与'被调用'来置 needEvent ⇒ 这条链本身也是攻击面。
# 覆盖：epoll_new/add/wait、socket 对的写读、坏 fd/负超时/0 字节读、非 0/1 的 readable、
# nonblock 后读空、端到端就绪、以及**跨特性**（协程体里 / 泛型函数里调事件层）。
EVT = [
    ('V1 事件层 epoll_new 拿到一个 fd', 'ok',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 { let ep = extc_epoll_new()\n  if ep < i64(0) { return 1 }\n  return 0 }'),
    ('V2 空 epoll 上的 wait（超时 0）', 'ok',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 { let ep = extc_epoll_new()\n  let r = extc_epoll_wait(ep, i64(0))\n  if r < i64(0) { return 1 }\n  return 0 }'),
    ('V3 socket 对：写一个字节再读回来', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 {\n  var fds: [2]i64 = [0, 0]\n  if extc_sock_pair(fds[..].data) < i64(0) { return 1 }\n  var b: [1]u8 = [u8(65)]\n  let w = extc_sock_write(fds[0], b[..].data, i64(1))\n  if w != i64(1) { return 2 }\n  var got: [1]u8 = [u8(0)]\n  let rd = extc_sock_read(fds[1], got[..].data, i64(1))\n  if rd != i64(1) { return 3 }\n  return i32(got[0]) - 65 }'),
    ('V4 epoll_add 用负 fd', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 { let ep = extc_epoll_new()\n  let r = extc_epoll_add(ep, i64(-1), i64(1))\n  return 0 }'),
    ('V5 wait 用坏的 ep 号', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 { let r = extc_epoll_wait(i64(-1), i64(0))\n  return 0 }'),
    ('V6 sock_read 读 0 字节', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 {\n  var fds: [2]i64 = [0, 0]\n  if extc_sock_pair(fds[..].data) < i64(0) { return 1 }\n  var got: [4]u8 = [u8(0), u8(0), u8(0), u8(0)]\n  let rd = extc_sock_read(fds[0], got[..].data, i64(0))\n  return 0 }'),
    ('V7 epoll_add 的 readable 传 2', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 {\n  var fds: [2]i64 = [0, 0]\n  if extc_sock_pair(fds[..].data) < i64(0) { return 1 }\n  let ep = extc_epoll_new()\n  let r = extc_epoll_add(ep, fds[0], i64(2))\n  return 0 }'),
    ('V8 nonblock 后读空', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 {\n  var fds: [2]i64 = [0, 0]\n  if extc_sock_pair(fds[..].data) < i64(0) { return 1 }\n  extc_sock_nonblock(fds[1])\n  var got: [4]u8 = [u8(0), u8(0), u8(0), u8(0)]\n  let rd = extc_sock_read(fds[1], got[..].data, i64(4))\n  return 0 }'),
    ('V9 epoll 端到端：可读事件就绪', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 {\n  var fds: [2]i64 = [0, 0]\n  if extc_sock_pair(fds[..].data) < i64(0) { return 1 }\n  let ep = extc_epoll_new()\n  if extc_epoll_add(ep, fds[1], i64(1)) < i64(0) { return 2 }\n  var b: [1]u8 = [u8(66)]\n  extc_sock_write(fds[0], b[..].data, i64(1))\n  let r = extc_epoll_wait(ep, i64(1000))\n  if r < i64(0) { return 3 }\n  return 0 }'),
    ('V10 wait 用负超时', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn main() -> i32 { let ep = extc_epoll_new()\n  let r = extc_epoll_wait(ep, i64(-5))\n  return 0 }'),
    ('V11 事件层在协程体里用', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn watcher(n: i64) -> coroutine<i64> {\n  var i: i64 = 0\n  while i < n { let ep = extc_epoll_new()\n    let r = extc_epoll_wait(ep, i64(0))\n    yield r\n    i = i + i64(1) } }\nfn main() -> i32 { var c = watcher(i64(2))\n  while c.next() { let v = c.value()\n    if v < i64(0) { return 1 } }\n  return 0 }'),
    ('V12 事件层在泛型函数里用', 'ok_or_reject',
     'extern!("extc") fn extc_epoll_new() -> i64\nextern!("extc") fn extc_epoll_add(ep: i64, fd: i64, readable: i64) -> i64\nextern!("extc") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64\nextern!("extc") fn extc_sock_pair(out: mut ref i64) -> i64\nextern!("extc") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_write(fd: i64, buf: ref u8, n: i64) -> i64\nextern!("extc") fn extc_sock_nonblock(fd: i64) -> i64\nfn probe<T>(v: T) -> i64 { let ep = extc_epoll_new()\n  let r = extc_epoll_wait(ep, i64(0))\n  return r }\nfn main() -> i32 { let r = probe(i64(1))\n  if r < i64(0) { return 1 }\n  return 0 }'),
]

# ---------------------------------------------------------------- io 边界
# 攻击面：把 `tests/io/` 已覆盖的（EOF、CRLF、超长行、cerr/cout 分流）**避开**，
# 专打相邻形状：整数溢出、部分读、零长/极小缓冲、写失败路径、关流后再写、浮点与布尔的
# 文本往返。每题自带输入文件（先 openOut 写、再 openIn 读），不依赖仓库里的数据文件。
IOG = [
    ('R1 写完再读回一个整数', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r1.txt\')?\n  fs::fout << "123" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r1.txt")?\n  var n: i64 = 0\n  fs::fin >> n\n  fs::closeIn()?\n  if n == i64(123) { return 0 }\n  return 1 }'),
    ('R2 读过大的整数（溢出 i64）', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r2.txt\')?\n  fs::fout << "99999999999999999999999" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r2.txt")?\n  var n: i64 = 0\n  fs::fin >> n\n  fs::closeIn()?\n  return 0 }'),
    ('R3 读负数', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r3.txt\')?\n  fs::fout << "-42" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r3.txt")?\n  var n: i64 = 0\n  fs::fin >> n\n  fs::closeIn()?\n  if n == i64(-42) { return 0 }\n  return 1 }'),
    ('R4 读带正号的整数', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r4.txt\')?\n  fs::fout << "+7" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r4.txt")?\n  var n: i64 = 0\n  fs::fin >> n\n  fs::closeIn()?\n  return 0 }'),
    ('R5 读一个浮点', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r5.txt\')?\n  fs::fout << "2.5" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r5.txt")?\n  var x: f64 = 0.0\n  fs::fin >> x\n  fs::closeIn()?\n  if x > 2.4 && x < 2.6 { return 0 }\n  return 1 }'),
    ('R6 没 openIn 就读（应失败）', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 { var n: i64 = 0\n  fs::fin >> n\n  return 0 }'),
    ('R7 写进不存在的目录（应失败）', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut("build/atk-nodir/x/y.txt")?\n  fs::fout << "x" << endl\n  fs::closeOut()?\n  return 0 }'),
    ('R8 读到 EOF 之后的整数', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r8.txt\')?\n  fs::fout << "1" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r8.txt")?\n  var a: i64 = 0\n  var b: i64 = 0\n  fs::fin >> a >> b\n  fs::closeIn()?\n  return 0 }'),
    ('R9 超过一个整数一次读完（部分读）', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r9.txt\')?\n  fs::fout << "1 2 3" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r9.txt")?\n  var a: i64 = 0\n  var b: i64 = 0\n  fs::fin >> a >> b\n  fs::closeIn()?\n  if a == i64(1) && b == i64(2) { return 0 }\n  return 1 }'),
    ('R10 读进零长缓冲', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r10.txt\')?\n  fs::fout << "abc" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r10.txt")?\n  var buf: mut slice<u8> = new u8[0]\n  fs::fin >> buf\n  fs::closeIn()?\n  return 0 }'),
    ('R11 读进很小的缓冲（截断）', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r11.txt\')?\n  fs::fout << "0123456789012345678901234567890123456789" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r11.txt")?\n  var buf: mut slice<u8> = new u8[4]\n  fs::fin >> buf\n  fs::closeIn()?\n  return 0 }'),
    ('R12 关掉输出流之后再写', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut("build/atk-r12.txt")?\n  fs::fout << "a" << endl\n  fs::closeOut()?\n  fs::fout << "b" << endl\n  return 0 }'),
    ('R13 读回刚写的浮点', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r13.txt\')?\n  fs::fout << "1.5" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r13.txt")?\n  var x: f64 = 0.0\n  fs::fin >> x\n  fs::closeIn()?\n  if x > 1.4 && x < 1.6 { return 0 }\n  return 1 }'),
    ('R14 读一个布尔字面量', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut(\'build/atk-r14.txt\')?\n  fs::fout << "true" << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r14.txt")?\n  var b: bool = false\n  fs::fin >> b\n  fs::closeIn()?\n  return 0 }'),
    ('R15 二进制路径：写 u8 切片再读回', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  var data: mut slice<u8> = new u8[3]\n  data[0] = u8(65)\n  data[1] = u8(66)\n  data[2] = u8(67)\n  fs::openOut("build/atk-r15.txt")?\n  fs::fout << data << endl\n  fs::closeOut()?\n  fs::openIn("build/atk-r15.txt")?\n  var back: mut slice<u8> = new u8[8]\n  fs::fin >> back\n  fs::closeIn()?\n  return 0 }'),
    ('R16 同一个文件开两次输出流', 'ok_or_reject',
     'use std::fs\nfn main() -> i32 {\n  fs::openOut("build/atk-r16.txt")?\n  fs::fout << "one" << endl\n  fs::openOut("build/atk-r16b.txt")?\n  fs::fout << "two" << endl\n  fs::closeOut()?\n  return 0 }'),
]

# ---------------------------------------------------------------- 容器（vector / hashMap / hashSet）
# 攻击面：容量增长与 dense 切片下标、越界（option vs 带位置 trap）、空容器、
# 元素是结构体/泛型实例/@noCopy、容器跨函数传递、hashMap 的覆盖与删除。
CNT = [
    ('Q1 vector 基本 push/len/下标', 'ok',
     'use stl::vector\nfn main() -> i32 { var v = vector<i32>::new()\n  var i: i32 = 0\n  while i < i32(5) { v.push(i)\n    i = i + i32(1) }\n  let dv = v.toSlice()\n  var sum: i64 = 0\n  var k: i64 = 0\n  while k < dv.len { sum = sum + i64(dv[k])\n    k = k + i64(1) }\n  return i32(sum) - 10 }'),
    ('Q2 跨容量边界增长后仍 dense', 'ok',
     'use stl::vector\nfn main() -> i32 { var v = vector<i32>::new()\n  var i: i32 = 0\n  while i < i32(1000) { v.push(i)\n    i = i + i32(1) }\n  let dv = v.toSlice()\n  var k: i64 = 0\n  while k < dv.len { if i64(dv[k]) != k { return 1 }\n    k = k + i64(1) }\n  return i32(v.len()) - 1000 }'),
    ('Q3 get 越界给 null、?? 取默认', 'ok',
     'use stl::vector\nfn main() -> i32 { var v = vector<i32>::new()\n  v.push(i32(7))\n  let a = v.get(i64(0)) ?? i32(-1)\n  let b = v.get(i64(9)) ?? i32(-1)\n  return i32(a) + i32(b) - 6 }'),
    ('Q4 切片下标越界必须带位置 trap', 'trap',
     'use stl::vector\nfn main() -> i32 { var v = vector<i32>::new()\n  v.push(i32(1))\n  let dv = v.toSlice()\n  return i32(dv[i64(5)]) }'),
    ('Q5 空容器：len 0、get 给 null', 'ok',
     'use stl::vector\nfn main() -> i32 { var v = vector<i32>::new()\n  let g = v.get(i64(0)) ?? i32(3)\n  return i32(v.len()) + i32(g) - 3 }'),
    ('Q6 元素是结构体', 'ok_or_reject',
     'use stl::vector\nstruct pt { x: i64  y: i64 }\nfn main() -> i32 { var v = vector<pt>::new()\n  v.push(pt { x: i64(1), y: i64(2) })\n  v.push(pt { x: i64(3), y: i64(4) })\n  let dv = v.toSlice()\n  return i32(dv[i64(0)].x + dv[i64(1)].y) - 5 }'),
    ('Q7 元素是泛型实例', 'ok_or_reject',
     'use stl::vector\nstruct box<T> { v: T }\nfn main() -> i32 { var vec = vector<box<i64>>::new()\n  vec.push(box<i64> { v: i64(5) })\n  let dv = vec.toSlice()\n  return i32(dv[i64(0)].v) - 5 }'),
    ('Q8 元素是 @noCopy（应拒）', 'reject',
     'use stl::vector\n@noCopy struct c { n: i64 }\nfn main() -> i32 { var v = vector<c>::new()\n  var a: c = { n: i64(1) }\n  v.push(a)\n  return i32(a.n) }'),
    ('Q9 容器传进函数', 'ok_or_reject',
     'use stl::vector\nfn total(v: ref vector<i32>) -> i64 { var s: i64 = 0\n  let dv = v.toSlice()\n  var k: i64 = 0\n  while k < dv.len { s = s + i64(dv[k])\n    k = k + i64(1) }\n  return s }\nfn main() -> i32 { var v = vector<i32>::new()\n  v.push(i32(2))\n  v.push(i32(3))\n  return i32(total(ref v)) - 5 }'),
    ('Q10 空容器 pop（option）', 'ok_or_reject',
     'use stl::vector\nfn main() -> i32 { var v = vector<i32>::new()\n  let x = v.pop() ?? i32(0)\n  return i32(x) }'),
    ('Q11 hashMap 基本 put/get/覆盖/删除', 'ok',
     'use stl::hashMap\nfn main() -> i32 { var m = hashMapI64<i32>::withCap(4)\n  m.put(1, i32(11))\n  m.put(2, i32(22))\n  let a = m.get(2) ?? i32(-1)\n  m.put(2, i32(99))\n  let b = m.get(2) ?? i32(-1)\n  let had = m.remove(1)\n  let n = m.len()\n  return i32(a) + i32(b) + i32(n) - 122 }'),
    ('Q12 hashMap 装结构体值', 'ok_or_reject',
     'use stl::hashMap\nstruct pt { x: i64  y: i64 }\nfn main() -> i32 { var m = hashMapI64<pt>::withCap(4)\n  m.put(1, pt { x: i64(3), y: i64(4) })\n  let p = m.get(1) ?? pt { x: i64(0), y: i64(0) }\n  return i32(p.x + p.y) - 7 }'),
    ('Q13 空 hashMap：contains/len', 'ok',
     'use stl::hashMap\nfn main() -> i32 { var m = hashMapI64<i32>::withCap(4)\n  if m.contains(1) { return 1 }\n  return i32(m.len()) }'),
    ('Q14 hashSet 基本操作', 'ok_or_reject',
     'use stl::hashSet\nfn main() -> i32 { var s = hashSetI64::withCap(4)\n  s.put(1)\n  s.put(1)\n  return i32(s.len()) - 1 }'),
    ('Q15 容器 clear 之后再用', 'ok_or_reject',
     'use stl::vector\nfn main() -> i32 { var v = vector<i32>::new()\n  v.push(i32(1))\n  v.clear()\n  v.push(i32(2))\n  let dv = v.toSlice()\n  return i32(dv[i64(0)]) - 2 }'),
    ('Q16 容量与 len 的关系', 'ok_or_reject',
     'use stl::vector\nfn main() -> i32 { var v = vector<i32>::new()\n  v.push(i32(1))\n  if v.capacity() < v.len() { return 1 }\n  return 0 }'),
    ('Q17 @noCopy 存进 hashMap（应拒）', 'reject',
     'use stl::hashMap\n@noCopy struct c { n: i64 }\nfn main() -> i32 { var m = hashMapI64<c>::withCap(4)\n  var a: c = { n: i64(1) }\n  m.put(1, a)\n  return i32(a.n) }'),
]

GROUPS = {'generics': GENERICS, 'coro': CORO, 'modules': MODULES, 'views': VIEWS,
          'dyn': DYN, 'arena': ARENA, 'extern': EXT, 'fs': FSG, 'nocopy': NOC,
          'containers': CNT, 'io': IOG, 'events': EVT, 'deep': DEEP, 'time': TIM,
          'match': MATCH, 'rec': REC, 'inline': INL, 'par': PARG}

def one(name, kind, want, src):
    os.makedirs(WORK, exist_ok=True)
    f = os.path.join(WORK, 'case.extc')
    c = os.path.join(WORK, 'case.c')
    exe = os.path.join(WORK, 'case')
    if isinstance(src, dict):
        # 多文件探针：一个文件 = 一个模块。目录每次清空，免得上一题的文件被 `use` 找到。
        import shutil
        shutil.rmtree(WORK, ignore_errors=True)
        os.makedirs(WORK, exist_ok=True)
        for fn, fs in src.items():
            open(os.path.join(WORK, fn), 'w', encoding='utf-8').write(fs + '\n')
        entry = 'main.extc' if 'main.extc' in src else sorted(src)[0]
        f = os.path.join(WORK, entry)
        src = src[entry]
    else:
        for junk in os.listdir(WORK):
            fp = os.path.join(WORK, junk)
            if fp != f:
                os.remove(fp) if os.path.isfile(fp) else None
        open(f, 'w', encoding='utf-8').write(src + '\n')
    try:
        r = subprocess.run([EXTC, '-w', '--no-line-map', '-o', c, f], capture_output=True, timeout=25)
    except subprocess.TimeoutExpired:
        return ('★挂死★', 'extC 25s 未结束')
    err = r.stderr.decode('utf-8', 'replace')
    if r.returncode < 0 or r.returncode >= 128:
        return ('★崩溃★', 'rc=%d' % r.returncode)
    if r.returncode != 0:
        if kind in ('reject', 'ok_or_reject'): return ('ok', '按期望被拒')
        first = next((l for l in err.splitlines() if 'error' in l), '')
        return ('★期望通过但被拒★', first[:96])
    try:
        g = subprocess.run(['gcc', '-O1', '-g', '-std=c11', '-fwrapv', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-o', exe, c],
                           capture_output=True, timeout=90)
    except subprocess.TimeoutExpired:
        return ('★gcc 挂死★', '')
    if g.returncode != 0:
        return ('★生成的 C 不合法★', (g.stderr.decode('utf-8', 'replace').splitlines() or [''])[0][:96])
    try:
        p = subprocess.run([exe], capture_output=True, timeout=10,
                           env=dict(os.environ, ASAN_OPTIONS='allocator_may_return_null=1'))
    except subprocess.TimeoutExpired:
        return ('ok' if kind == 'ok_or_reject' else '★运行挂死★', '10s')
    err = p.stderr.decode('utf-8', 'replace')
    bad = [l for l in err.splitlines() if 'runtime error' in l or 'ERROR: AddressSanitizer' in l]
    if bad: return ('★UB★', bad[0][:96])
    if 'trap:' in err:
        return ('ok', '按期望 trap') if kind in ('trap', 'ok_or_reject') else ('★意外 trap★', err.strip().splitlines()[-1][:80])
    # 题目的程序大多 `return` 计算结果 ⇒ 退出码非 0 是正常的（早期版本把它当失败，
    # 47 题里报了 29 条假警报）。判据是：没有 sanitizer 报告、没有意外 trap、干净退出。
    if kind in ('ok', 'ok_or_reject'):
        return ('ok', '运行 rc=%d' % p.returncode)
    return ('★期望被拒但通过了★', 'rc=%d' % p.returncode)

def main(argv):
    names = argv or list(GROUPS)
    bad = 0; total = 0
    for name in names:
        cases = GROUPS.get(name)
        if cases is None: print('没有这个组:', name); return 2
        print('== 定向攻击：%s（%d 题）==' % (name, len(cases)))
        for case in cases:
            title, kind = case[0], case[1]
            want = case[2] if len(case) == 4 else ''
            src = case[-1]
            total += 1
            st, detail = one(title, kind, want, src)
            mark = 'ok  ' if st == 'ok' else '★FAIL★'
            if st != 'ok': bad += 1
            print('  %-6s %-40s %-14s %s' % (mark, title, st, detail))
    print('共 %d 题，问题 %d 条' % (total, bad))
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

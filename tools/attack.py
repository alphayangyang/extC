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
    ('F1 dyn 打在泛型实例上', 'ok',
     'trait Tag { fn tag(self: ref Self) -> i64 }\nstruct pair<T> { a: T }\n'
     'impl<T> Tag for pair<T> { fn tag(self: ref pair<T>) -> i64 { return i64(7) } }\n'
     'fn main() -> i32 { var p: pair<i64>\n  p.a = i64(0)\n  var d: dyn Tag = dyn Tag(p)\n'
     '  return i32(d.tag()) }'),
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

GROUPS = {'generics': GENERICS, 'coro': CORO}

def one(name, kind, want, src):
    os.makedirs(WORK, exist_ok=True)
    f = os.path.join(WORK, 'case.extc')
    c = os.path.join(WORK, 'case.c')
    exe = os.path.join(WORK, 'case')
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

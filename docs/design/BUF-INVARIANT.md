# The buffer invariant (`buf<T>`)

> 状态：**当前口径** —— 这篇写的是 `buf<T>` 对用户承诺的语义。改语义先改它。
> 实现：`stdlib/std/data/buf.extc`（策略，全部在 extC 里）+ `src/back/bufrt.c`（三格 C 入口，
> 只做指针算术与 `madvise`，不认识任何策略）。验收：`bench/buf/` 与 `tests/stl/run.sh`。

R4a 把「可增长数组」从池块上搬到一个自己拥有存储的对象上。这篇不是实现说明，是**对外面**
的说明：一个用户只需要记住一条不变量，和四个动作。

## 1 一条不变量，四个动作

```
每个 buf<T> 恰好持有一片地址预留；元素 i 的地址永远是 base + i*sizeof(T)（直到它搬家）；
长度 len <= 容量 cap <= 上限 max；max = 预留字节数 / sizeof(T)。
```

不变量之外没有状态。四个动作是唯一能改变它的方式，每个动作只动其中一两个量：

| 动作 | 地址 | 拷贝 | 页 | 动的是哪个量 |
|---|---|---|---|---|
| `push` / `reserve`（预留内） | 不动 | **不拷** | 第一次写触发提交 | `cap`（`n` 由 `push` 再加） |
| 搬家（撞到 `max`） | **换**一片 | 拷 `len` 个元素 | 旧预留当场还掉 | `base`、`cap=max`、`moves++` |
| `shrink(k)` | 不动 | 不拷 | `[k, cap)` 的页还给 OS | `cap=k`、`n=k` |
| `release()`（也在 `@drop`） | 整片还掉 | — | 全还 | 全部归零 |

`clear()` 是 `shrink(0)` 的轻形式：只把 `n` 归零，预留与页都留着（下一次 `push` 直接复用）。
要还页就写 `shrink(0)`。

## 2 为什么是成员函数，不是自由函数

如果把增长写成自由函数 `mem::grow(slice<T>, n) -> slice<T>`，**存储就没有主人**：调用者手里只有
`(ptr, len)`，谁在什么时候把旧片还回去、谁保证元素地址在两次调用之间不变，都无处安放。
对象形状把这些问题的答案放进不变量：

- 存储属于对象（`@noCopy`，值位置出现是转移，按值传会被拒 `@noCopy: it may not be copied by value`）；
- 容量是对象的状态，不是调用者递进来的参数；
- 视图（`slice<T>`）是**从对象借的**，借的规矩由对象定义（§4 的冻结位）。

所以 API 形状是 `b.push(x)` / `b.reserve(1000)` / `b.shrink(0)`，而不是 `b = grow(b, 1000)`。这也是
R4c 要把 `vector`/`hashMap`/`string` 搬过来的理由：它们的存储语义应该长成同一个对象形状。

## 3 预留是地址空间，不是内存

`buf` 的起点不是 `malloc`，是**一片 2 MiB 对齐的地址预留**：

- 小缓冲区也至少留 2 MiB 地址空间（大页的量子），上限 1 GiB 字节；
- 预留从一张进程级的 buddy 竞技场切（2 MiB 量子、8 个 64 GiB 竞技场懒创建，共 512 GiB 用户地址
  空间的 0.4%），切一块本机 **7 ns**，而一次独立的 `mmap` 是 **1,897 ns**（`tools/probe/vaarena.c`）；
- 没写过的页**不驻留**，所以 `reservedBytes()` 报的是地址空间，不是内存。

这条性质把「增长」和「换一块」分开了：

```extc
use std::io
use std::data::buf

fn main() -> i32 {
    var b: buf<i64> = buf<i64>::withCap(4)
    var i: i64 = 0
    while i < 100000 { b.push(i)  i = i + 1 }
    println("len=", b.len(), " cap=", b.capacity(), " max=", b.maxCapacity(),
            " moves=", b.relocations(), " reservedKiB=", i64(b.reservedBytes()) / 1024,
            " liveKiB=", buf::bufLive() / 1024)
    return 0
}
```

这段程序的实际输出（`taskset -c 6`，本机）：

```
len=100000 cap=138255 max=262144 moves=0 reservedKiB=2048 liveKiB=2048
```

10 万次 `push` **搬 0 次家**：预留最小 2 MiB ⇒ 262,144 个 i64，撞不到上限；`cap` 是预留内推出来的
（几何增长到 138,255，够装下 10 万），`reservedKiB` 与 `liveKiB` 都是 2,048（量子取整后的实际块），
而进程 RSS 只有 **2,624 kB** —— 预留是地址空间，只有写过的页驻留。

## 4 视图：借出、冻结、失效

`view()` / `viewMut()` 返回 `slice<T>`，它看到的是 `[0, len)`。存储会动，所以「视图什么时候还
有效」必须由语言或对象说清 —— `buf` 用两道闸：

**(a) 同块内是编译错误。** 视图活着的时候改存储，编译器直接拒：

<!-- fragment -->
```extc
var b: buf<i64> = buf<i64>::withCap(2)
var i: i64 = 0
while i < 4 { b.push(i)  i = i + 1 }
let v: slice<i64> = b.view()
b.push(99)                 /* error: `b` changes the storage that `v` views */
```

诊断原文（`build/extc`）：

```
error: `b` changes the storage that `v` views
  note: a view is only good until the container moves its storage: copy what you need
        (`snapshot()`) or finish with the view before growing
```

**(b) 跨函数/跨块靠借出守卫。** `borrow()` 返回一个 `borrowGuard<T>`，活着期间这块存储不许搬家；
真要搬就在运行期 `trap`，而不是让旧视图静默读到已经被分配器收回的字节：

<!-- fragment -->
```extc
var b: buf<i64> = buf<i64>::withCap(2)
b.push(1)
var g: borrowGuard<i64> = b.borrow()
pushMany(ref b)            /* 另一个函数里接着 push：一旦需要搬家就 trap: this buffer is frozen by a live borrow */
println("len=", g.len())
```

守卫是按值不可复制的（`@noCopy`），所以「借了没还」不是用户能犯的错：它随作用域结束 `@drop`。
这两条合起来就是 `buf` 对视图的全部承诺：**同一块里靠类型检查，跨块靠运行期检查**。

## 5 归还：旧代不会留在堆上

这是 R4a 反复要的那句话的兑现点。RSS 峰值高的老原因是「换代时旧的一代还活着」：
`std::vector` 是 `malloc` 新块 + 拷 + `free` 旧块，峰值 = 旧代 + 新代；`buf` 搬家时是**拷一段丢一段**：

每拷 2 MiB 就 `madvise(MADV_DONTNEED)` 掉源的那一段（向下取整到页，避免丢掉还没拷的半页），
所以搬家的瞬时峰值是「一份 + 一块」，不是「一份 + 一份」。`bench/buf` 的 S1 里这一条差 3.7 MB。

离开作用域、被覆盖、`return` 提前走时由编译器发 `@drop release`；也可以现在就叫 `release()`。
第二次调用是空操作（`reserved` 已归零），所以「先 `release()` 再离开作用域」不会重复归还。

## 6 可观测的东西（判据都挂在这四个数上）

| 成员 | 含义 | 判据 |
|---|---|---|
| `len()` / `capacity()` / `maxCapacity()` | 不变量三格 | `len <= cap <= max` 恒成立 |
| `relocations()` | 搬过几次家 | 预留内增长必须**不动**它 |
| `reservedBytes()` | 地址空间字节数（量子取整后） | 与 `buf::bufLive()` 的差值是泄漏 |
| `buf::bufLive()` | 进程级预留账本 | 块前块后相等 ⇒ `@drop` 把预留全还了 |
| `borrowGuard` 的 `len()`/`view()` | 借出期间的只读视图 | 借出后增长 ⇒ `trap` |

`reservedBytes()` 报的是**实际切到的块**（2 MiB 量子取整），不是请求字节。第一版报请求字节，
于是一个 512 字节的 `buf` 实际握着 2 MiB 却只报 4096，账本差了两个数量级 —— 泄漏判据当场失效。
现在 `withCap` 与搬家都走 `extc_buf_span` 问分配器要实际大小。

## 7 与回收器的关系（很重要，R4b 之前是边界）

`buf` 的存储是**外来内存**（foreign memory）：它不占 `rc` 槽、不进 region、不参与标记/清扫。
所以一个只用 `buf` 的程序**仍然是 T0**（运行期一行回收代码都不发射），`EXTC_RC_*` 各档对它无效。

这也意味着 R4a 不覆盖「`buf` 里装引用」的形状：元素是 `mut ref T` 或含引用的结构体时，元素里的引用
不会被登记成块级根，块离开作用域时也不会被撤销 —— 这是 R4b 要修的、也是 L10-a/G15 那条池块 pin
泄漏的同族问题。在 R4b 落地之前，**不要把含引用的值放进 `buf`**：没有测试这么做，也没有检查器拦它。

## 8 未做 / 已知边界

1. **含引用元素**：见 §7，R4b。
2. **发射器的 arena 前置挡住内联**：现在每个函数都带 `extc_arena __extc_a[3]` 与两次
   `extc_arena_destroy`，即使它一次都不分配；这让库的热成员必须显式写 `@inline` 才能被内联进调用者
   的循环（不写时 `push` 从 2.6 ns 变 10 ns，同一条循环）。根因在发射器，修法是「不分配的函数据
   不发射前置」，本轮没做 —— 见 `bench/buf/RESULTS.md` §6。
3. **多线程**：竞技场是一把单锁，未测并发形状。
4. **`shrink` 之后旧视图**：页被还掉后重新提交的字节是零，所以地址还是同一个、内容不再是；
   跨块的旧视图由 §4(b) 的守卫兜住（守卫活着时不许搬家，也不许 `shrink` 到视图之外）。
5. **arena 用满时回落**：竞技场（512 GiB）切不出来时回落成独立 `mmap`，`bufLive()` 仍然记账，
   但那一块的「切块价」变成 `mmap` 价。

## 9 位置与测试

- 策略与全部注释：`stdlib/std/data/buf.extc`（约 300 行，含每个成员的语义注释）。
- C 入口：`src/back/bufrt.c` 的发射载荷（`extc_buf_reserve` / `extc_buf_release` /
  `extc_buf_discard` / `extc_buf_span` / `extc_buf_live`），只在程序声明
  `extern!("extc-buf")` 时发射 —— 声明的位置就是触发器，声明搬走了就不会有人为没用过 `buf` 的程序
  付这 10 KB。
- 测试：`tests/stl/buf.extc`（基本）、`buf_new.extc`（`@buffer` 的 `new T[n]` 降级）、`buf_adv.extc`
  （对抗写入 + 账本）、`buf_struct.extc`（非平凡元素）、`buf_frozen.extc`（守卫 `trap`）、
  `buf_churn_{a,b}.extc`（块内/循环内 churn 的账本必须回零）、`buf_fuzz.extc`（3,000 轮随机长度
  + 非 LIFO 释放 + 每步账本）、`buf_huge.extc`（1 GiB 预留只驻留碰过的页）。
- 卡具：`bench/buf/`（五条形状对 C++，`SPEC.md` 判据、`RESULTS.md` 数字与陷阱）。
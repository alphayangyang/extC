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

## 7 与回收器的关系（元素含引用）

`buf` 的存储**不是** `rc` 槽：不占槽、不进 region、不参与标记/清扫，所以只用 `buf` 的程序仍然是
T0（运行期一行回收代码都不发射），`EXTC_RC_*` 各档对它无效 —— 这一条在元素含引用时也不变。

含引用元素（`buf<mut ref T>`、元素是含引用的结构体）时，元素里的每条边仍要被回收器看见：生成 C 在写
元素时发 `extc_rc_storeRefSlot`，而槽落在 `buf` 的预留里 ⇒ 「这条槽装在哪个对象里」证不出来 ⇒ 走运行期的
**外部边表**（键 = 槽地址，内容 = 被指着的那块 + 它所属的区域；一条 24 B）。因此只有**真的**往 `buf` 里写
引用才登记：`buf<i64>` 一条都不进表。

结构动作同时撤销这些边，撤销按**地址区间**一次做完（`extc_buf_releaseRefs` → `extc_rc_pinDropRange`）：

| 动作 | 撤销的区间 | 备注 |
|---|---|---|
| `shrink(k)` | `[k, n)` | 字节还是同一片地址，只是元素不再可达 |
| `clear()` | `[0, n)` | 之后 `n = 0`；容量不变 |
| `release()` | `[0, n)` | 先撤边再还预留（`@drop` 也走这里）|
| 搬家（`reserve` 跨代）| 整段旧代 | 拷一段、撤一段、丢一段源页 |
| `pop()` | **不撤** | 取出的元素由调用方持有；该槽下一次被写时表项自动改正 |

价：`buf<i64>` 的 `shrink(0)` 仍是 `madvise` 价（≈13 ns/KB，64 MiB < 1 ms）；`buf<mut ref node>` 的
`shrink` 额外付一次表扫描。两者都只落在结构动作上，不在 `push`/`pop` 热路径上。

T0 程序编译成空操作：`extc_buf_releaseRefs` 的函数体被 `#if defined(EXTC_RC_TIER)` 包住，没有表就没有
登记、也没有撤销。

判据：`tests/gc-p0` 的 G21（每轮 `withCap(4)` → push 四个引用 → `shrink(2)` → push 一个 → `release`，
50k 与 800k 轮）RSS 3,764 → 3,764 kB（1.000×，判据 ≤1.20×；硬门禁，不是 XFAIL）；生成物里
`extc_buf_releaseRefs` 调用点 3 处（形状确实走到撤销路径）；`tests/reclaim` 的 `t_pinRange`（区间内全撤、
区间外不动、表长正好减槽数、撤销后同槽重新登记不凭空减计数）；`buf_adv`/`buf_fuzz` 的账本判据不变。

## 8 未做 / 已知边界

1. **表项上界是「写过的不同槽地址数」**：同一个槽被反复写只改一条，所以上界不是一个 buf 的元素数，
   而是「峰值同时在用的缓冲区的槽集合」；`pop` 不撤、预留归还竞技场后没人复查的槽都留在这个上界里。
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
  `extc_buf_discard` / `extc_buf_span` / `extc_buf_live` / `extc_buf_releaseRefs`），只在程序声明
  `extern!("extc-buf")` 时发射 —— 声明的位置就是触发器，声明搬走了就不会有人为没用过 `buf` 的程序
  付这 10 KB。
- 引用侧的桥：`extc_buf_releaseRefs` 在 `#if defined(EXTC_RC_TIER)` 里调运行期的
  `extc_rc_pinDropRange`；`buf.extc` 的私有成员 `dropRefs(from, to)` 是四个结构动作唯一的调用点。
- 测试：`tests/stl/buf.extc`（基本）、`buf_new.extc`（`@buffer` 的 `new T[n]` 降级）、`buf_adv.extc`
  （对抗写入 + 账本）、`buf_struct.extc`（非平凡元素）、`buf_frozen.extc`（守卫 `trap`）、
  `buf_churn_{a,b}.extc`（块内/循环内 churn 的账本必须回零）、`buf_fuzz.extc`（3,000 轮随机长度
  + 非 LIFO 释放 + 每步账本）、`buf_huge.extc`（1 GiB 预留只驻留碰过的页）、`buf_par.extc`（`par`
  与 `buf` 同程序 ⇒ 基质必须带锁发射，见 §11.3）；引用侧的两条判据是
  `tests/gc-p0` 的 G21 与 `tests/reclaim` 的 `t_pinRange`。- 卡具：`bench/buf/`（五条形状对 C++，`SPEC.md` 判据、`RESULTS.md` 数字与陷阱）。

## 10 第一个消费者：`vector` 建在 `buf` 上

### 10.1 形状：一个字段，一条不变量

`vector<T>` 现在只有一个私有字段 `b: buf<T>`：长度就是 `b.len()`，元素 `i` 的地址恒为
`base + i*sizeof(T)`。增长因此只有两种形态 —— 预留内推长度（0 拷贝、地址不动），或撞到
`maxCapacity()` 时搬一次家（由 `buf` 做：逐段拷、拷完丢源页，峰值 = 一份 + 一块）。`shrink()`
把尾部页还给系统而地址不变；`release()` 与 `@drop` 把整片预留还回竞技场。

方法的语义全部委托给 `buf`，`vector` 只负责下标形状与下限：`withCap` 把容量下限夹到 4，
`shrink` 把目标容量夹到 `[4, capacity)`，`pop` 在空时给 `option<T>::none` 而不是走进 `buf` 的
`null` 返回。`view()` 仍借出 `[0, len)` 的只读视图，`snapshot()` 仍是拷贝，`from<T>(xs)` 仍是
逐元素 `push`。

### 10.2 所有权：`@noCopy` 让「两份值共用一块板」写不出来

旧 `vector` 是 `@sharesStorage @place`：按值拷贝两份共用同一个池板块，靠池的世代号
（`pid` / `pidGen` / `pStale` / `pPoison`）判陈旧。`buf` 拥有预留且是 `@noCopy`，若 `vector` 仍
标 `@sharesStorage`，`var b = a` 会逐位复制 `buf` 句柄 ⇒ 两个名字各自 `@drop release` 同一片预留
（double free），而判陈旧的凭据（池世代）随池一起退场。因此 `vector` 改成 `@noCopy`：

- `var b = a` 是**转移**。转移之后读 `a` 是编译错（实测报 `a was moved into b and cannot be read
  here`，并连带 `no method push on <error>`），所以「一份 release 之后另一份悬垂」这类事故在语言里
  没有写法。
- 要一份能自己改的副本用 `clone()`：它是深拷贝（新建预留 + 逐元素 push）。`tests/stl/clone.extc`
  的 ④（原件 `release()` 后副本仍可用）现在由 `clone()` 保持。
- 旧的世代/冻结三件套（`pid`、`pidGen`、`pStale`、`pPoison`、`frozen`）整个消失；`borrow()` 仍给出
  `borrowGuard`，`vectorGuard` 只是把它转发一层（`view()` / `len()`）。

### 10.3 顺带修掉的两个编译器缺陷

**（a）泛型实例的字段类型。** 一个 `StructDef` 被所有实例共享，所以 `vector<i32>` 的字段在定义侧
仍写着 `buf<T>`。codegen 的 `typeNeedsDrop` / `genDropValue` 与 checker 的 `typeHasDrop` /
`markDropHooksUsed` 若照字面递归，问的是模板、发射的是模板钩子：生成的 C 调
`buf$buf_T_release`，而实际定义叫 `buf$buf_i32_release` ⇒ `implicit declaration of function`。
四处统一成「先按实例的实参替换字段类型再往下走」（checker 侧的新助手 `fieldTypeInInstance`），
两侧的分析与发射重新一致。

**（b）effects 摘要与名字分派内建。** `buf::withCap` 调 `extc_sizeOf<T>()`、`extc_viewOfT<T>()`，
`buf::push` 调 `sysheap::extc_viewOf`：这些都是 checker/codegen 按名字分派的内建，没有声明可读，
旧的效应收集器直接用「回调名的整串」与 `print`/`println`/`flush` 比 `strcmp`。经模块限定调用的
名字是 `mod$name`（实测 `heap$extc_viewOf`、`io$print`），整串永远匹配不上 ⇒ 摘要被标成不完整 ⇒
**所有**调用者都要证明每个引用实参活得比被调方的位置更久，于是 `vector::from(a[..])` 被拒
（`tests/stl/from.extc`）。修法是把白名单判断改成「比最后一个 `$` 之后的那一段」，并把
`trap`/`extc_sizeOf`/`extc_viewOf`/`extc_viewOfT` 一并列入 —— 它们要么不返回、要么只重解释调用方
已经持有的指针，与 `extc_viewOf` 自己的 `effects Addr=0 Cont=0` 是同一个承诺。

### 10.4 判据

| 判据 | 结果 |
|---|---|
| `tests/stl/run.sh` | 56 项 0 失败（含 ASan 组） |
| `tests/stl/vector.extc` 输出 | `cap0=4 n=9 cap=9 sum=36 shrink=9 pop=8 n=8 clear=0/9 tail=-9`（与迁移前逐字相同） |
| `tests/stl/vector_grow.extc` | `caps=4,6,9,…,711,1066 n=1000`（1.5 倍、1000 次 push 搬 14 次） |
| vector churn（1e5 / 1e6 轮） | 1,664 kB / 1,664 kB ⇒ 平 |
| 峰值 RSS（同形状 vs arena 切片） | 9,964 kB vs 18,284 kB（低 45%） |
| `tests/stl/from.extc` | 由「effects 不完整」的编译错变为通过，输出 `s=5 e=0 v=3 w=2 sum=6` |

### 10.5 未做

- `string` / `hashMap` / `map` / `place` 仍在池板块上（它们的迁移是 R4c 的后续步），`stdlib` 的池
  与运行期池因此还不能删。
- `vector` 的 `@noCopy` 让「按值拷贝共享」这一族测试失效：`tests/pool/rt_stale_alias.extc` 已改成
  编译错负例，`tests/pool/rt_clone_alias.extc` 是 `clone()` 的正例；池的其余用例等 R4c 后续步。

### 10.6 视图元素不能装「拥有预留的值」

容器把元素按字节写进自己的平面数组，容器死亡时**不会**逐个跑元素的 `@drop`。所以一个拥有预留
的值（`buf`、以及建在它上面的 `vector`）一旦被写进视图元素，那份预留就再没有人还：

- 临时量：`outer.push(vector<i64>::new())` 的参数按值传入，函数尾发射参数的 `@drop release`，
  而元素已经被拷进容器 —— 元素指向**已释放**的预留。迁移前实测：`buf::bufLive()` 在 `push` 前
  2,097,152 字节、`push` 后 0，而 `outer.get(0).len()` 仍读到 1。
- 具名变量：先被 `@noCopy` 拒（`may not be copied by value`），这条更早、更清楚。

因此语言加了一条规则：**视图元素不能是拥有型值**。两条路径：具体类型在 `checkStoreEscape` 当场
报（`cannot store T into a view element: it owns a reservation and the copy would never be dropped`），
模板里的 `T` 延迟到实例化时报（复用 `RefCheck`，诊断落在元素写入那一行并带上实例名）。附带修
了同一处的可见性漏洞：视图跟踪的旧门是「容器自己声明了 `@drop` 钩子或 `poolObject`」，而新的
`vector` 自己不声明钩子（存储靠字段 `buf` 归还）⇒ 7 条「视图增长后仍能用」的负例被漏判；门
改成传递性的 `typeHasDrop`。

这条规则对用户代码同样成立：`mut slice<vector<i64>>` 的元素赋值也是同一个形状。仓库里仍有装引用
的容器（`buf<mut ref node>`、`vector<mut ref node>`），引用不拥有存储，不受影响。

### 10.7 协程调度器跟着改

`stdlib/std/coro/scheduler.extc` 里 28 处 `self.rows.vals[i]` 依赖旧 `vector` 的**公开** `vals` 字段。
新 `vector` 的存储藏在 `buf` 里，唯一可行的写法是先绑局部视图再按下标读写：`var rs = self.rows.viewMut()`，
然后 `rs[i].h.next()`。语言侧的两条硬约束（实测，别再试）：

- `v.viewMut()[i].field = x` 不行 —— 临时视图不是「地方」（报 `it is not a place`）。
- 给容器加 `fn at(self: mut ref buf<T>, i: i64) -> mut ref T` 也不行 —— 只有本帧 `new T[n]` 来的视图
  允许对元素取引用（报 `cannot take a reference to an element of this view`），容器背后的视图可能被换掉。

所以调度器的每个方法在自己的作用域里取一次视图，视图存活期间不做结构性动作（不 `push` / `reserve`），
需要 `finish` 时只改字段。`tests/coro/coro_table.extc` 与 `coro_sched.extc` 同步改写。

### 10.8 生成 C 的可见变化

两类，都被 `tools/emitted_identity.py` 逐个文件看过：

1. **真实行为变化**：用到 `vector` 的 22 个程序（两种回收配置各一行，共 44 行）生成 C 变了 ——
   `vector` 的存储从池板块换成 `buf` 预留，钩子从池的 `@drop` 换成 `buf::release`。
2. **声明顺序的置换**：`hashMap`/`string`/`map` 这类**没有**用到 `vector` 的程序里，类型声明区有
   若干行换了位置（`pool$pool_u8` 之类）。原因是 checker 侧的字段替换会内化（intern）实例类型，
   让它的登记时机早于 codegen 原本的时机；逐行排序后两个文件完全相同（对 `hashSet` 与 `stringOps`
   实测为纯置换），语义无变化。重冻结基线时按「置换，已验证」记录。
## 11 基质本身的账：回收量子、span 快路、要不要带锁

`buf` 的第一版在**小容器 churn** 上输给了它取代的池。`bench/containers` 的 v4 是 20 万个小容器
（`withCap(8)`、各 8 个 `i64`）的生死循环：池版本 4 ms，`buf` 版本 13 ms —— 3.25×。这条形状是
故意的反向选择（池的免费表在这种尺寸上本来就快），但删池之前必须把它平掉，否则「池提供的廉价
共享与复用」会被悄悄换成每容器一次 15 层 buddy 记账。三处结构性修正把它拉到 3 ms：

| 台阶 | v4 墙钟（20 万容器） | 改了什么 |
|---|---|---|
| 第一版 | 13 ms | 每次 `reserve` 切 2 MiB 量子、每次 `release` 合并回去 |
| +回收量子 | 6 ms | `EXTC_BUF_MAG_KEEP 32`：每竞技场留 32 个 order-0 块的免费链 |
| +span 快路 | 5 ms | 记住最近一次 `carve` 的 `(地址, 大小)`，`span` 不再扫竞技场 |
| +锁按需 | 3 ms | `EXTC_BUF_MT 0`：单线程程序里预留/归还不发原子操作 |

### 11.1 回收量子必须在合并之前

`extc_buf_give` 把块交回去时要和同尺寸的伙伴合并。合并是 15 层循环，而 4 KiB 的容器只用一个
order-0 块 —— 合并完再进免费链，下次 `carve` 拿到的已经是更高阶的块，还得再切下来。所以回收
判断要放在合并**之前**：`if (k == 0 && extc_buf_magN < EXTC_BUF_MAG_KEEP)` 就把块挂进 `mag0`
（链走 `nextQ`）并按 2 MiB 记 `liveBytes`，直接返回。第一版把这段写在合并之后，热路径永远命中
不了，热路径永远命中不到，与没写这段代码等价 —— 记在这里，因为它是「看代码像对的、量起来是零」的典型。

缓存只占地址空间（32 × 2 MiB），不占驻留：块里的页本来就是还给系统的。

### 11.2 span 快路与它的闸

`extc_buf_span(at)` 回答「这块 `at` 占了多大」，`release` 要用它记账。原实现加锁扫竞技场的
`ordQ`，在 4 KiB 的容器上是每容器一次的固定开销。快路是记下最近一次 `carve` 的 `(地址, 字节数)`，
命中就直接返回。

**它只能出现在单线程档**：`lastAt`/`lastSpan` 是裸变量，多线程下无锁读写是 UB。所以快路的声明、
三处写、一处读共五个位置全部包在 `#if !EXTC_BUF_MT` 里（`tools/check_buf_mt.py` 数的就是这 5 个
`#if` 与那行读，改少一处门禁就红）。

### 11.3 锁只有能起线程的程序才付

竞技场是进程级结构，`reserve`/`release` 要保护它 —— 但只在**程序真的能起线程**时才需要。`par`
是 extC 程序起线程的唯一途径（`pthread_create` 只出现在 `needPar` 点亮的那段线程池里），而发射器
在所有函数体发射完之后才知道 `needPar`，所以 `bufRtEmitRuntime(arena, out, g->needPar ? 1 : 0)`
在文首写一行 `#define EXTC_BUF_MT 0/1`，载荷里锁的变量与两个函数体都在 `#if EXTC_BUF_MT` 内。
代价是每容器两次原子交换；单线程程序不再付它，`--selftest` 的「MT 恒写 1」变异会被门禁抓到。

当前语料里 `par` 循环体不能调函数（编译期就报 `cannot call a function inside a par loop`），所以
worker 线程今天碰不到竞技场；按 `needPar` 发射是保守选择 —— 规则放宽时这里不需要再想一遍。
`tests/stl/buf_par.extc` 就是这条的判据：同一程序里有 `par` 与 `buf`，输出一行
`len=100 sum=65280 live=2097152`。

### 11.4 数字（`bench/containers`，REPS=11，min，`taskset -c 6`，三臂输出逐字一致）

| 形状 | 池版本 old | `buf` 版本 new | C++ | new / old |
|---|---|---|---|---|
| v1 8e6 个 `i64` 增长 | 34 ms / 64,256 kB | 22 ms / 65,456 kB | 64 ms / 69,232 kB | 0.65× |
| v2 2e6 后 5 次 clone | 28 ms / 32,852 kB | 14 ms / 34,732 kB | 36 ms / 35,048 kB | 0.50× |
| v3 clear 后复用容量 | 54 ms / 64,256 kB | 28 ms / 65,456 kB | 76 ms / 69,348 kB | 0.52× |
| v4 20 万小容器 churn | 4 ms / 1,868 kB | 3 ms / 1,868 kB | 7 ms / 4,152 kB | 0.75× |

判据（`bench/containers/SPEC.md`）：新臂墙钟不劣于旧臂 10%，RSS 不劣于旧臂 +ε。四格现在都满足；
RSS 的最大偏差是 v2 的 +5.7%：克隆出的每个副本各自拥有预留，而池版本的克隆与原件共享一块板块。

### 11.5 判据与门禁

- `tools/check_buf_mt.py`：编译两段内嵌探针（单线程、`par`+`buf`），对**生成 C**判 6 条结构判据，
  并用 6 个变异做红自测（MT 恒 0 / 恒 1、删回收链、`carve` 不看缓存、删单线程闸、锁换成 `while (0)`）。
  注册为 `gate-buf-mt`。
- `tests/stl/buf_par.extc`：`par` 与 `buf` 同程序的输出判据。
- 行为侧仍然由 `tests/stl` 的 buf 系与 `bench/buf` 的五条形状顶着（v4 那 3 ms 门禁不判墙钟 ——
  结构对了就便宜，结构错了就该被 `gate-buf-mt` 抓住，墙钟只记在这里）。

## 12 元素所有权：容器里的值自己拥有存储

§7 说的是"元素里装的是**引用**"（`buf<mut ref node>`：存储不归元素，归别处）。这一节是另一半：
元素**自己拥有一块预留**（`buf<vector<i64>>`）。两件事必须同时成立，容器才允许持有这种元素：

1. **写进去的东西必须把所有权交出来**。元素存储是视图背后的平面数组，写入只搬字节、不为那一份
   拷贝跑 `@drop`；如果源名字之后还照常跑钩子，同一块预留就被还两次。
2. **容器自己必须跑每个元素的钩子**。`clear` / `shrink` / `release` 逐元素跑 `@drop`，否则元素
   的预留永远没人还。

### 12.1 写入即移动（五个值位置里的最后一个）

`self.vals[self.n] = v` 在 `push` 的形参是**普通名字** ⇒ 这是一次移动：源头标死，块尾与形参收尾
都不再跑钩子。临时量（`b.push(vector<i64>::new())`）没有第二个名字要还，容器就是第一个所有者。
语言面没有新关键字：`@noCopy` 的移动规则本来就写着"值位置"，视图元素写入只是补上了最后
一个值位置（`docs/design/TYPE-MODEL.md` §七 的表）。

**拒绝的形状**：源是字段或另一个视图的元素（`out.vals[i] = self.vals[j]`）。拿走一半所有权会留下
一个半死的聚合体，所以只要那个类型拥有存储就是编译错误。

### 12.2 两个库内部原语：`elemDrop` / `elemTake`

`@drop` 是编译期知道类型的钩子调用，库代码没法"对一个视图的每个元素"写出来。所以有两个只由标准库
使用的原语，`buf.extc` 是唯一的调用方：

| 原语 | 返回 | 发射成 |
|---|---|---|
| `elemDrop<T>(v, from, to)` | `int64_t` | 语句前缀里的 `for` 循环：对每个槽跑 `T` 的钩子，然后把槽 `memset` 成零 |
| `elemTake<T>(v, i)` | `T` | 取值到临时、把槽 `memset` 成零、返回临时 |

三条不变量写在这里，改这两个原语时必须一起看：

- **钩子跑在语句之前**（发射进语句前缀）。`buf::pop` 是"先降 `n`、再撤这一格的引用边、再把值移出"，
  三个动作的相对顺序由调用方写在源码里，前缀保证钩子看到的还是容器认账的那个槽。
- **移出即清零**。`elemTake` 之后那个槽是全零：容器之后 `clear` 时不会为一个它已经不拥有的槽再跑
  一次钩子。零值上的钩子是空操作 —— `buf::release` 已经按 `reserved > 0` 短路，这一条是它的推论。
- **没有钩子的元素类型零成本**。`elemDrop` 在元素类型不需要 `@drop` 时发空语句（`(void)0`），所以
  `buf<u8>::clear` 仍然是 O(1)，不会为了"逐元素"走一遍 8 MiB。

`pop` 的顺序是 `n = n - 1` → `dropRefs(n, n + 1)` → `elemTake`：引用边按**槽**登记（§7），撤边必须
在槽被清零之前完成。

### 12.3 枚举载荷也会被 drop

`pop` 返回 `?T`，所以 `let top = b.pop()!` 拿到的值走过一层 option。`?T` 是枚举，`some` 的载荷是第 0 个
字段：`typeNeedsDrop` 与 `genDropValue` 原来只认结构体字段，枚举载荷一条都不跑 ⇒ 取出来的容器永远
不还。现在两者都按变体走：`switch (v.tag)`，每个有载荷的变体 `case <类型>_<变体>:`，载荷路径是
`v.u.<变体>._<k>`。

### 12.4 `clone` 只对可复制的元素成立

`clone` 逐槽写成视图元素赋值（`out.vals[i] = self.vals[i]`），而不是 `out.push(self.vals[i])`：后者是
一次**移动**，会把元素从 `self` 里搬走。写成赋值之后，模板在实例化时看到"源是另一个视图的元素" ⇒
元素拥有预留时 `clone` 编译期被拒（`a view element cannot hold ...`），而不是拷出一份共享所有权的值。

### 12.5 判据

- `tests/stl/buf_own.extc`：`len=3 popped=1 n=2 poplive=6291456 clearlive=2097152 live=0` —— 依次判
  移动进入（只有一份预留）、`pop` 移出（槽清零、之后的 `clear` 不多还）、`clear`/`shrink` 逐元素
  `@drop`（账本回落到容器自己那一份）、最后归零。
- `tests/stl/buf_own_clone.extc`：`clone` 拥有预留的元素必须编译期被拒（`rejects`）。
- `tests/pool/rt_nest_promote.extc`：**循环里建内层、推入外层**，从旧机制的反例改成运行期正例
  （`after=0 innerlen=2 v=20 bytes=0`）：内层内容完整、池一个字节都不占，元素逐个还清。

## 13 第二个制度：密排槽（一个量子切成 13 个尺寸类）

§3 说预留以 2 MiB 量子为最小单位。对"会增长的大载荷"这是对的，对"几十字节的小值"是灾难：
20 万个 32 字节的载荷各占一个量子 = 442 MB 地址空间。这就是"把小 `string` 直接搬上 `buf`"的粒度冲突 ——
负载小到几十字节时，2 MiB 的最小单位本身就是错的量级。

于是同一个 buddy 竞技场上有两个制度：

| 制度 | 单位 | 增长 | 现状 |
|---|---|---|---|
| 预留 range | 2 MiB 量子（按 2 的幂取整） | 预留内零拷贝，撞上限搬一次家 | 全部容器在用 |
| 密排槽 slot | 16 B…64 KiB，13 个尺寸类（类 `lg` 的槽 = `16 << lg` 字节） | 类内不搬，跨类搬家 | 底盘已就绪，容器还没切 |

结构：每个 2 MiB 量子首 64 字节是 `ExtcBufSml {cls, live, free, next}`，槽地址 = `quantum + 64 + idx * cls`；
同一尺寸类的量子挂在 `smlHead[lg]` 链上，空闲槽的链穿在槽首 4 字节。一个量子只有一类；`live` 记用了几个槽、
`free` 记空了几个；**整量子空了才还给 buddy**（`extc_buf_smlRetire` 先从类链摘掉再 `give`，合并照常发生）。
记账不变：量子被 carve 时 `liveBytes += 2 MiB`，退还时减回 —— `extc_buf_live()` 仍然是"活缓冲区占的地址空间"。

归还入口只有一个：`extc_buf_release(at, bytes)` 先问 `extc_buf_smlClsAt(at)`（槽起点返回类字节，否则 0），
命中走 `extc_buf_slotRelease`，否则走 buddy。`extc_buf_discard` 对槽是空操作（返回 0）：槽与邻居共页，
`MADV_DONTNEED` 会把活槽一起抹掉。

### 13.1 修掉的一个真缺陷

`extc_buf_slotReserve` 第一版只在"已经存在的 arena"上找类链，于是 `extc_buf_nArena == 0`（一次都没切过块的进程）
恒返回 NULL，所有小分配失败。现在取锁之后先 `if (extc_buf_nArena == 0) (void)extc_buf_arenaNew();` ——
carve 本来就会自己建 arena，缺的是"类链必须有一个可走的 arena"。

### 13.2 判据

`tests/bufrt/`（新目录）：`emit_main.c` 走仓库发射口 `bufRtEmitRuntime` 导出载荷，`driver.c` 27 条判据，
对 `EXTC_BUF_MT` 0 / 1 两个发射档各跑一遍：

- 密排：20 万个 32 B 槽的地址空间 ≤ 16 MiB、`span` == 32、内容互不覆盖、全部归还后账回零。
- 退休：同类槽步长 32、一个量子记 2 MiB、退还单个槽不牵连同量子的其他槽、量子空了退回 buddy。
- discard：槽上调用返回 0，且三个槽的字节一个都不丢。
- 混合：槽与 4 MiB 预留共存，账 = 6 MiB，`span(big)` == 4 MiB，首尾字节可写。
- 拒绝：131072 返回 NULL、65536 命中最后一类、预留起点不许当槽还。
- 周转：10 万次 40 字节周转后账回原值。

实测：`通过 27，失败 0` × 2 档；C 级探针 20 万个 32 B 槽只占 8 MiB 地址空间（4 个量子），全部归还后 `live` 回 0。

## 14 从元素里"取出来"是拷贝，不是移动

§12.1 说**写进**视图元素是移动。反方向不成立：从元素或字段里取出一个拥有存储的值**不是**移动，
那个槽仍然持有它。所以这两行是编译错误，诊断与带注解的 `let d: buf<u8> = s[0]` 完全一样：

```
let d = s[0]        /* s 是视图：元素仍在容器里 */
let d = box.b       /* 字段仍在 box 里 */
```

本轮之前无注解的绑定走的是"按值检查"，那条路从不看拷贝 ⇒ 静默拷出一份共享存储，两个析构器各还一次，
运行期 `trap: buf: cannot give the reservation back`（复现：一个元素的 `vector<buf<u8>>`，`let e0 = s[0]`）。
现在无注解绑定先试"名字 ⇒ 转移"，不是名字（字段、元素）就交给 `@noCopy` 的拷贝拒绝。

**就地读仍然是合法的**：`println(s[0].len())` 是对元素位置的只读方法调用，不产生第二份所有权，
判据 `tests/stl/buf_elem.extc`（`n=1 len0=3 live=4194304`，结束账正好是容器与元素各一份预留）。

**已知边界**：`for d in v.view()` 在元素拥有存储时现在是编译错误 —— 按值迭代要把元素拷出来。
要让它工作需要"借出视图元素"的语义（`ref` 到视图元素目前被拒），那是语言面的事，留给迭代语义那一轮；
今天写 `for i in 0..v.len() { s[i].len() }`（`s` 先绑定一次视图）就地读。
- `tests/nocopy/move_arg.extc`（按值实参 = 移动）与 `errors/arg_twice.extc`（同一个名字移动两次）。
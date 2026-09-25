# `SOUNDNESS.md` —— 「不会静默 UB」这句话，在代码里对应什么

> **一句话**：把"不会静默 UB"拆成 **7 条义务**，每条**钉到 extC 的一个具体地方**。
> 今天 **4 条成立、3 条缺**（都缺在池/dyn 那一档），另有一条（编译器正确性）是**持续义务**。
>
> 姊妹篇：[`ARENA-SOUNDNESS.md`](ARENA-SOUNDNESS.md)（arena 档的证伪，7 个反例）·
> [`POOL-SOUNDNESS.md`](POOL-SOUNDNESS.md)（池档的证伪，E1 实测 UAF）。
>
> 基准版本：`f9cd8bc`。

---

## 0. 先把"不会静默 UB"说成大白话

> **程序要么全程有定义，要么在失败的那一刻 trap。**
> trap 不算 UB —— 它是语言规定的行为（带 `.extc` 位置）。

它**不是**这三件事（这三件是另外的定理）：

- 不是"程序一定正确"（程序可以算错，但不会踩坏内存）；
- 不是"不会内存泄漏"（忘记 `release` 是泄漏，不是 UB）；
- 不是"性能有保证"。

---

## 1. 七条义务 ↔ 代码落点（这张表就是全文）

| # | 义务（白话） | extC 里对应的东西 | 代码落点 | 今天 |
|---|---|---|---|---|
| **O1** | **每一种"产生引用"的操作，都得有一个活性理由** | `ref x`（`EX_REF`）· `a[i..j]`（`EX_SLICE`）· `new`（`EX_NEW`）· 库里的 `toSlice`/`sub` · `extern!` 的返回 | 表达式种类见 `src/ast.h:88-110`；分配见 `src/codegen.c:2059/2075/2163`（`extc_arena_alloc`） | ⚠️ 语言自带的三样有检查；**库 API 的"零拷贝视图"没有语言规则**（E1 就出在这里） |
| **O2** | **借用的静态记账必须是真实寿命的上界** | 深度记账 `d(v) ≤ at(p)`；存借规则 | `src/check_escape.c:365`（取上界）、`:967`（`arenaDepthOf`）、`:791`（拒绝记录）、`:605/:1239`（`recordRefCheck`）；重放见 `src/check_internal.h:279`、`RefCheck` 结构 `:376` | ✅ 规则在；**但记账不是上界的地方有 7 个反例**（`ARENA-SOUNDNESS.md`） |
| **O3** | **每一种"让引用失效"的操作，都必须让检查看到的版本变化（同粒度）** | 换代：`generation++`（**全文只有 1 处**，在 `new_at`）；`freeSelf` 只置 `live = 0`；`reset` **既不换代也不置 0** | `src/pools.c` 的 `extc_pool_new_at` / `extc_pool_freeSelf` / `extc_pool_reset` | ❌ **缺元素粒度**（`reset` 那一行） |
| **O4** | **句柄寻址的存储只追加（或删除用墓碑）** | 今天不存在：容器不暴露句柄，只靠 `pStale` | —— | ❌ **缺**（dyn 必须定这条） |
| **O5** | **派发键必须决定布局（类型身份）** | 容器身份 = `pid`/`pidGen`；类型身份 = 类型表的 intern | `src/types.c`（`ttGeneric`/`ttEquals`）；`stdlib/stl/{map,hashMap,vector}.extc` 的 `pStale` | ❌ **缺**（dyn 的派发键还没有；E3 类型混淆就在这里） |
| **O6** | **动态失败全部检查，并映射到 trap** | 越界 · `copyInto` 计数 · 除零 · 溢出除法 · 浮点→整数 · 池 OOM · `step` 返回 `none` | `src/codegen.c:609`（越界）、`:633`（计数）、`:5379`（除零）、`:5514`（f2i）、`:5348/:5356/:5169`（`extc_trap`/`extc_trapMsg`/`extc_die`，共 17 处 `trapMsg`） | ✅ 成立（本会话实测过越界 trap、f2i trap） |
| **O7** | **可信断言：谁用谁签字（不可证）** | `extern!` + `effects Addr=0 Cont=0`；`!` | `src/parser.c:318/382`（解析）；`src/check_top.c:1124/1140`（按 effects 决定保守程度）、`:1082`（effects-closed 分析）；`docs/MANUAL.md:828`（`!` = "错了算我的…真没有值就是 UB"） | ✅ 机制在；**按定义不可证**（C 说谎就没了） |
| — | **编译器正确性**（另一条定理） | 检查器 + 代码生成 | 全体；反例库 `tests/arena-soundness/`、`tests/pool-soundness/` | ⚠️ 进行中（ARENA 7 个 + POOL 1 个实测） |

**读法**：4 条 ✅（O1 的语言自带部分、O2 的规则、O6、O7）· 3 条 ❌（O3/O4/O5，**全在池–dyn 那一档**）·
1 条 ⚠️（O1 的库 API 部分、以及编译器正确性）。

---

## 2. 今天缺的三条：位置、反例、修法

### O3 · 失效覆盖（元素粒度换代）

- **位置**：`src/pools.c` 的 `extc_pool_reset`（保留池、把槽标记可复用，**不换代**）。
- **反例**：E3（`POOL-SOUNDNESS.md` §5）—— `reset` 之后槽被复用，旧句柄通过校验 ⇒ 指向另一个对象 ⇒
  dyn 落地时就是**类型混淆**。
- **修法**：`extc_pool_reset` 里加 `generation++`（一行）。语义变成"重置 = 该池所有引用作废"。

### O4 · 只追加 / 墓碑

- **位置**：不存在。今天 `extc_pool_give` = `free`、`extc_pool_resize` = `realloc`，**都不动 `gen`**。
- **反例**：E1（**实测 ASan UAF**）、E2（结构性）。
- **修法**：dyn 用的池定成**只追加**（永不 `give`/`resize`）；删除 = 墓碑（对象在、标记已删 ⇒ 取用时 trap）。

### O5 · 类型身份

- **位置**：容器层有 `pid`/`pidGen`（`pStale`），类型层只有 `types.c` 的 intern；**没有 dyn 的派发键**。
- **反例**：E3。
- **修法**：让**池即类型**（池描述符携带 vtable）或让 **tag 与版本一起校验**；vtable 的身份用**稳定键**
  （mangled `trait$类型`），不用插入顺序。

---

## 3. 白话词典（抽象词 → extC 的话）

| 抽象词 | 在 extC 里就是 |
|---|---|
| 状态 σ | 运行期那几张表：`extc_zones[]`（地方栈）· `extc_pools[]`（池表：`live`/`generation`/`blocks`/`zone`）· 池的 `blocks` 链 · arena 的块链 |
| 良构 WF(σ) | "活着的引用都指着活内存，且在界内" = arena 的深度上界 **∧** 池的三条不变量（O3/O4/O5） |
| 静态纪律 D | `check_escape.c` 判的那条 `d(v) ≤ at(p)`（加上 `RefCheck` 的实例重放） |
| 运行期检查 C | 生成的 `if (i < 0 \|\| i >= v.len) extc_trap(...)`、`extc_pool_generation()`、容器里的 `pStale()` |
| 可信断言 T | `extern!` 的 `effects`、`!` |
| 语义 ⟦·⟧ | 生成的 C 在 `gcc -O2 -std=c11 -fwrapv` 下的行为 |
| progress + preservation | "每一步要么保持安全、要么 trap" —— 不需要类型论，就是这句话 |
| 定理 | "**检查器接受的程序**，跑起来要么有定义、要么 trap" |
| TCB | 那三样签过字的东西：FFI 声明、`!`、`dlclose` 契约 |

---

## 4. 定理的完整形状（想写论文/文档时引用这一节）

```
设 D 为可判定的检查纪律，C 为运行期检查，T 为可信断言集合，⟦·⟧ 为语义（trap 是有定义的结果）。
若 (i)  每个产生引用的操作都由一个活性事实正当化（静态的或检查过的）        —— O1
   (ii) 静态记账是真实寿命的上界（且只往保守方向改写）                     —— O2
   (iii) 每个失效事件都在检查所用的粒度上改变版本                          —— O3
   (iv) 句柄寻址的存储在句柄活着时不被回收/搬家（或删除留墓碑）            —— O4
   (v)  派发键决定布局                                                    —— O5
   (vi) 所有动态失败条件都被检查并映射到 trap                             —— O6
则 ∀P: D ⊢ P ⇒ ∀σ₀ ⊨ WF: P 的每一步要么保持 WF，要么走 trap 转移。        —— 定理
前提：T 中的断言为真（O7）—— 这是 TCB，不可证。
```

**两个不可能（都不是实现问题）**：
1. **sound ∧ complete 不可能**（莱斯）⇒ 必然保守（误报），代价付在表达力上；
2. **未声明的 FFI 不可验证**（任意 C 的行为不可判定）⇒ 必须进 TCB。

---

## 5. 怎么用这份文档

- 每修一个洞，划掉表里的一行；反例库跟着翻面（`HOLE` → `编译错误/trap`）。
- 反例库：`tests/arena-soundness/`（7 个，`ARENA-SOUNDNESS.md`）· `tests/pool-soundness/`（E1 实测 + C1 对照）。
- 判据**双向**：正例不许误杀，反例不许静默。

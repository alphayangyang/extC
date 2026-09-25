# `POOL-SOUNDNESS.md` —— 池档健全性的**证伪**（以代码为基准，2026-09-25）

> 结论一句话：**池的"存储"是健全的（块、`live`、`malloc`/`free` 都对），
> 不健全的是"什么引用可以指向池里的什么"，以及"失效时到底换不换代"。**
> 今天 extC 只有**池粒度**的陈旧检查（`live` + `generation`），而 dyn 需要**元素/块粒度**的；
> 两条粒度之间的缝里，本文件给出 3 个反例（1 个 **实测 ASan UAF**、1 个结构性、1 个设计期）。
>
> 基准版本：`ee5b0e4`（`src/` 干净；池运行期在 `src/pools.c` 里是**一段 C 字符串**，
> 所以本文引 `extc_pool_*` 的函数名而不引行号）
>
> 姊妹篇：[`ARENA-SOUNDNESS.md`](ARENA-SOUNDNESS.md)（arena 档：定理对、静态记账漏）。
> 两篇合起来才是完整的图：**arena 档漏在"深度记账"，池档漏在"失效粒度"。**
>
> 📁 反例库（建议按 arena 的样子建）：`tests/pool-soundness/`（本文 §8 给出可复现命令）

---

## 0. 判定（先说结论）

| # | 命题 | 判定 |
|---|---|---|
| **T1** | 池运行期的**存储机制**（块 = `malloc`、`give` = `free`、`resize` = `realloc` + 补零、`live` 标志、plate 只属于自己） | **成立**。没有越界、没有重复释放、`new T[n]` 的"新分配读作 0"在 plate 里也守住了 |
| **T2** | **池粒度**的陈旧检查（`drop` ⇒ `live = 0` ⇒ `extc_pool_generation()` 返回 0；槽复用 ⇒ `new_at` 里 `generation++`） | **成立**。旧句柄携带的 `gen` 一定不再匹配（容器级 `pStale`/`pidGen` 就建立在这上面） |
| **T3** | **元素/块粒度**的陈旧检查（"这个槽/这块在这次失效之后还是原来那个吗"） | **证伪**：全文 `generation++` **只有 1 处**（在 `new_at`）；`extc_pool_freeSelf` 只置 `live = 0`、**不换代**；`extc_pool_reset` **既不换代也不置 `live = 0`** ⇒ 池仍然 live、`gen` 不变、而槽已被标记可复用 |
| **T4** | 因此「`build/extc` 接受的程序，在池档不会**静默 UB**」 | **证伪**：§5 的 E1 **实测** `heap-use-after-free`（检查器接受）；E2 由代码结构直接推出；E3 是 dyn 落地时必然出现的类型混淆 |
| **T5** | 边界：把 §7 的三条不变量补回去之后 | **可修**（三条都是局部改动：`reset` 一行、dyn 池只追加、视图不许进池；判据可双向写） |

**一句话的可操作结论**：

> 池档的洞**不在存储实现**，而在**两条记账**：
> ① **失效时必须换代**——今天只有"池死了"换代（`live = 0`）与"槽被新池占用"换代（`new_at`），
>    而 **`reset`（保留池、槽复用）两样都不是**；
> ② **块不能被单独回收/搬家**——`extc_pool_give` 是 `free`、`extc_pool_resize` 是 `realloc`，
>    两者都**不动 `gen`**。
> 所有反例长成同一个形状：**把一个"会被运行期替换或复用的块/槽"，当成了"活得和池一样久"。**

---

## 1. 形式模型

### 1.1 三样东西：块 / 池 / 地方

- **块（plate block）**：`extc_pool_take(rid, n)` 得到的一块 `malloc` 内存，挂在 `pools[rid].blocks` 链上。
- **池（pool）**：`extc_pools[rid] = { generation, live, zone, parent, firstChild, zoneNext, blocks, bytes }`。
  池**生在某个地方**（`zone` 字段 + `zoneNext` 链），随地方死（`extc_pool_zoneLeaveTo`）。
- **地方（zone）**：编译期的一摞栈；运行期只是 `extc_zones[]` + 颜色。

状态 `σ = (Z, P, B, A)`：地方栈、池表、块集合、arena 块集合。

### 1.2 实现真正用的三个判据

| 判据 | 代码 | 答的问题 |
|---|---|---|
| `live` | `pools[rid].live` | 这个池现在存在吗 |
| `generation` | `extc_pool_generation(rid)`；非 live 返回 **0** | 这个 `rid` 还是当年那只池吗 |
| 块在链上 | `pools[rid].blocks` 链（`give` 从链上摘掉并 `free`） | 这块现在属于这个池吗 |

**注意缺的那一行**：没有任何一个数回答「**这个槽/这块，在这次 `reset` 之后还是原来那个吗**」。

### 1.3 要证的命题（池档定理）

> 若每个"指向池"的引用（**句柄** `h = {pool, idx, gen}` 或**视图** `v = ref/slice`）满足：
> **(i)** 句柄每次取用都校验 `gen`，且 `gen` 在**任何可能让 `h` 失效的变化**上都改变；
> **(ii)** 视图指向的块，在视图的**整个静态生命周期**内不被替换也不会被单独回收；
> 则任何取用要么读到活内存，要么 **trap**（带 `.extc` 位置），绝不静默读到已释放/已搬家/已复用的内存。

**它依赖三条不变量**（下面就是账单）：

| 不变量 | 内容 | 今天 |
|---|---|---|
| **INV-G（换代覆盖）** | 任何让旧引用可能失效的状态变化，都必须让旧引用携带的 `gen` 不再匹配 | 池粒度 ✓（drop / 槽复用）；**元素粒度 ✗**（`reset`） |
| **INV-A（只追加）** | 以句柄寻址的存储，块在句柄可能活着期间不得 `free`/`realloc`（或必须换代） | **✗**（`give` = `free`、`resize` = `realloc`，都不换代） |
| **INV-V（视图不进池）** | `ref`/`slice` 不得指向会被 `push`/`clear`/`shrink`/`release` 替换的块 | **✗**（`string::sub` 零拷贝；且无静态规则禁止） |

---

## 2. 定理为什么是对的（前提成立时）

设 `L(σ, r)` = "引用 `r` 指的对象此刻活着"。

1. **分配**：`take`/`take_raw` 拿到的块**只属于这一只池**，只有 `give`/`resize`/池释放能取消它 ⇒ 块的活性是**单调**的（INV-A 保证不会在句柄活着时取消）。
2. **校验**：`gen` 匹配 ∧ `live` ⇒ 由 **INV-G**，"自 `h` 发出以来没有发生过能让 `h` 失效的变化" ⇒ 目标仍是当年那个 ⇒ `L` 成立。
3. **视图**：视图没有 `gen`，所以它**只能靠静态规则**：由 **INV-V**，块在视图生命周期内不被替换 ⇒ 地址一直有效（这与 arena 档的 `INV-H`（深度上界）是同一个形状：**静态记账必须是真实情况的上界**）。
4. **地方退出**：`zoneLeaveTo` → `drop` 每只池 ⇒ `live = 0` ⇒ 池粒度的句柄全部失效（且 `generation()` 返回 0）；视图由 arena 档的逃逸规则管（视图不能活过地方）✓

⇒ **池档定理成立 ⟺ INV-G ∧ INV-A ∧ INV-V。** 三条都是"记账"层面的，不是存储层面的。

---

## 3. 记账有哪些数（谁写、谁读、答哪个问题）

| 数 | 谁写 | 谁读 | 答的问题 |
|---|---|---|---|
| `pools[rid].live` | `new_at`（=1）、`freeSelf`（=0） | `generation()`、`take`、`give`、`resize`、`pStale` | 池还在吗 |
| `pools[rid].generation` | **只有 `new_at`（槽被新池占用时 `++`）** | `extc_pool_generation(rid)`（非 live 返回 0） | 这个 rid 还是当年那只池吗 |
| `blocks` 链 | `take` 头插、`give` 摘除、`resize` 换节点 | `give`/`resize` 按地址查找 | 这块属于这个池吗 |
| 容器的 `pid` / `pidGen` | 构造/`release` | `pStale()` | **这个容器**是不是陈旧了 |
| —— | —— | —— | **槽/块的陈旧：没有数** ← 缺口 |

**读法**：`pStale` 那一行说明"池粒度的检查"已经用上了（本会话早先在 `map`/`hashMap` 上验过它能把陈旧容器变成带位置的 trap）；
缺的是**下一格粒度**：dyn 的 `{pool, idx, gen}` 里的 `idx` 需要一个"槽粒度"的失效信号，而今天**没有**。

---

## 4. 证伪：三条不变量各自怎么被违反

### 4.1 INV-G 在元素粒度上不成立（`reset` 不换代）

```
void extc_pool_reset(int64_t rid) {
    ... 把子池全部 drop ...
    extc_pools[rid].firstChild = -1;
}
```

`reset` 的语义是"**保留池、把槽标记为可复用**"（注释原文：*Keep the pool, mark its slots reusable*）。
它**既不 `freeSelf`（不置 `live = 0`）也不 `generation++`** ⇒ 池仍然 live、`gen` 不变，
**而它的槽已经可以被后来的人拿去用**。
⇒ 任何"指向槽"的引用（今天没有语言级的，dyn 一落地就有）**能通过校验，指向另一个对象**。

### 4.2 INV-A 不成立（块被单独 `free` / `realloc`，换代没发生）

```
int64_t extc_pool_give(int64_t rid, void *p) { ... free(b); return 1; }
void   *extc_pool_resize(int64_t rid, void *p, int64_t bytes) {
    ExtcBlock *nb = (ExtcBlock *)realloc(b, sizeof(ExtcBlock) + bytes); ... return (void *)(nb + 1);
}
```

两者都可能让"某地址"从有效变成无效/搬家，而 `generation` **一个字节都没动**。
`give` 是容器换缓冲时调的（codegen 里 `extc_pool_give((int64_t)(...), (void *)(...).data)`）；
`resize` 是"原地增长"的优化（注释：比 take+copy+give 少一个峰值）。

### 4.3 INV-V 不成立（视图可以指向池块，且族内不一致）

| API | 行为 | 评价 |
|---|---|---|
| `vector::toSlice`（`stdlib/stl/vector.extc:70`） | **拷贝** | 注释（66–69）明说：*"没有'零拷贝视图'是有意的：那等于把池里的东西交出去（POOLS.md §5.4），而且守不住"* ✓ |
| `string::sub`（`stdlib/stl/string.extc:167`） | `return self.buf[a..b]` —— **零拷贝** | **违反同一条教义** ✗ |

而 `docs/topics/POOLS.md:792` 早就写下了结论：*"视图本身也守不住 —— `push` 一次（可能搬家）、
`clear`（翻纪元）、`shrink`…"*。**文档是对的，实现漏了一处** —— 与 arena 档"定理对、记账漏"同形。

---

## 5. 反例

### E1（**实测**）`string.sub` 零拷贝视图 + 增长 ⇒ `heap-use-after-free`

```extc
use std::io
use stl::string
fn main() -> i32 {
    var s: string = string::withCap(i64(4))
    s.append("hello world")
    var v: slice<u8> = s.sub(i64(0), i64(5))      // 零拷贝：视图指向池块
    var i: i64 = 0
    while i < 400 { s.push(u8(120))  i = i + 1 }  // 增长 ⇒ take+copy+give（旧块 free）
    io::cout << "v[0]=" << i64(v[0]) << "\n"      // 读已释放内存
    return 0
}
```

- `./build/extc -o /tmp/ps/v2.c /tmp/ps/v2.extc` ⇒ **接受**（无警告、无错误）
- `gcc -O1 -g -fsanitize=address -fwrapv /tmp/ps/v2.c -o v2 && ./v2`
  ⇒ `ERROR: AddressSanitizer: heap-use-after-free on address 0x6e40...`
- 归因：**INV-V 被违反**（视图指向池块），且 `give` 的 `free` 不换代（INV-A）。

### E2（**结构性**，不必运行）`give`/`resize` 在 `gen` 不变时让地址失效

只要"引用池内地址"的引用存在，`4.2` 那段代码就是**静默失效**的充分条件。
今天它以"容器换缓冲"的形式存在，**恰好常常不炸**：
`vector::push` 的增长走 `resize`（`realloc` 常常原地成功）⇒ 我实测 `vector::toSlice`（拷贝）
与"20 万次 push 后再读旧块"都**没有**报 UAF；而 `string` 的增长走 take+copy+give ⇒ 一炸就炸。
**E1 只是这一族里"恰好先炸的那个"**：换分配器、换大小、换顺序，E2 就会变成可复现的 UAF。

### E3（**设计期**）`reset` 不换代 + 槽复用 ⇒ **类型混淆**

dyn 的句柄是 `{pool, idx, gen}`。若 `idx` 落在被 `reset` 标记可复用的槽上，而 `gen` 没变：
旧句柄**通过校验**，指向**另一个对象**。若那个对象是**另一个 dyn 实现**，
派发就会用 A 的方法读 B 的布局 ⇒ **静默 UB（类型混淆）**，而且比 UAF 更隐蔽（内存是活的、值是错的）。

> 今天**不可复现**，因为 extC 还没有语言级的池句柄（`{pool, idx, gen}` 只存在于设计里）。
> 这正是本文最重要的用途：**它把"dyn 落地时会踩的坑"提前钉在了 `reset` 这一行上。**

---

## 6. 边界清单

### 6.1 已经守住的（不是洞，别误伤）

- 池的**存储实现**：`take`/`give`/`resize`/plate 的自有性、`new T[n]` 读作 0、`raw` 列的金丝雀（`tests/pool/run.sh` 两层）。
- **池粒度**的陈旧：`drop` ⇒ `live = 0` ⇒ `generation()` 返回 0；槽复用 ⇒ `new_at` 换代 ⇒ 容器级 `pStale`/`pidGen` 能把陈旧容器变成**带位置的 trap**（`map`/`hashMap` 上验过）。
- 与池无关但同属"不静默 UB"的那几条：越界索引 trap（带位置）、除零 trap、float→int 不精确 trap、`-fwrapv` 让有符号溢出有定义（本会话早先做过）。

### 6.2 洞的半径

> **"池粒度"与"块/槽粒度"之间的缝。**

- 换代只发生在"池死了"与"槽被新池占用"两处；
- 失效却可以发生在**块**（`give`/`resize`）与**槽**（`reset` 后复用）两处；
- 缝里能塞下：视图（E1，已实测）、任何句柄（E2/E3，dyn 落地即现）。

### 6.3 与文档不一致的地方（诚实对照）

| 文档说 | 实现 |
|---|---|
| `POOLS.md:792`：视图守不住，不能越过 `push`/`clear`/`shrink` | `vector::toSlice` **守住了（靠拷贝）**；`string::sub` **没守（零拷贝）** |
| `vector.extc:66-69`：零拷贝视图"等于把池里的东西交出去" | 同上——教义写对了，但只在 `vector` 落实了 |

---

## 7. 修复方向（三条不变量 + 双向判据）

### 7.1 必须成立的三条（写进代码，别靠注释）

1. **INV-G（元素粒度换代）**：`extc_pool_reset` 加 `generation++`（**一行**）。
   语义变成"重置 = 该池所有引用作废"，粗但 sound；若将来要"单槽失效"，再加**每槽代际**。
2. **INV-A（dyn 的池只追加）**：dyn 用的池**永不 `give`、永不 `resize`**；删除 = **墓碑**
   （对象仍在、标记已删 ⇒ 取用时 trap）。这一条同时买到：`self` 借用安全、迭代中插入安全、同池句柄稳定。
3. **INV-V（视图不进池）**：`ref dyn` 只借**栈 / arena**；`string::sub` 改成拷贝
   （或保留零拷贝但**改名**并给出明确契约：`subView` + "容器再增长/清理即失效"）。

### 7.2 验收判据（沿用本项目的老规矩，双向）

- **正例**（`tests/pool-soundness/`）：合法期内的句柄/视图照常可用（不能因为收紧而误杀）。
- **反例**（每个都必须 **trap 或编译错误**，且**不许**是 ASan 报 UAF）：
  - `reset` 之后用旧句柄 ⇒ **trap**
  - `give`/`resize` 之后用旧引用 ⇒ **trap**（或静态被拒）
  - `string::sub` 的视图越过后增长 ⇒ **编译错误**（改名/拷贝之后）
  - dyn 侧：卸载/`reset` 之后调用 ⇒ **trap 带位置**；跨实现槽复用 ⇒ 不许静默派发
- **oracle**：`build/extc` 接受 + `gcc -fsanitize=address` 编过 + 运行**不是** UAF/类型混淆。

---

## 8. 复现清单（全部在 `/tmp`，仓库未改）

```bash
# E1：实测 heap-use-after-free（检查器接受）
cd <repo>
./build/extc -o /tmp/ps/v2.c /tmp/ps/v2.extc
gcc -O1 -g -fsanitize=address -fwrapv /tmp/ps/v2.c -o /tmp/ps/v2 && /tmp/ps/v2
#   => ERROR: AddressSanitizer: heap-use-after-free

# 对照 1：vector::toSlice 是拷贝（同一形状不炸）
./build/extc -o /tmp/ps/v1.c /tmp/ps/v1.extc && gcc -O1 -g -fsanitize=address -fwrapv /tmp/ps/v1.c -o /tmp/ps/v1 && /tmp/ps/v1
#   => s[0]=11 s[1]=22（20 万次 push 之后仍然对：因为 toSlice 拷贝，且 resize 恰好原地）

# 对照 2：reset / freeSelf 都不换代（结构证据）
python3 - <<'PY'
s=open('src/pools.c',encoding='utf-8').read()
print('generation++ 出现次数:', s.count('generation++'))     # => 1（只在 new_at）
i=s.find('extc_pool_freeSelf(int64_t rid)'); print(s[i:i+120])
PY
```

---

## 9. 定理的**相对性**（TCB）与莱斯定理的关系

池档定理（以及 arena 档定理）都是**相对**的，前提是这三样东西诚实：

1. **FFI 声明**：`extern!("libc") fn write(...) effects Addr=0 Cont=0` 是**承诺**
   （"我不存你的指针"）。C 侧说谎（存了指针、或之后写它），extC 看不见 ⇒ 静默 UB。
   这是第三档（外面来的内存）的**固有边界**：它只能靠"签字 + 信任"。
2. **`!` 逃生门**：显式放弃检查的地方，定理不覆盖（用户签过字）。
3. **`dlclose` 契约**：卸载一个还持有活池的库 ⇒ 必须"禁止卸载"或"卸载时把所有相关池置为墓碑"，
   否则句柄会指向已卸载的代码（dyn 的 vtable 尤其）。

**关于莱斯定理**：它限制的是**分析的完备性/精度**（不存在既 sound 又 complete 且能判定任意语义性质的分析），
**不是 soundness**。宁严勿松永远能 sound —— 代价是**误报**（把安全程序拒掉）与**精度预算**
（比如 arena 档把深度记账取上界、池档禁用零拷贝视图）。
所以"会不会静默 UB"是**可证**的，我们要付的不是"不可能"，而是**保守**。

---

## 10. 一句话

> **池的存储是对的；漏的是"什么时候必须换代"和"什么引用可以指向池"。**
> 三条不变量（INV-G / INV-A / INV-V）补上，池档与 arena 档就各自成为了
> "**每条引用要么读到活内存，要么 trap**"的一个引理；dyn 只是把这三条不变量
> **必须现在就满足**而已。

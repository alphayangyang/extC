# 加固记录（自主迭代）

来源：主人交代"上强度、构造 hack 数据、跑 fuzzer，遇到问题就修，修不了的记下来"。
工具：[tools/fuzz.py](../tools/fuzz.py)（变异 fuzzer）· [tools/golden.sh](../tools/golden.sh)（413 份生成物逐字节 + 必须合法 C）。

## 一、已修（都有回归判据 + 提交）

| 编号 | 问题 | 触发（最小） | 修法 | 提交 |
|---|---|---|---|---|
| H1 | **SIGSEGV**：`main` 里对**非 option** 用 `?` | `fn main() -> i32 { let x = 5?` | `checkTryInner` 的"main 里失败即 trap"分支加"操作数真的带载荷"守卫；非 option 落到原分支报 `` `?` needs an `option<...>` `` | `724ca7d` |
| H2 | **SIGSEGV**：畸形结构体字面量 + 泛型返回 | `fn make<T>(x: T) -> pair<T> { return { a: make, b: x } impl }` | `levelOfValue2` 里 `identBindOf` 结果为 NULL 时提前返回（与"无发布可跟"同答案；名字没解析出来本身就会报错，不可能被接受） | `724ca7d` |

两条回归进 `tests/errors/`（`// expect:` 注释由 `tools/parrun.py` 自动收集，parrun 285/0）。

**Fuzzer 本身**：变异保留源码行结构（只换 token），否则几乎全成语法错、走不到 codegen；实测 yield 100%
（3 seed × 400 次里 1199 次编译成功）。失败用例带 seed/迭代号，可复现。

### H3（已修）非有限浮点字面量生成 `inf`，C 里没有这个标识符

`tools/fuzz.py` 的变异词表里有 `1e400`，两个程序因此"extC 说成功、gcc 编不过"：
`-1e400` 被 `%g` 打成 `-inf` ⇒ `error: 'inf' undeclared`。

修法：`EX_FLOAT` 落地时先判断 NaN / ±∞（`v != v`、`v * 0.5 == v`），走
`__builtin_nan("")` / `±__builtin_inf()`；**不引入 `<math.h>`**，否则每一份生成物都会变。
实测：`1e400` / `-1e400` / `f32(-1e400)` 都产出合法 C，且 413 份生成物**逐字节不变**
（正常浮点走原路径）。回归：`tests/asan/nonfinite-float.extc`（该套件按目录 glob 收集）。

### H4（已修）给字面量赋值被接受，生成的 C 不是左值

`fail-00075`：变异把标识符换成了字面量 ⇒ 源码 `18446744073709551615 = total + p.val`，
extC 说成功，生成的 C 是 `9223372036854775807 = (total + p->val);`，gcc 报
`lvalue required as left operand of assignment`。**赋值左边从来没被要求是可写位置**。

修法：`case ST_ASSIGN` 开头加校验，判据与 C 一致 —— 只看**最外层形态**
（`EX_IDENT` / `EX_FIELD` / `EX_INDEX` / `EX_SLICE` / `EX_DEREF` / `EX_SIGN`），其余（字面量、
算术/比较表达式、调用）没有存储。两处踩到又修掉的误拒，都靠语料抓出来：

1. 先写成 `isPlace(target)`（那是为**切片**写的判据，只认绑定/字段/下标/切片）⇒ 25 份语料编不过；
2. 补上 `EX_DEREF` 后仍是 5 份 ⇒ 真正原因是"最外层"这个原则：`self.big![self.n] = b`（stdlib）
   与 `(*buf)[k] = (*src)[i]`（bench/oi）的外层是下标/签名，C 允许，而按根递归就误判了。

实测：6 种非法形状全拒（字面量 / 大整数 / 调用结果 / 算术 / 比较 / 复合 `+=` 字面量），
6 种合法形状全通（变量 / 字段 / 下标 / 解引用 / 括号解引用下标 / 取字段再下标）；
**413 份生成物逐字节不变**；parrun 285 → 286/0（回归 `tests/errors/assign_to_literal.extc`）。

## 二、已修：文本后处理 pass 的**越界删除**（2026-09-28 修完，见 §3.2 的收尾）

**症状**：`examples/prelude.extc` 的生成物在某种路径写法下是**非法 C**：

```c
/* 期望 */ (printf("%s", (slice_u8_isEmpty(&(b))) ? "true" : "false"), printf("\n"));
/* 实得 */ (printf("%s", (slice_u8_e" : "false"), printf("\n"));
```

**可复现的判别对**（同一棵树、同一份源码；两次编译各自 10/10 确定性一致）：

| | 从仓库根编译 `examples/prelude.extc` | 拷贝到 `/tmp/*.extc` 后编译 |
|---|---|---|
| 生成物哈希 | `ac9c8d53…` ✗ 非法 C | `f3ac7cfd…` ✓ 合法 |

**更正（2026-09-28，之前写成"路径写法"是不准确的）**：我又量了一轮 —— 在 `/tmp` 下用**相对**
与**绝对**两种写法编译同一个拷贝，输出**完全一致**；把文件从 `examples/` 挪到 `/tmp` 才改变结果。
所以真正的变量是**编译上下文**（同一份源码在仓库根与在别处解析出的编译单元不同），
而不是路径字符串本身。曾经想把它做成 fuzz oracle 的一条"同一输入必须产出同一 C"判据，
因为上面这个反例（合法的上下文差异也会改变输出）会**误报**，已撤掉。

**因果链（已用探针钉住）**：

1. pass 流水线**迭代多轮**：`dropUnreferenced` 被调用三轮（文本 11571 → 4950 → 3557）。
2. 第 1 轮 `dropRuntimeDefs` 之后调用仍在（`len=11548 调用=11400`）；第 2 轮入口就没了
   （`len=4950 调用=-1`）⇒ 是第 1 轮 `dropRuntimeDefs` 的某刀吃掉了它。
3. 逐刀加"是否覆盖这次调用"的探针，命中的是：

   ```
   [rt] extc_arena_alloc  at=3080 span=2030 调用在5105 覆盖=★★是★★
   ```

   `extc_arena_alloc` 的定义被按 **2030 字节**的范围删除 ⇒ 越界切进用户代码，吞掉 `main` 里的调用。
4. 机制：多行定义的 `span` 是"从定义首行扫到**第一个列 0 的 `}`**"。当该定义已被**前面的轮次**
   部分删掉时，这个扫描会越过定义、落到**用户函数**的收尾花括号上。

### 2.1 修法与证据（round 6 收敛）

两处改动，都在 `dropRuntimeDefs`：

1. **定义结束改成按括号深度配对**（原来"扫到第一个列 0 的 `}`"）：定义完整时停在它自己的收尾；
   被部分删过时深度回不到 0 ⇒ `span` 保持 0 ⇒ **不删**；
2. **扫描与删除都不许越过文本末尾**：`for (; p < rp + rl && p < text + len; p++)`，并在删除前加
   `if (ln + span > text + len) span = 0;`。

定位过程（每一步都是实测，不是推理）：

| 观察 | 结论 |
|---|---|
| `EXTC_SKIP_*` 逐个关 pass | 损坏需要 `dropRuntimeDefs` **和** `dropUnreferenced` 都跑 |
| 逐刀"是否覆盖这次调用" | `[rt] extc_arena_alloc at=3080 span=2030 覆盖=★★是★★` |
| 打上改动 1 之后 | 所有 `span` 变成定义大小（161/242/…）✓，**2030 消失** ✓ —— 但输出**逐字节没变** ✗ ⇒ 那一刀是**红鲱鱼** |
| 在每个 pass 后查损坏串 | 它在 `cg-runtime` 里出现 |
| 在 pass 内部逐刀查 | 每刀之后都干净 ✗（我当时只看前 14 行，那是第 1 轮）|
| 进入/离开该 pass 对照 | **第 2 轮**：进入干净（len=10715）→ 离开已坏（len=4950）✗ |

⇒ 综合起来：没有任何一刀"覆盖"那次调用，调用却消失了 —— 只可能是**越界**：`rp + rl` 用的是
**原始**运行期文本长度，第 2 轮里它已经指向缓冲区之外，`span` 在界外量出来之后
`memmove` 的长度 `len - (ln - text) - span + 1` 会下溢成天文数字，一次拷贝毁掉后面整个文件。
改动 2 正是堵住这条（改动 1 单独不够：它只让 span 更合理，越界仍然发生）。
**注**：下溢这一步是从"界外 span + 无覆盖却损坏"推出来的机制，未逐字节验证；已验证的是
两处界加上去之后损坏消失、413 份全部合法。

**验证**：`examples/prelude.extc` 在**仓库根**编译 ⇒ 合法 C ✓；全量 413 份**只有这一份**变化，
且是"非法 C → 合法 C" ✓；`tools/golden-known-bad.txt` 已清空（任何一份产物不合法都会让闸门变红）；
`tools/golden.sh` ⇒ 413/413 逐字节 ✓ 且 **非法 C 0** ✓；parrun 286/0 ✓；asan 9 项 ✓；instimpl 15/15 ✓。

**试过但无效（都已回退，量过）**：① 把扫描窗口 `rl` 提到外层循环外；② 定义长度改成"在当前文本里
再找一次列 0 的 `}`"；③ 用 `g->primText` 里同一行的原始长度给 `span` 加上界（`strstr` 整行查找，
仍没拦住那一刀 —— 说明同一次删除里还有别的路径在改 `span`，或越界发生在另一处 `memmove`）。

## 三、待拍板（需要主人决定，先不阻塞）

### 3.1 文本 pass 的偏移记账要不要重做（对应 §二）

**补记（round 3）：这件事按目标 ② 本来就在"我做"的范围内，不需要拍板 —— 只是要挑一个
不与我自己的构建互相干扰的时间窗做。** 下面是据已有证据写好的落地计划。

已知的最后一环（探针原话）：

```
[rt] extc_arena_alloc  at=3080 span=2030 调用在5105 覆盖=★★是★★
```

`dropRuntimeDefs` 里多行定义的 `span` = "从定义首行扫到第一个列 0 的 `}`"，窗口上界是
`rp + rl`，而 `rl` 每轮外层循环重置为 `rl0 = strlen(g->primText)`（**原始**长度）。流水线跑多轮 ⇒
第二轮起当前文本里已没有整段运行期块，窗口却仍按原始长度延伸 ⇒ 扫描越过块尾、落到**用户函数**的
`}` 上 ⇒ 一刀 2030 字节。三次尝试都**没拦住**，原因分别是：

| 尝试 | 为什么无效 |
|---|---|
| 把 `rl` 提到外层循环外 | 越界发生在**单次**扫描里，跨迭代收缩救不了它 |
| 定义长度改成"在当前文本里重新找列 0 的 `}`" | 找的仍是那个**错的**位置（扫描起点已经越界） |
| 用 `g->primText` 里同一行的原始长度给 `span` 上界 | 行查找一开始写成 `strstr(g->primText, ln)`（`ln` 不是 C 字符串 ⇒ 永远找不到）；改成整行拷贝后仍没拦住 ⇒ 说明那一刀之外还有另一处 `memmove`/另一条路径在改 `span`，**没定位完** |

**round 5 读代码的更正（不必再打点了）**：`dropRuntimeDefs` 里**只有一处** `memmove`
（`codegen.c:5323`），`span` 的唯一来源是 `:5308-5309`「从定义首行扫到**第一个列 0 的 `}`**」。
所以上一轮写的"还有第二处 `memmove` 在改 `span`"是**猜错**（那是没打点的猜测）。真正的机制是：

* `rl0 = strlen(g->primText)` 是**原始**运行期文本长度，而 `size_t rl = rl0` **每次调用**都重置；
* 流水线跑到第二轮时，当前文本里的运行期块已经短了一截 ⇒ 窗口 `rp + rl` 从一开头就偏长
  ⇒ 扫描越过块尾、落到**用户函数**的 `}` 上 ⇒ `span=2030`；
* 因此任何"调用内"的记账（第 ① 次改动把 `rl` 提到外层循环 ✗）都救不了它 —— 偏长发生在
  进入这次调用之前。

**下一次的修法（按括号深度配对，一次只动这一处）**：

把 `:5308` 的扫描从"第一个列 0 的 `}`"改成**深度配对**：从 `ln` 起数 `{`/`}`，定义在
**深度回到 0** 的那个 `}` 结束（`ln` 所在行以 `{` 收尾 ⇒ 初始深度 1；单行定义也自然覆盖）。

* 定义**完整**时：正好停在它自己的收尾 ✓（比列 0 启发式更准）；
* 定义**被部分删过**时：开头那个 `{` 永远配不上 ⇒ 深度回不到 0 ⇒ 窗口内找不到结束
  ⇒ `span = 0` ⇒ **不删**（宁可不删，绝不越删）✓。

成功判据：`examples/prelude.extc` **在仓库根**编译也产出合法 C ⇒ 从
`tools/golden-known-bad.txt` 删掉那一行 ⇒ 提交信息写明"基准只此一份变化、且是非法 C → 合法 C"。

**下面这段是上一轮写的打点计划，保留备查（其中"第二处 memmove"的前提已被上面更正）：**

1. 在第 1 轮 `dropRuntimeDefs` 的**每一次** `memmove` 前后各打一条 `len` + 调用位置（已有探针骨架 ✓），
   找出除 `[rt]` 之外还有谁在缩短文本；
2. 落地"**甲**"：删除前要求"被删区间在当前文本里**首行匹配且以列 0 的 `}` 收尾、且区间内不再出现
   另一个列 0 的 `}`"——不满足就**跳过**（宁可不删，绝不越删）；
3. 若"甲"留下残留（生成的 C 里出现半截定义 ⇒ gcc 会报），再上"**乙**"：删之前把所有区间**收集**完、
   从后往前一次性应用（`markUncalledFunctions` 那边有同类写法可参照，我还没定位到它的具体结构）；
4. 成功判据：`examples/prelude.extc` 在**仓库根**编译也产出合法 C ⇒ 从 `tools/golden-known-bad.txt`
   删掉那一行，并在提交信息里写明"基准只此一份变化、且是非法 C → 合法 C"。

### 3.1′ 原问题（保留备查）：偏移记账要不要重做

越界删除的根子是：**每个 pass 都拿着"生成时捕获"的偏移/文本，而流水线会跑多轮、文本每轮都在变**。
可证安全的做法有两条方向，选哪条是设计决定：

- **甲**：删除一律"先在当前文本里重新定位、并且只删**完整匹配**的定义"（发现不完整就跳过）——
  改动小，但可能出现"该删没删"的残留；
- **乙**：给每个 pass 维护"当前文本里各定义的真实区间"，每刀之后同步更新（或干脆先收集所有区间、
  从后往前一次性应用）—— 更彻底，但要动 pass 的数据结构。

### 3.2 基准重设的范围

`tools/golden-known-bad.txt` 里目前挂着 `examples/prelude.extc`（非法 C）。§二修好之后它应当变成
合法 C，那一天需要把这一份的哈希一起更新（**只此一份**，且是"非法 → 合法"）。

### 3.3 `impl` 方法不读 `self` 触发 `-Werror`（L 债）—— 功能可修，但**按目标判据不落地**（round 7 量过）

**修法与效果**（`impl Tag for box { fn tag(self: ref box) -> i64 { return i64(9) } }`，方法体不读 `self`）：

把"**有调用点**的 `impl` 方法"登记进 `g.deadFuncs`（`markUnusedParams` 只遍历它），
登记点必须在**原型循环**里（放进收集循环会把原型排到 `#include` 之前，整份文件编不过）。
实测该形状从 `-Werror=unused-parameter` 失败变成**编译通过、运行正确** ✓。

**为什么仍然不落地**：量了输出面 —— 413 份里有 **410 份变化**：

| 类别 | 数量 |
|---|---|
| **只删 3 个空行**（外观） | **406** |
| 含代码删除（死代码消除） | 4（`tests/dyn/dyn_call.extc` 21 行、…）|
| 只插 `EXTC_UNUSED` | 0 |

目标 ③ 的判据是"变化不止**属性/死代码**两类 ⇒ 只留档不落地"，而"删空行"是第三类
（外观 ✓ 但确实是字节变化）⇒ 按判据回退，代码停在 `64e3c3b`。

**关于那 406 份"恰好 3 个空行"（round 8 更正，待验证）**：我上一轮把它写成"可独立修的伪影"，
但读代码后有了更可能的解释 —— 生成模板里**块与块之间恰好隔 3 个空行**，而 pass 的删除是**按区间**
的（`memmove` 精确删一段），"删掉这个块"自然连**它后面的分隔空行**一起删掉 ⇒ 于是每份被处理到的
文件都恰好少 3 行。若这个解释成立，它**不是**一个可单独修的 bug，而是"删除即连带分隔"的固有结果
⇒ ③ 的几百份字节抖动就是**激活 pass 的固有代价**，不是先修伪影就能避免的。

**round 9 实测（假设证实到形状，机制未钉死）**：在 worktree 里装上完整的 ③（含原型循环那段），
与主树编同一份 `examples/alloc-in-block.extc` 对比，差异是：

```
delete before[152:155]   删掉的正是 3 行空行
上下文: ['    int64_t len;', '};', '']   ← 上一行是 `struct slice_i32 { … };`
```

⇒ 那 3 个空行是**两个顶层项之间的分隔**（`struct slice_i32 {…};` 与下一个构造之间），被原地吃掉，
不是"某个被删块的尾巴"。但**谁吃的还没定**：现有 5 个 pass 里没有重建顶层项的，而两个 marking
pass 的重写是"整份逐字节拷贝 + 按偏移插入属性"，理论上不丢行 ⇒ 留作待查（它决定 ③ 能否字节无损落地）。

**round 10：这条路我追了四轮，没结论，先收手（记下方法与教训）**。最后两次尝试是：

* 在 pass 边界放"精确标记"探针（盯 `};\n\n\n\nstatic int32_t inner` 这段分隔）⇒ **从来没匹配上**，
  连"生成刚结束"那一刻也是"无" ⇒ 说明**我对形状的假设是错的**：diff 里的"删掉 3 行"很可能不是
  "字面意义上的三行空行被删"，而是某个 pass **重排/重写**了那一带文本（行数与内容同时变，diff 才
  显示成 3 行）—— 这也解释了此前若干次"按形状写守卫却毫无效果"。
* 教训：不要在 pass 边界猜形状，**应当在每个 pass 边界把整个缓冲区落盘**（`out->data`/`out->len`
  直接写文件），跑一遍之后逐对 diff ⇒ 一次就能指出是哪个 pass、改了哪一段。这个做法留作下次。

⇒ 它是**外观项**（不影响生成物正确性，只影响 ③ 能否字节无损落地），而目标的核心是 ①（fuzz 与修
真 bug）⇒ 本项按判据继续"只留档"，把力气留给战役。
若要验证这个解释：装上完整的 ③ 登记（含原型循环那一段），取一份变化的文件，看那 3 个空行是不是
紧跟在某个被删除的运行期块之后。

## 三点五、H5（已落地）：视图存储为空时索引会**往 null 写**

**来源**：战役5（三模式 × 5 种子 × 400 次）第 2 条真发现，变异自 `tests/pool/rt_promote.extc`：

```
/home/alphayang/extc-fuzz/w00086/case.c:819:91: runtime error: store to null pointer of type 'int32_t'
```

**根因**：视图索引原语只查下标范围，不查存储指针（`genViewIndexer`，`codegen.c:653`）：

```c
static inline int32_t *slice_i32_index(slice_i32 v, int64_t i, const char *file, int line) {
    if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);
    return &v.data[i];        /* data == NULL 且 i == 0 时，范围检查通过 ⇒ 直写 null */
}
```

变异后的程序里 `extc_pool_take` 什么都没给（`.data` 是 NULL），而切片仍宣称长度 4 ⇒
`slice_i32_index(s, 0, …)` 返回空指针 ⇒ 赋值写空。按 extC 的规矩这里**应当 trap**（解引用 null
要报错），实际放行成 UB ⇒ 这是健全性缺口，不是程序的错。

**修法**（已在 worktree 里验证，补丁存 `~/extc-work/h5-view-null-check.patch`）：

```c
    if (!v.data) extc_trapMsg(file, line, "the view has no storage");
    if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);
```

用已有的 `extc_trapMsg`，**不新增运行期原语**。实测复现用例从"往 null 写"变成
`case.extc:24: trap: the view has no storage`（退出码 1）✓。

**落地**：主人拍板"**健全性修复直接做，不用报**"（只有很严重的问题才需要报）⇒ 已落地并重设基准。

* 实测该改动让 **258 / 413** 份语料变化（凡是用到视图下标的程序多这一句检查），
  **0 份变成非法 C** ⇒ 全部仍是合法 C，基准已按当前编译器重算；
* 回归：`tests/traps/view_no_storage.extc`（该套件的判据是"必须 trap 且带源码位置"），
  parrun 由 286 增至 **287/0**；
* 补丁留档 `~/extc-work/h5-view-null-check.patch`。

**分工约定（主人 2026-09-28 定）**：**健全性修复（UB / 崩溃 / 生成物非法）自主落地**，
包括为此重设相应基准；只有**很严重**的问题（大范围语义变化、需要改语言规则、影响面无法估算）
才需要事先报备。这条规则写在这里，后续轮次照此执行，不再逐条请示。

## 三点六、H6（已落地）：字符串里的非法转义原样透传给 C 后端

**来源**：战役5 的第 3 条发现（`fail-00021`，原语料 `tests/pool/rt_nest_promote.extc`；战役6
又独立抓到同一条）——变异在字符串里插了 `\u64`。extC 说成功，gcc 拒绝整份文件：
`incomplete universal character name \u64`（C 的 `\u` 必须跟 4 位十六进制）。

**根因**：`lexString` 的注释写着"转义原样透传给 C 后端，parser 与检查器从不看里面" ⇒ 它只做
`lxAdvance` 跳过转义；而**字符字面量那边早就有**一个完整校验器（`lexCharEscape`：`n t r 0 \ ' " a b f v x`，
`\x` 要求至少一位十六进制，其余报错）。

**修法**：把那份唯一的权威改名为 `lexEscape(…, const char *what)`（错误措辞按种类），
`lexString` 复用它 ⇒ **一份列表管两种字面量**。

**踩到并修掉的坑**：`lexEscape` 会消费转义及其参数，我在循环末尾又 `lxAdvance()` 了一次
⇒ 每个转义多跳一个字符 ⇒ **197/413 份生成物的字符串文本被改**（闸门当场抓住）⇒ 改成 `continue`。

**验证**：复现用例报 `error: unknown escape '\u' in a string literal`；合法转义
（`\t \n \" \\ \x41`）照旧编译运行正确；`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0**
（纯加诊断，**零输出变化**）；回归 `tests/errors/bad_escape_in_string.extc`，parrun 287 → 288/0。

**更正上一轮的一条不实说法**：提交 `5659c28` 的信息里写"战役5 因此丢了一个用例"——
不准确。战役5 的 3 片失败**都存下来了**（`fail-00021` / `fail-00034` / `fail-00086`）。
目录名带 mode+seed 仍然是值得的加固（避免将来撞名覆盖），但那次并没有真丢东西。

## 三点七、H7（已落地）：数组与标量比较被放行 ⇒ 描述符比较越界读

**来源**：战役7（三模式 × 6 种子 × 500 次）的变异体（原语料 `examples/generic-free-fn.extc`，
同一种子在 `mutate` 与 `modules` 两个模式各命中一次）：

```
==ERROR: AddressSanitizer: stack-buffer-overflow ... READ of size 8
    #0 ... in extc_eq
```

**形状**：变异把 `a[i] == x` 削成 `[i] == x` ⇒ extC 把 `[i]` 当成 **1 元素数组字面量** ⇒
泛型实例化后是"`[1]i32` vs `i32`"的比较。生成物按**左操作数的描述符**（`array_1_i64`，元素 8 字节）
去读右操作数（`i32`，4 字节）⇒ 越界读。

**两处放行**（都要修，缺一不可）：

1. `check_escape.c` 的数组规则递归问"**元素**能不能和 `rhs` 比" ⇒ `i32` 能比 `i32` ⇒ 通过 ✗；
   改为"数组只能和**同一个数组类型**比，再判元素"，一行。
2. `check_top.c` 的延迟复查（`runOpCheck`）在"两边类型不等且左边不是结构体"时**直接 return** ✗
   ⇒ 那次比较从头到尾没人判。删掉这句提前返回，把判定交给 `typeSupportsOp` —— 它本来就是
   "这个类型能不能和那个类型比"的唯一权威（数值走 `cmpIsNative`、数组判元素与同型、结构体走
   `findOp`）。

**验证**：变异体现在报 `` `indexOf_i32` needs `[1]i64` to define `==` ``；
`examples/generic-free-fn.extc` **本体照旧编译运行正确**（`indexOf(30) = 2 …`，退出码 0）；
`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0**（**零输出变化**，收紧了判据而已）；
parrun ⇒ **288/0**（无任何误拒）；回归 `tests/errors/array_vs_scalar_eq.extc`。

## 三点八、定向攻击（tools/attack.py）：泛型组 47 题，先修 H8（实例化失控 ⇒ 挂死 + OOM）

**换打法**：不再只撒网（fuzz），而是**按实现结构点名攻击**。`tools/attack.py` 每组题都带期望
（ok / reject / trap），再叠三条硬 oracle（不许崩 / 说成功就必须合法 C / 不许 UB），组名可单独跑。

第一组 **泛型 47 题**（实例化身份与命名撞车、替换与签名、延迟检查即 H7 家族、每实例方法集隔离、
递归终止、与 dyn/协程交叉）。首轮命中 9 条，先修最严重的：

### H8（已落地）：自递归实例化让编译器失控（挂死 54 秒后被 OOM 杀掉）

**触发**（三行）：

```extc
struct box<T> { v: T }
fn grow<T>(x: T) -> i64 { var b: box<T>
  b.v = x
  return grow(b) }        // 每实例化一次，实参类型就深一层
```

**性质**：不是"慢" —— 它**同时吃爆内存**：每步的实参类型比上一步深一层（`box<T>` 再嵌一层），
第 N 步的类型有 N 层，而每份拷贝是 O(N) ⇒ 总量 O(N²) ⇒ 实测 54 秒后被 SIGKILL（OOM）。

**修法**：在 `funcInstance` 入口按**创建的类型的嵌套深度**设限（`TYPE_DEPTH_LIMIT 64`），
超限就用仓库既有的那句措辞报错（"This is a limit of the compiler, not something wrong with the
program; please report it …"），并给实例**总数**留一个兜底（`FUNC_INST_LIMIT 4096`）。
深度才是要害：只限个数时，4096 步 × 每步 O(深度) 照样把内存吃光（第一版就是这么失败的）。

**验证**：E3 现在**立即**返回
`error: internal: a generic instance was requested with a type nested 65 levels deep …`（退出码 1）；
十层嵌套（攻击套件 A4）不受影响；`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0**（零输出变化）；
parrun ⇒ 289/0。

### H9（已落地）：泛型函数返回 `T` 构造的实例 ⇒ 零值初始化用了**模板名**（生成的 C 非法）

**触发**（B14，最小的那个）：

```extc
struct box<T> { v: T }
fn wrap<T>(x: T) -> box<T> { var b: box<T>
  b.v = x
  return b }
```

生成物里**声明与初始化用了两个名字**：

```c
box_i64 b = (box_T){ .v = 0 };      /* gcc: `box_T` undeclared ⇒ 整份文件编不过 */
```

**根因**：`zeroValue` 收到的是**模板**类型 `box<T>`（检查器在替换 `T` 之前就把局部变量的类型
记下来了），而它随后"进入实例上下文"用的是 `t->targs` —— 那恰好就是 `[T]` ⇒ **用 T 替换 T**，
原地打转 ✗。声明侧用的是 `cType`，它**第一步就 `subst`** ⇒ 于是两处名字不一致。
（第一次尝试只把 `t->name` 换成 `cType(g, t)`，因为替换上下文是空的，输出**一点没变** ✗ ——
真正缺的是入口那一步 `subst`。）

**修法**：`zeroValue` 入口先 `t = subst(g, t);`（与 `cType` 同步），`TY_GENERIC` 分支用 `cType(g, t)`。

**验证**：B14 与 **A4（十层嵌套）**都产出合法 C 并运行正确（`box=8 pair=3,4`）；
`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0**（零输出变化 ⇒ 语料里没有这种形状，所以它一直是
"能编过就看不见"的暗坑）；回归 `tests/instimpl/t_gen_return.extc`（含 `pair<A,B>` 双参数版本）。

### 泛型组剩余的 7 条（按严重度排队，下一轮继续）

| 题目 | 症状 | 归类 |
|---|---|---|
| F4 | 泛型函数里开协程（`coroutine<T>`）⇒ **生成的 C 非法** | A4/B14 的同族残余 |
| A5 | 用户类型名 `pair_i64` 与实例 C 名撞车 ⇒ `redefinition of 'struct pair_i64'` | 生成非法 C |
| F1 | `dyn Tag` 打在"泛型 trait impl"的实例上 ⇒ 被误拒（`` `pair_i64` does not implement `Tag` ``）| 误拒 |
| B7 | `var c: coroutine<i64> = gen(i64(9))`（`gen<T> -> coroutine<T>`）⇒ 被误拒 | 误拒（待查）|
| B3 / B10 | 显式实参 `zero<i64>()` 不支持 ⇒ 但报的是"cannot infer type parameter" | 诊断措辞误导 |

## 三点九、H10（已落地，兜底）：用户类型名撞实例 C 名 ⇒ 生成非法 C

**来源**：攻击套件 A5（三行）：`struct pair_i64 { a: i64 }` 与 `pair<i64>` 的实例名 `pair_i64`
同时存在 ⇒ 生成的 C 里两份 `struct pair_i64` ⇒ gcc 报 redefinition，extC 却说成功。

**修法**（兜底）：在 `generateC` 入口核对"实例的 C 名 vs 用户声明"，撞了指名报错，并给另一处
交代怎么改。两个坑：① 放在检查器里**从不触发**（最后一批实例是在检查器之后才建的）；
② 第一版把编译器自己造的**方法持有者**（`pair<i64>` 的持有者也叫 `pair_i64`）当用户声明 ✗
⇒ 误报了 `impl pair<i64> { … }` 与泛型 receiver（套件 D3/F2）。判据最终用
"`sd->type->sdef != sd` ⇒ 编译器造的持有者，跳过"。

**验证**：A5 报 `` `pair_i64` is the name the compiler needs for an instance of `pair`, and the
program already defines it ``（退出码 1）；`tools/golden.sh` ⇒ 413/413 · 非法 C 0（零输出变化）；
parrun 290/0；回归 `tests/errors/instance_name_collision.extc`。

**注意这是兜底**：主人提出**根治**方案 —— codegen 生成的一切名字一律以**双下划线**开头，
语言层面禁止用户标识符以 `__` 开头（C 也是这么保留给实现的）。那样整族 mangling 冲突
（本条、`struct extc_arena` 撞运行期、`array_*`、`_index` 辅助函数…）一次性消失，
本条检查退化成"永不触发"的安全网。落地要改所有生成名的拼写 ⇒ 413 份基准全量重设。

## 三点十、F4（已定位到根，修法比预想深一层，未落地）：泛型协程**没有自己的帧**

**症状**（攻击套件 F4，泛型函数里开协程）：

```
invalid application of 'sizeof' to incomplete type 'struct gen_i64$frame'
EXTC_UNUSED static bool gen$step(struct gen$frame *f);      /* 模板的 step 反而"用了没定义" */
```

**根因链**（两次试错逐层剥出来的）：

1. 帧类型由 `checkFunc` 的协程 prologue 建立（`check_top.c:3834` 的 `if (f->isCoro)`），
   并且它把 `f->ret` **改写**成帧类型（`f->ret = cft`）✓；
2. 泛型实例是模板的**浅拷贝**（`funcInstance` 里 `*in = *tmpl`）⇒ 实例继承的是**已经改写过的**
   `ret`（模板的帧类型 ✗）⇒ `isProtoType(ret, "coroutine")` 为假；
3. 于是实例的协程 prologue 被跳过 ⇒ **没人给实例建帧** ⇒ 生成物里 `gen_i64$frame`
   只有使用、没有定义 ✗；
4. 第一次尝试（给"帧单元收集"那趟补上 `g.funcs`）**更糟** ✗：实例的 `coroFrameType` 仍指着模板的帧
   ⇒ `struct gen$frame` 被发两遍（redefinition）✗，而 `gen_i64$frame` 依然不存在；
5. 第二次尝试（记住写下来的 `coroutine<T>`，实例化时替换回来并清空 `coroFrameType`）也失败 ✗：
   实例的 body **不走** `checkFunc` 那个 prologue ⇒ `ret` 停留在原型 `coroutine<i64>` ⇒
   codegen 按"句柄"发成 `extc_coro` ✗，而它期待的其实是帧类型。

### F4 + B7 的共同根：实例**完全不跑**协程 prologue（拼图已完成三块中的两块）

**关键发现（第 10 轮）** ✓：实例根本不走 `checkFunc` —— 检查 body 那一趟写着
`if (fx->tmpl) continue;   /* covered by the per-instance recheck */`（`check_top.c:5204` ✓），
而"per-instance recheck"只是**延迟检查**（算符 ✓ 方法 ✓ 可赋值性 ✓），**从未建帧、也从未替换
协议方法的类型** ✗。B7（`c.value()` 的类型还是 `T` ✗）与 F4（实例没有帧 ⇒ 生成非法 C ✗）
因此**同一个根** ✓。

**三块拼图（都已实测 ✓，还差最后一块 ✗）**：

1. ✓ `ast.h` 记 `coroRetProto`（写下来的 `coroutine<T>` ✓ —— 模板的 `ret` 会被改写成帧 ✓，
   实例是浅拷贝 ⇒ 必须能重建原型 ✓）；`funcInstance` 里重建原型 + 清空继承来的帧 + `isCoro=false` ✓。
   实测：帧名变成**实例自己的** `struct gen_i64$frame` ✓，B7 的调用也变成了
   `extc_coro_value_int64_t` ✓（`T` 替换成功 ✓）；
2. ✓ 让协程实例走 `checkFunc`：把 `if (fx->tmpl) continue;` 放宽成
   `if (fx->tmpl && !fx->tmpl->isCoro) continue;` ✓ —— prologue 就在里面 ✓，那里的注释本来就写着
   "检查泛型实例会把同一个 body 再走一遍" ✓（同一顺序 ⇒ 生成的 C 名不会漂移 ✓）；
3. ✗ **还差**：① 该帧结构没进**单元列表**（"用了没定义" ⇒ 早先那次"给帧收集补一趟 `g.funcs`"的
   尝试 ✗ 当时失败是因为帧还不存在 ✓，现在帧真的存在了 ✓ ⇒ 这两块可以合起来 ✓）；
   ② 协程**运行期文本**（`extc_coro` / `extc_task_*` ✓，来自 `src/coroutine.c` ✓）没被触发 ✓
   （`unknown type name 'extc_coro'` ✓）⇒ 要找到它的触发条件并让"有协程实例"也算数 ✓。

**第 11 轮：四块补丁一起落 ⇒ 只剩两个精确的阻碍** ✓（都已定位 ✓、都已回退 ✓）

- ✗ **F4**：`unknown type name 'extc_coro'` ⇒ 协程的 **typedef 与运行期文本**（`codegen.c:6496` 的
  `typedef struct ExtcCoroS extc_coro;` + `src/coroutine.c` 的 `extc_task_*`）**没被发出** ✓
  ⇒ 触发条件要找到并让"有协程实例"也算数 ✓；
- ✗ **B7**：`redefinition of 'struct gen$frame'` ⇒ 说明实例的 `coroFrameType` **仍然指向模板的帧** ✓
  （否则我的"帧单元也收实例"那趟不会把模板帧登记两遍 ✓）⇒ 也就是**建帧那段对这个实例没跑/没生效** ✓
  ⇒ 下一步应在 prologue 里打点确认：实例进来时 `isCoro` 是否为真 ✓、`f->coroFrameType` 是否真的被
  换成新帧 ✓。

**四块补丁（原型字段 ✓ 实例重建 ✓ 放行实例 ✓ 帧单元收实例 ✓）都已回退 ✓**，闸门保持 413/413 ✓；
两个阻碍各自只剩一处待查 ✓。

**正确的修法（原记，仍适用）**：帧合成要放进**实例的 body 检查路径**（就是"检查泛型实例会把同一个 body 再走一遍"
那条路径），而不是 `checkFunc` 的 prologue —— 让实例在那里建自己的 `<实例名>$frame`（字段按实参
替换），帧单元收集自然也就对了。两次试错都已回退，代码停在 `084303c`。

## 三点十一′、A4 **结案：是我套件自己的题写错了**（并据此更正前两轮的判断）

**结案实测**：套件里那版 A4（`b.v.v…v = a.v` + 读回 ✓）直接编 + `gcc -Werror` + ASan/UBSan 参数
⇒ gcc 退出码 **0**、运行退出码 1（套件判据里"非零退出"不算失败 ✓）⇒ **A4 通过** ✓。
重跑 `tools/attack.py generics` ⇒ **47 题剩 5 条** ✓（A4 已消失 ✓）。

**我错在哪**：那道题原本声明了 `b` 却**不使用**它 ✓ ⇒ `-Werror=unused-variable` ✓；你早先让我
"把题目改成会用 `b`" ✓，我改的是**套件里**那份 ✓，而我后来三轮一直拿 **`/tmp/a4.extc` 的旧副本**
做实验 ✗ ⇒ 于是看到的是"残骸 ⇒ `unused-value`" ✗ —— 那是**另一个**现象 ✓，与套件的 A4 无关 ✗。

**从旧副本里确实留下一条独立的小问题**（与本条分开、待排期）：当局部变量**真的没人用**时，
`dropUnusedLocals` 把这 415 字节的声明作为一刀删掉 ✓（探针实测区间正确 ✓），可最终产物里仍留下
一句裸的复合字面量 ✗ ⇒ `gcc -Werror` 报 `value computed is not used` ✗。它只在 `-Werror` 下可见 ✓、
只在"未被使用的深嵌套泛型局部变量"这一形状上出现 ✓，属于**质量类**（不是健全性 ✓）——
留档在案，不占主线。

## 三点十一、A4（旧副本的进一步观察，保留备查）：切点算对了，但最终产物仍有半截

**现象**（攻击套件 A4）：十层嵌套泛型的局部变量 `b` ⇒ 生成物里**声明消失、只剩一句裸的复合字面量**
⇒ `gcc -Werror` 报 `value computed is not used`（不带 `-Werror` 时只是警告，所以平时看不见）。

**逐层排除**（每一步都是实测，不是推理）：

| 观察 | 结论 |
|---|---|
| 关掉 `dropUnusedLocals` ⇒ 声明**完整正确**；开着 ⇒ 只剩初始化表达式 | 是本趟 pass 干的 |
| `DeadLocal` 记录：`b` 只有**一条**，`textlen=415`、**1 个换行**（单行）| 不是"登记两次"，也不是多行文本 |
| 切点：`dl` 找到；`[apply] func=main cuts=1` | 确实切了一刀 |
| 那一刀的实际区间：`start=11760 end=12175 len=415`，删掉的正是**完整声明** | 切点**算对了** |
| 但跳过关卡时，`main` 里**只有声明、没有裸表达式** | 残骸**不是** codegen 多发的 |

⇒ 结论：**切点按一份文本算、应用时落在另一份文本上**（偏移/文本不同步 —— 这一族在本轮里已是第三次
出现：② 的 `bl` 过期、H9、A4）。**下一轮的打点**：在应用切点**之后**，打印该区域内实际留下的字节
（以及"实际删掉的字节数" ✓），看它与 415 的差在哪里 ⇒ 然后照 ② 的成熟做法修：**应用前重新定位并
校验完整匹配，失配就跳过**（宁可不删 ✓ 绝不删半截 ✗）。

## 三点十二、F1（两段式，第一段已做通、第二段待做，故整体未落地）：`dyn` 打在泛型 impl 的实例上

**症状**（攻击套件 F1）：`` `pair_i64` does not implement `Tag` `` —— 而 `impl<T> Tag for pair<T>` 明明写了。

**第一段（检查器，已验证可行 ✓）**：`dynTraitOf`（`check_expr.c:435`）用
`ttEquals(im->target, pt)` 找实现，而泛型 impl 记录的 `target` 是**模板** `pair<T>`（`targs` 是块级
类型参数）⇒ 与实例 `pair<i64>` 永不相等。补一条"**同一个 `sdef` + 该 impl 有块级参数** ⇒ 视为覆盖
所有实例"（语义上正是 `impl<T> … for pair<T>` 的意思；每实例的一致性检查已在别处做过）⇒ 检查器
**放行** ✓。

**第二段（codegen，未做 ✗）**：放行之后生成物变成**非法 C** ✗ —— 表和 thunk 是按**模板**发的：

```
extc_vt$Tag$pair_i64   —— 使用处（对的）
extc_vt$Tag$pair_T     —— 定义处（模板名 ✗，于是"未声明"）
extc_th$Tag$pair_T$tag → 里面用 pair_tag((pair_T *)self)   ✗ pair_T 根本不存在
```

⇒ 需要让**泛型 impl 也按实例发表与 thunk**（表名用 `vtKey` 的实例名、thunk 里用实例的 C 类型）。
这与第 6/7 轮统一过的 `methodSetOf`/`vtKey` 是同一套机制 —— 那次统一了"表名从哪来"，这次要统一
"**泛型 impl 的实例也要各发一份**"。

**为什么不落地半段**：只改检查器会把"误拒"变成"生成非法 C" ✗（更糟）⇒ 已回退，两段一起做才算数。

**第二段的两次尝试（都回退，但把范围缩到最后一处）**：

- 尝试 1（已做通一半 ✓）：把 `for (… m->impls …)` 的循环头改成"泛型 impl ⇒ 对模板的**每个实例**
  各发一份表"（`g.insts` 里 `sdef` 相同的实例 ✓），循环体一行不动 ✓。结果：表名对了 ✓
  （`extc_vt$Tag$pair_i64` ✓，不再是 `pair_T` ✓），但 **thunk 体仍调 `pair_tag`** ✗；
- 尝试 2 ✗：把 `tsd = bt->mholder ? bt->mholder : bt->sdef` 换成权威 `methodSetOf(bt)` ⇒ **没变** ✗
  —— 因为 `pair<i64>` 的 `sdef` 本来就是模板 `pair` ✓，实例方法与模板同住一个声明 ✓ ⇒
  `methodSetOf` 返回的还是模板 ✓。

**于是范围缩到一处** ✓：thunk 里要的是**带实例限定的方法 C 名**（`pair_i64_tag` ✓，即第 6 轮
"按实例解析方法"那套命名 ✓），而 thunk 是**在任何函数上下文之外**写的 ⇒ 必须显式用实例上下文
求名（`cFuncName` + 实例替换 ✓），不能靠"当前函数"的隐式状态 ✓。

**尝试 3（找到了正确的着力点 ✓，但脚本改写的插入点算错 ⇒ 已回退 ✗）**：

- **关键发现** ✓：实例限定的方法 C 名由 **`g->ownerPrefix`** 决定 —— `cFuncName`（`codegen.c:569`）里
  就是 `if (g->ownerPrefix) return "<prefix>_<method>"` ✓（注释写着"a generic instance uses its own
  name" ✓）。所以在发 thunk 时把 `ownerPrefix` 临时设成实例的 C 名（`pair_i64` ✓），名字自然就是
  `pair_i64_tag` ✓ —— 一处 `保存/设置/恢复` 即可 ✓，不需要任何新命名逻辑 ✓；
- ✗ 失败原因纯属我自己的脚本：用"从替换段之后找第一个 `{`"来定位循环体末尾 ✓，可外层 `for` 的 `{`
  已经包含在替换段里 ✓ ⇒ 插入点落进了循环体内部 ⇒ 编译错（`expected ')' before ';'` ✓）⇒ 回退 ✓。
  **教训**：这种改写要在**原文**里用精确锚点（例如 `const char *vtKey = cType(&g, bt);` 与循环体末尾
  的 `bufPrintf(&vtDefs, "static const struct extc_vt$%s_t __attribute__…` 之后）手工定位 ✓，
  不要靠脚本猜括号 ✓。

**尝试 4（三步全部手工落地 ✓，差最后一块 ✗）**：用**逐字锚点切片**改写（不再猜括号 ✓）⇒

- ✓ 第①步（检查器放行）与第②步（按实例发表）**都成功**：表名 `extc_vt$Tag$pair_i64` ✓；
- ✓ 第③步（`g->ownerPrefix = vtKey` ✓ 保存/恢复 ✓）**也成功**：thunk 体变成了
  `return pair_i64_tag((pair_i64 *)self);` ✓ —— 名字正是实例限定的那个 ✓；
- ✗ **但 `pair_i64_tag` 从未被生成** ⇒ `implicit declaration of function 'pair_i64_tag'` ✗。

⇒ **最后一块** ✓：泛型 impl 的方法实例（`pair_i64_tag` ✓）只有在**被调用**时才由调用点造出来 ✓，
被 **vt 表引用**时没人造它 ✗ ⇒ 表发射这一趟必须**主动为该实例造/取方法实例**（走第 6 轮那套
"按实例解析方法"的入口 ✓，而不是直接用 `tsd->methods` 里的模板方法 ✓），这样 codegen 才会把它的
函数体也发出来 ✓。

**第 24 轮：第④步的原语仍未锁定（但排除法很有信息量 ✓）**

- 找到**一致性检查**的位置 ✓：`check_top.c:249-295`（`impl<T> Trait for pair<T>` 的签名核对 ✓）——
  它**只校验、不造实例** ✗；
- 打点 `funcInstance` ✓（`EXTC_FI=1` 打印模板名/宿主/参数数 ✓），跑一个**能编能跑的直接调用**
  程序（`p.tag()` ✓，生成物里确实有 `pair_i64_tag` ✓）⇒ **`funcInstance` 一次都没被调用** ✗
  ⇒ 方法实例是**另一个原语**造的 ✓；
- 候选位置：**持有者克隆**那段 ✓（`check_top.c:5081-5095` ✓，为"`impl` 指定一个**实例**"造
  `t->mholder` 并从声明抄一份方法 ✓）与注释里说的 "`resolveSignature` + `ttSubstitute` over
  `tt->instances`" ✓。

⇒ **结论** ✓：F1 缺的正是"**只被 vt 表引用、从没被调用**"时没人造方法实例 ✓ ⇒ 修法应在一致性
检查那一趟里，对**泛型 impl** 的每个目标实例主动物化一次方法实例 ✓（用的是上面那个原语 ✓）。
已经试过 4 次、都回退 ✓；这条是**功能缺口**（dyn × 泛型 impl ✓）不是健全性问题 ✓ ⇒ 按 goal 规矩
**留档、继续做别的** ✓（余下轮次给 B7+F4 ✓，它四块拼图已实测 ✓）。

**注意闸门** ✓（尝试 4 实测：三步落地后闸门**仍是绿的** 413/413 ✓ —— 语料里的 dyn+泛型 impl 组合
要么没走到这条路径、要么本来就正常 ✓；先前两次变红是我改坏了中间状态 ✗）：这条修到底后**可能**会让
`tools/golden.sh` 变红（**5 份产物不同 + 5 份非法 C** ✗，
`tests/dyn/dyn_stored_call.extc` 等 ✓）—— 说明语料里**确有**泛型 impl + dyn 的组合 ✓，所以这条修好
之后要**重设这几份基准** ✓（并在提交信息里写明"新增按实例的 vt 表" ✓）。

## 三点十三、B3/B10（已查清，未落地）：显式类型实参**接受但不喂推断** + 诊断推荐了无效写法

**实测三形状**（第 12 轮）：

| 写法 | 结果 |
|---|---|
| `maxOf<i32>(4, 3)`（注释里的官方例子）| **通过** ✓ —— 但那只是因为**推断本来就成功**（`T` 从实参得到 `i32`）✓ |
| `f<i64>(i64(1))`（`T` 只在返回类型出现）| ✗ `cannot infer type parameter T` |
| `zero<i64>()`（无实参）| ✗ 同上 |

⇒ 路径是有的 ✓：`EX_GENCALL`（`check_expr.c:1448` ✓）建好实例 ✓、把节点改写成 `EX_CALL` ✓，
**但没把写下来的类型实参带过去** ✗ ⇒ `EX_CALL` 从头再推断一次 ✓ ⇒ 显式实参成了**纯装饰** ✗。
而两处诊断偏偏写着 "write it explicitly: `f<i32>(...)`" ✗ —— **推荐的正是那个不工作的写法** ✓
（两处文本完全相同 ⇒ 改的时候要一起改 ✓；`s.count(...)==2` ✓）。

**两次尝试与结论** ✓：

- 在 `Expr` 的 call 成员里加 `targs`/`hasExplicitTargs` 字段 ✗ ⇒ **这个 union 不清零** ⇒
  普通调用节点读到**垃圾长度** ⇒ 误种子 ⇒ 语料 `tests/stl/sort.extc`（`sort::sort(x)`）
  当场报 "cannot infer" ✗ ⇒ 新字段在有初始化遍之前**不可用** ✓（这是本次最有价值的发现 ✓）；
- 改用**已初始化**的 `e->func`（改写时指向实例 ✓，其实例 `targs` 就是显式实参 ✓）读回 ✗ ⇒
  **没生效**（`f<i64>`/`zero<i64>()` 仍失败 ✓）⇒ 说明 `EX_CALL` 路径在种子之前**重新解析了 callee** ✓、
  把 `e->func` 覆盖掉了 ✓。

**下一步（顺序明确）** ✓：① 先让 `exprNew` **清零整个 union**（一处 ✓，受益于所有将来加的字段 ✓）；
② 再加字段并在改写的节点上置位 ✓；③ `EX_CALL` 的推断用它预填 `targs`（`unifyTParams` 对已填槽位
本就跳过 ✓ 见 `vecPush(NULL)` 那段的模式 ✓）；④ 两处诊断文案一起改成实情 ✓（写出来**不参与**推断 ✓，
给出真正能用的做法 ✓）。前三步都不动既有产物 ✓，第四步只改字符串 ✓。

## 三点十四、B3/B10 的**诊断**已落地（H11）：不再推荐一个不工作的写法

**落地内容**：两处逐字相同、**缩进不同**（32 空格 / 28 空格 ✓）的解释，一起改成实情：

```
A generic function's type parameters are inferred from its arguments. Writing them out is
accepted (`f<i32>(...)`) but does **not** seed the inference: give an argument whose type
mentions the parameter, or a typed variable to assign into.
```

**证据** ✓：`f<i64>(i64(1))` 与 `zero<i64>()` 现在都给出上面这段（旧文是
"…If one only appears in the return type, write it explicitly: `f<i32>(...)`" ✗ —— 推荐的正是那个
必然失败的写法 ✓）；`maxOf<i32>(4, 3)` 照旧通过 ✓（它靠推断成功 ✓，与显式实参无关 ✓）。

**验证** ✓：`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0** ✓（纯字符串改动 ⇒ 零输出变化 ✓）；
parrun ⇒ **292/0** ✓（回归 `tests/errors/explicit_targs_no_seed.extc` ✓）；`check.sh quick` ⇒
**41/0**、退出码 0 ✓；`make` 零诊断 ✓。

**仍然开着的那半**（让显式实参真去喂推断 ✓）留档在三点十三 ✓，顺序是：① 先把显式实参放进**检查器侧的
小表**（按节点索引 ✓ —— **不要碰 `Expr` 的字段** ✗：`exprNew` 虽然 `arenaAllocZero`，但 `sort::sort(x)`
那种**限定调用**的节点不是它造的 ✓ ⇒ 新字段仍可能是垃圾 ✓，上一轮已实测踩到 ✓）；② `EX_CALL` 推断时
用它预填 `targs` ✓；③ 两处注释（`check_expr.c:2000` 与 `:2026` 的 *docs 注释* ✓）也跟着对齐 ✓。

## 三点十五、新攻击组 coro（协程 × 池/arena）：16 题抓到两条

`tools/attack.py coro`：基本驱动 ✓ `for` 与显式驱动一致 ✓ 耗尽后 `next` 为 false ✓ 局部数组跨 yield ✓
suspend 前后写同一局部 ✓ 视图跨 yield ✓ 跨 yield 取局部地址（拒 ✓）✓ `send` ✓ 协程驱动协程 ✓
帧在循环里反复创建 ✓ 池对象共存 ✓ 池切片 ✓ 句柄存数组 ✓ —— 14/16 通过 ✓。

### K7 = 题目的期望写错了（已改 ✓）

`var p: mut ref i64 = alloc<i64>(1)` 在协程体内跨 `yield` ⇒ 检查器拒绝 ✓：
"`p` lives across a `yield` and carries a …" ✓ —— 这是**设计限制**（引用不跨 suspend ✓），
不是 bug ✓ ⇒ 题目期望改为 `reject` ✓。

### K4 = **两个真问题**（同一条用例）

**症状 1（已修 ✓ = H12）**：`main` 不写 `return` 时，生成物末尾是 **trap** ✗：

```c
    while (ids$step(&a)) { s = (s + a.ret); if (ids$step(&b)) { s = (s + b.ret); } }
    io$ostream_shl_slice_u8(…);
    extc_trapMsg("/tmp/k4suite.extc", 6, "a non-void function reached its end without returning");
```

而 C 约定 `main` 掉出末尾等价 `return 0` ✓，检查器里也写着"`main` 的返回类型可以缺省" ✓
⇒ **假 trap** ✗（程序白死 ✓）。修法二选一：codegen 对 `main` 发 `return 0;` ✓，或检查器直接要求
`main` 显式 return ✓（前者更贴近 C 与既有注释 ✓）。

**H12 修法** ✓（`codegen.c:4601`）：只有 `main` 走 C 的隐式规则 ⇒ 发 `return 0;` ✓；别的非 void 函数
掉出末尾**仍旧 trap** ✓（那里的值是真正未定义的 ✓，注释里那条"match 所有分支都 return"的理由仍然成立 ✓）。
**验证** ✓：K4 的 trap 消失 ✓、程序 `rc=0` ✓；生成物差异**恰好 13 份** ✓（都是"`main` 掉出末尾"的程序 ✓，
它们以前会在**退出时 trap** ✗）⇒ 已重设这 13 份基准 ✓（提交信息写明范围 ✓）；`tools/golden.sh` ⇒
**413/413 · 非法 C 0** ✓；回归 `tests/instimpl/t_main_falls_off.extc` ✓ ⇒ instimpl **17/0** ✓；
parrun **292/0** ✓。

**症状 2（撤回 ✗：是我算错了）**：当时记"交错驱动丢一个值 ✓，期望 436 实测 336" ✗ ——
第 16 轮逐步打印后看清：`10 → 110 → 121 → 222 → 234 → 336` ✓ **每一步都正确** ✓，
而 `10+100+11+101+12+102` **本来就等于 336** ✓（我把它心算成 436 ✗，多算了一个 100 ✗）。
`value()` 的幂等性也单独验过 ✓（`x=y=z=7` ✓）⇒ **K4 唯一的真 bug 就是那句假 trap** ✓，
H12 已整个修好 ✓：`tools/attack.py coro` 现在 **16/16** ✓。教训与 A4 同一条 ✓：
**结论要落在实测数字上，不靠心算** ✓。

**（原记录，保留备查）症状 2 的最初描述**：交错驱动**丢了一个值** ✗ —— 期望 `10+100+11+101+12+102 = 436` ✓，实测
**336** ✓（正好少一个 `100` ✓）⇒ 即第一次 `ids$step(&b)` 之后 `b.ret` 不是 100 ✗。生成物里两个帧
是**各自独立的局部 struct** ✓，`ids$step` 无状态 ✓ ⇒ 下一步：写一个每轮打印 `a.ret`/`b.ret` 的变体 ✓
把丢失点钉住 ✓。

**注意**：这两个症状都出现在**栈上（未装箱）表示** ✓（生成物里没有 `extc_task_*`/`extc_coro_*` ✓，
逃逸分析选了未装箱 ✓）—— 装箱路径是否也有同样问题另行验证 ✓。

## 三点十六、攻击组 modules（10 题）：**全绿** ✓ —— 这一面是稳的，但题面还不够狠

配套改了 harness ✓：`tools/attack.py` 的探针现在支持**多文件**（`src` 传 `dict`：文件名 → 源码 ✓），
每题先把工作目录清空（免得上一题的模块被 `use` 找到 ✓），入口取 `main.extc` ✓。这是打模块系统的前提 ✓。

**10 题结果**：基本跨模块调用与常量 ✓ `@private` 不可跨模块 ✓ 两个模块的同名私有/公开函数互不干扰 ✓
模块名与类型名撞车（mangling 家族 ✓ `pair_i64` 那条的模块版 ✓）✓ 跨模块共享泛型实例 ✓ 跨模块结构体与方法 ✓
循环 import ✓（**被拒** ✓ —— 不是挂死 ✓）✓ `use` 不存在的模块 ✓ 模块内的 `main` 不夺入口 ✓。

⇒ **0 条问题** ✓。这本身是有用信息 ✓（模块的可见性、mangling、循环 import 都守住了 ✓），
而且这 10 题已成为**常设判据** ✓。

### 加压批 13 题（共 23 题）：**23/23** ✓ —— 两条发现

跨模块 trait impl + `dyn` ✓ `@private` 类型从公开签名漏出（被拒 ✓）✓ 模块文件与同名子目录并存 ✓
跨模块类型闭环（被拒 ✓）✓ **模块名撞 `std` 模块名（`io.extc`）** ✓ 同一泛型在两模块各实例化 ✓
`@private` 泛型经公开包装跨模块用 ✓ 两个模块同名结构体各实例化 ✓ 模块函数遮蔽标准名 ✓
`@private` 常量经公开常量（被拒 ✓：`HIDDEN` 不可见 ✓）✓ 三模块链 ✓ 跨模块 impl 别处的 trait ✓
模块名形似关键字 ✓。

**发现 1（题目自己的错 ✓）**：`dyn` 收**无限定**的 trait 名 ✓ —— `use tag` 就把 `Tag` 带进作用域 ✓，
`dyn tag::Tag(t)` **不是合法语法** ✗（题目踩过 ✓，已改 ✓）。
**发现 2（小瑕疵 ✓，非健全性）**：`use tag::Tag`（想按限定名导入 trait ✓）报的是
"type name `Tag` must start with a lowercase letter" ✗ —— 对 trait 导入来说措辞不对 ✓，
留档不占主线 ✓。

**下一批加压方向**（已记 ✓）：跨模块 trait impl + `dyn` ✓、`@private` 类型
从公开函数签名里漏出去 ✓、模块文件与子目录同名 ✓、`use` 一个自身也 `use` 回头的类型闭环 ✓、
模块名与 `std` 模块名撞车 ✓。

## 三点十七、攻击组 views（视图与切片，16 题）：**全绿** ✓

覆盖：基本切片读写与长度 ✓ mut 与只读视图**同一个 C 结构**（第 6 轮统一 `unitFind` 的那条 ✓）✓
空切片（长度 0、不 trap、不越界读 ✓）✓ **越界索引带位置 trap** ✓ 元素是结构体（字段可写 ✓）✓
元素是泛型实例 ✓ 切片从函数返回（全局数组 ✓ 拒局部 ✓）✓ `new i64[4]` 得到可写切片 ✓
`[4]i64` 当 `slice<u8>` 重解释（拒 ✓）✓ 同一数组两个可写视图 ✓ 嵌套视图 ✓
字符串字面量取可写视图（拒 ✓）✓ 结构体里存视图（按逃逸规则拒 ✓）✓ 视图传进协程 ✓ 池切片 ✓
`for` 遍历与下标循环结果一致 ✓。

⇒ **0 条问题** ✓（三组新面 coro / modules / views 里，只有 coro 抓到了真 bug ✓）。
这 16 题已成为常设判据 ✓。**下一批加压方向**：视图的逃逸级别（全局数组 ✓ 局部 ✓ 帧内 ✓
池内 ✓ 四档各写一题 ✓）、对视图元素做 `ref` 再跨 suspend ✓、`slice<slice<T>>` 的写穿 ✓。

## 三点十八、攻击组 dyn（16 题）：**H13 修掉一个编译器段错误** ✓ + 一条待查

### H13（已落地 ✓）：无接收者的方法经 `dyn` 调用 ⇒ **编译器 SIGSEGV**

**触发**（`tools/attack.py dyn` D2 ✓，六行）：

```extc
trait Bad { fn nope() -> i64 }              /* 没有接收者 */
struct s { v: i64 }
impl Bad for s { fn nope() -> i64 { return i64(1) } }
… var d: dyn Bad = dyn Bad(a)
  return i32(d.nope())                      /* extc 段错误 ✗ */
```

**gdb** ✓：`genMethodCall`（`codegen.c:1803` ✓）里 `Param *p0 = *(Param **)vecAt(&f->params, 0);`
—— 方法**没有参数**（连 `self` 都没有 ✓）⇒ 索引空表 ⇒ 崩溃 ✓。

**对象安全本来就要拦它** ✓：检查器的注释（`dynTraitOf` ✓）写着"no receiver、generic、返回 `Self`"三类 ✓，
而"返回 `Self`"那半**早就拒了** ✓（D3 ✓）—— 缺的就是"no receiver" ✓。

**修法** ✓（两处 ✓）：① 检查器在 dyn 调用解析出 trait 方法之后判 `want->params.len == 0` ⇒
报 `` `%s` has no receiver, so it cannot be called through `dyn` `` ✓（并说明"dyn 走统一表、每项都要接收者"✓，
或改写成自由函数 ✓）；② 生成器加一句 `if (f->params.len == 0) return "0";` 兜底 ✓
（诊断留在检查器 ✓，注释写明这是防御 ✓）。

**验证** ✓：D2 现在退出码 1 + 上面的报错 ✓；`tools/attack.py dyn` **16 题剩 1 条** ✓；
`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0** ✓（零输出变化 ⇒ 只新增拒绝 ✓）；回归
`tests/errors/dyn_no_receiver.extc` ✓；parrun ✓；`make` 零诊断 ✓。

### D12 结案 ✓：是**我题目的期望写错了** —— `dyn` **复制**载荷，本来就安全

判据三条 ✓：

1. 生成物是 `extc_dyn_put((const void *)&__extc_dyp0, (int64_t)sizeof(s), &extc_vt$Tag$s)` ✓ ⇒
   载荷被**复制**进池存储 ✓（不是留一个指向局部变量的指针 ✓）；
2. ASan/UBSan 下运行**零报告** ✓（无 use-after-free ✓）；
3. 更狠的形状也被**正确挡住** ✓：载荷里含**视图**（`struct holder { s: slice<i64>  v: i64 }` ✓）
   且局部先死 ⇒ 检查器报 "this return value would hold a reference to a local variable that dies
   first" ✓ —— **逃逸分析**在起作用 ✓（这条才是真正的攻击面 ✓，它守住了 ✓）。

⇒ 题目期望已从 `reject` 改成 `ok` ✓，`tools/attack.py dyn` 现在 **16/16** ✓。

## 三点十九、攻击组 arena（arena / 逃逸，12 题）：**全绿** ✓

**策略** ✓：`tests/arena-soundness/` 已经收录了一批**已知**的逃逸漏洞 ✓（如 `B_field_table_stale.extc`：
"字段表过期 + 引用型结构体的值 ⇒ 让 arena 内存逃出函数" ✗）⇒ 这一组**避开**已收录的族 ✓，
专打"按级别规则**应当安全**"的相邻形状 ✓ —— 真漏了才是新发现 ✓。判据是硬 oracle ✓（ASan/UBSan 零报告 ✓）。

**结果**：局部 `new` 在块内用 ✓ `new` 值从函数返回（提升 ✓）✓ 跨两层调用提升 ✓ `new` 切片从函数返回 ✓
循环里分配并保留最后一次 ✓ 泛型返回 `new` 值 ✓ 结构体装 `new` 值再返回 ✓ 辅助函数里的 `alloc`
用于外层 ✓ 协程里持有 `new` 切片跨 yield ✓ `alloc` 计数为 0 ✓ 嵌套结构体装 `new` 值 ✓
两层容器返回 ✓ ⇒ **12/12** ✓。

**顺带确认一条语言规则** ✓（题目踩过一次 ✓）：含**引用/视图**的结构体**不能零初始化** ✓ ——
`var h: holder`（`holder { v: mut ref i64 }`）报 "cannot zero-initialize `h`: it contains a reference" ✓
（零值引用会是 NULL ✓，直接拒绝是对的 ✓）⇒ 要用字面量一次写全 ✓
（`var h: holder = holder { v: new i64 }` ✓）。

## 四、事故：fuzz 产物把 /tmp 写满，连带把工具链卡死（2026-09-28，round 9）

**现象**：`/tmp` 写满（`ENOSPC`）⇒ **bash 工具起不来**（它的暂存也在 `/tmp`）⇒ `rm`/`df`/`grep`
一类要起外部进程的工具全部失效 ⇒ **想删东西却需要一个 shell**，先有鸡还是先有蛋。
只读的 `Read` 与写 **home** 的 `Write` 仍然可用（两个文件系统不同：`/tmp` 是 tmpfs，home 是 ext4）。

**原因**：`/proc/mounts` 显示 `/tmp` 是 **tmpfs（内存盘）**（`tmpfs /tmp tmpfs rw,...`，没有
`size=` ⇒ 默认约一半内存）。而 fuzz 每次迭代都要落 `case.extc` / `case.c` / **ASan 可执行文件**
（约 1 MB），一轮战役几百到几千次迭代，三种模式 × 多种子叠加几轮就足以吃掉内存盘。

**已经做的改正**：

1. `tools/fuzz.py` 的 `--out` 默认值由 `/tmp/extc-fuzz` 改为 **`~/extc-fuzz`**
   （`os.path.expanduser`），模块文档里写明"**不要放在 /tmp**"及原因；
2. 新增 **`tools/clean-tmp.sh`**：一键清掉 `/tmp` 下 fuzz/参考/工作树/探针产物并报告释放量，
   `--dry-run` 只看不删；只碰 `/tmp` 下的已知前缀，不进仓库。

**约定**（免得下次再踩）：**fuzz 产物、参考副本、隔离构建的 worktree 一律放 `~/extc-work/`**
（仓库之外的 home 目录）；`/tmp` 只用来放编译器中间文件那种短命对象。

**当时的进展状态**：战役 3（三模式 × 三种子 = 2250 次迭代）已经跑完，结果还**没有读出来**
（日志 `/tmp/fuzz3.log` 与产物 `/tmp/fuzz3` 都在，清 `/tmp` 之前可以先 `tools/fuzz-triage.py
/tmp/fuzz3` 分诊；若要保留就先复制到 `~/extc-work/`）。编译器本体停在与 `64e3c3b`/`a26b8ad`
一致的状态（② 已修、基准 413/413 且 0 份非法 C）。恢复后的第一件事：分诊战役 3，然后按
H 系列套路继续。


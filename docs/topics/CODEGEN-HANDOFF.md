# CodeGen 重构交接（2026-10-05）—— 从这一步开始

> **这是给下一个对话的单一入口。** 读完本文即可开工，不必回看长上下文。
> 配套（按深度递进）：
> `docs/reviews/CODEGEN-2026-10-05.md`（BG-1 bug 记录 + 维护性四条）
> `docs/topics/CODEGEN-ARCH.md`（现状怎么工作 + 质疑前提 Q1–Q7 + E1 实测）
> `docs/topics/CODEGEN-WORKLIST.md`（本方案的完整版，含 6 问答案与子步细节）

## 0. 一句话

**把"先全发、再按文本删"改成"按需发射（worklist）"** —— 目标是**在发射之前决定谁存在**，
从而让生成的 C 里**不可能有死代码**，并让那 ~600–700 行文本级删除逻辑退场。

## 1. 前提（用户拍板，不要再讨论"要不要"）

1. **编译到 C：保留** ✓
2. **多文件输出：可以接受，而且期待** ✓（"一个 TU、全 static"不是硬要求）
3. **生成的 C 里不能有死代码** ✓ —— 理由是**编译期压力**：`-ffunction-sections` + `--gc-sections`
   只省**链接期**，gcc 在 `-O2` 下照样把每个死函数解析/优化/发射一遍 ✗ ⇒ **工具链 DCE 不够**。

## 2. 已测数据（全部实测，别重测）

**M1 死代码现状**：414 份产物 · **6917 个 static 函数 · 没人调用的 0 个** ✓
⇒ 前提今天**是被满足的**；重构的理由不是"现在有很多死代码"，而是"**保住这个 0 的机制是文本级的**
（成本随产物增长、可靠性不增长）" ✓

**M2 大小与编译期代价**（`gcc -std=c11 -O2 -c`，best of 3）：

| 产物 | 生成 C | gcc -O2 |
|---|---|---|
| 空程序 | 2,871 B | 0.01s（= 每份产物的支撑地板）|
| `examples/alloc-in-block` | 12,818 B | 0.03s |
| `tests/coro/coro_sched` | 86,382 B | 0.23s |
| **`qqbot-extc/examples/wss-echo`（TLS+WS）** | **157,660 B** | **0.47s** |

**≈3 ms/KB 线性** ⇒ 完整机器人（估 400–800 KB）每次 1.2–2.5s，而今天**每份产物重编同一份运行时** ✗

**W0 基线（预言机读数）**：414 份产物共 **13,923 条删除**
= `prim` **8,332** + `func` **5,466** + `scoped` **125**；**0 份产物完全没有死代码** ✓

## 3. 方案：W0–W4（每步同一个三元组判生死）

| 步 | 做什么 | 期望指纹 | 预言机读数 |
|---|---|---|---|
| **W0 ✅ 已落** | 三个删除点按 `EXTC_DBG_DCE` 打一行（零行为变化）| — | **13,923**（基线）|
| **W1 ✅ 已落** | 登记 API：发射实体时就地登记 `{name, kind, block, span}`；删除端**优先用登记信息**，行形识别降级兜底 | **不变** ✓ | 不变 ✓ |
| **W2a ✅ 已落** | 原语块**按需发射**：闭包在块进产物**之前**决定谁存在；旧 pass 变预言机（块内必须 0）+ 兜底 | **不变** ✓ | **块内 0** ✓（`[wl]` 8,323）|
| **W2b-1 ✅ 已落** | BG-1 修好：`needPrint` 与 `needRuntime` 分开 ⇒ `extc_print` 按需；摘掉 `unused` 属性 | **351 份变**（全是删死机器，见 §4.4）| 不变（0）|
| **W2b-2** | pool 运行期 + 其余 `need*` 旗子进闭包 | 见 §4.5 | `prim-out` → 0 |
| **W3** | worklist 覆盖**函数/实例/描述符** ⇒ 删除 pass 的函数段、描述符段退场 | **不变** | `func` → 0 |
| **W4** | 删掉 `dropRuntimeDefs` 的行形识别与 anchor 技巧（`dropUnusedLocals` **留着**）| **不变** | 全 0 |

**三元组**：`414 份指纹 diff`（`/tmp/leadchk/fp-before.sha256` 是基准，**建议先重取一份放进仓库**）·
`EXTC_DBG_DCE=1` 的删除条数 · `gcc -O2` 时间与字节数。

## 4. 零回退的两个手法（本方案的精髓）

1. **旧机制当预言机**：W1–W3 期间**保留**删除 pass，只把删除条数打印出来。按需发射真做对了 ⇒
   条数**掉到 0** ⇒ "新机制是否生效"是一个**可观测的数字**，不用赌 ✓
2. **双路径并存 + 单开关**：W1/W2 保留旧路径，任一步指纹不符就单开关回退 ✓

### 4.1 W1 已落（2026-10-05）：登记 API + 兜底

* **块变成表**：运行时原语块原来是 4 个字符串字面量；现在是 `src/back/codegen.c` 里的
  `PRIM_BLOCK[]`——**61 行**（**34 个定义** + 27 段脚手架：注释、空行、`#if/#else/#endif` 骨架）。
  逐字节与原块相同（生成器 round-trip 校验 9,675 B ✓，全语料指纹为证）。
* **登记**：`primEmit` 一边发一边登记 `PrimEnt{name, kind, off, span}`（`primEnts`）。
  宏行的名字从它自己的 `#define` 行读出（表里不写裸 `EXTC_*` 字面量：`tools/check_switches.py`
  把任何这种字面量当编译器开关）。
* **删除端**：`dropPrimReg` 用登记的 span 做与旧扫描**同一个判据**（全篇提及数 == span 内提及数；
  宏另按 `#ifdef` guard 不计入、`#define` 行算自身）。旧扫描 `dropRuntimeDefs` **降级为兜底**，
  仍然运行；`EXTC_DBG_PRIMSCAN=1` 跳过登记路径 ⇒ **单开关回退** ✓（已在 `--help` 里点名）。
* **读数**：**414/414 指纹逐字节不变** ✓ · `EXTC_DBG_DCE` **13,923 条不变** ✓ ·
  `./check.sh quick` **72/0** ✓ · `tools/golden.sh` 414 份全是合法 C ✓。
* **`__extc_dying`**：原来是块内一行 + 手工 `DeadDef` 登记（它有括号，行形扫描看不见它）。
  现在它是块里的一个普通 `PRIM_VAR` 实体；`quiet=1` 使它不计入 `[dce] prim`，好让读数与 8,332 基线**可比**。

### 4.2 W1 挖出来的坑（**W2 必须先处理**）

**旧扫描的窗口会越过块尾**：`dropRuntimeDefs` 的 `rl` 每次外层重启都重置为 `rl0 = strlen(primText)`，
而块在被裁剪后变短 ⇒ 窗口 `[rp, rp+rl0)` **伸进块后面的文本**。实测后果：它顺手删掉了
pool 运行期头部的 `extc_pool_capacity`（全语料 **2 份**产物：`tests/stl/clone_warn.extc`、
`tests/pool/rt_stale_alias.extc`）——**这是巧合，不是设计**（伸进去多少取决于块被裁掉多少）。

登记式删除把范围**限定在块内**（这才是这个 pass 一直以来的本意），于是那 2 份会多留一个死函数。
为了 W1 指纹不变，**旧扫描作为兜底保留**并由它继续负责那 2 份 ✓。

⇒ **W2 的入口条件**：要让 `prim → 0`，必须先把 **pool 运行期**（`poolsEmitRuntime`，`src/back/pools.c`，
14 个大字面量）也纳入登记/闭包，否则这 2 份产物要么带死代码、要么指纹变（二选一）。
W2 的验收必须**逐条解释**这 2 份的变化。

### 4.3 W2a 已落（2026-10-05）：原语块按需发射 —— 块内删除 → **0**

**做法**（这是方案 §0 那句话的落点：「在发射之前决定谁存在」）：

* 块在 `primEmit` 里组装到**自己的缓冲区**（`g.prim`），**不写进 `out`** ⇒ 它还不是产物的一部分。
* 8 轮 `dropUnreferenced` 收敛后，`primSplice` 做**需求闭包**：在「还没有这个块」的成品文本 +
  块的脚手架 + 还活着的定义之间迭代到不动点。判据与旧 pass **同一个**（名字的提及全在自己这段里 ⇒ 删；
  宏另按 `#ifdef` guard 不算使用、`#define` 行算自身），但它是**问登记表**，不是猜行形。
* 脚手架（注释、空行、`#if/#else/#endif`）**永远发** ⇒ 丢弃一个定义后留下的字节与旧 pass 完全一样。
* 然后把块**拼进 `primA`**（协程运行期也拼在这个偏移，所以块落在它前面 —— 原来的顺序）。
* 旧的行形 pass 随后跑**一次**：**预言机**（块内必须删 0 条）+ **兜底**（§4.2 的越界）。

| 读数（414 份）| 值 |
|---|---|
| 指纹 vs `tools/codegen-baseline-2026-10-05.sha256` | **414/414 逐字节不变** ✓ |
| `[wl] prim`（闭包排除、**根本没进产物**）| **8,323** |
| `[dce] prim`（旧 pass 在**块内**还删掉的）| **0** ✓ ← W2 的承诺 |
| `[dce] prim-out`（旧 pass 在块外删掉的）| **2**（`tests/stl/clone_warn` / `tests/pool/rt_stale_alias` 各 1，即 §4.2 的 pool 头部 `extc_pool_capacity`）|
| `[dce] func` / `[dce] scoped` | 5,466 / 125 **不变** |
| `./check.sh quick` | **72/0** ✓ |
| 编译器自身耗时（414 份轮询，best of 3）| HEAD 4.59s → **4.46s**（略快：块发得更短）|

**计数账**（旧账 8,332 去哪了）：8,323（`[wl]`）+ 2（`prim-out`）+ **7**（`extc_desc_text` 这 7 份描述符行
现在由**顶层 DeadDef 那一遍**删掉 —— 以前越界的 prim pass 抢先删了它；产物逐字节相同，`[dce] func/scoped` 不变）。
⇒ 「旧机制闲置」有了准确读数：**块内归零**，剩下的 2 条是它的越界。

**回退开关**：`EXTC_DBG_PRIMSCAN=1` 恢复**完全旧**的管线（块直接进产物 + 行形 pass 在轮内跑 +
`__extc_dying` 的手工 `DeadDef` 登记）⇒ 指纹同样 **414/414 不变** ✓（两条路都验过）。

**W2a 顺手修掉的真 bug（闸门抓到的）**：`dropRuntimeDefs` 的窗口是 `strlen(primText)`
（**捕获时的**长度）而从 anchor 量起，块被裁短后窗口会**读越过产物末尾**。以前块的裁剪是它自己做的、
删除与窗口同步收缩，越界不深；W2 把「裁短」交给了 `primSplice`，越界一下大了几千字节 ⇒
`memchr` 里 SIGSEGV（复现：把 gate 用例的**绝对路径**喂进去，`build/gate/diff/f_neg_zero/t.extc`）。
修法：窗口按 `text + len` 夹一次（越界处本来也没有可删的东西，`span` 检查早就拦着）。
这是审计 §7 闸门③（差分 fuzz）抓到的，不是判据覆盖率问题 —— 是**真崩**。

### 4.4 W2b-1 已落（2026-10-05）：BG-1 修好 —— `rtPrint` 独立按需

**`needRuntime` 拆成两个**：`needRuntime` = 「要那张描述符表（`rt`）」，`needPrint` = 「真的调用了
`extc_print`（`rtPrint`）」。`needPrint` 只在**两个**调用点置位（`genPrint` 的结构化分支、
`?` 在 `main` 里失败时印载荷）；`trap(msg)` 那处**整条删掉**（它只用 `extc_trapMsg`/`extc_die`，
两个都在原语块里，跟描述符表无关 —— **那正是 BG-1 的根因**）。于是：
`extc_print` 只在有人调用时发射 ⇒ `static __attribute__((unused))` **摘掉** ✓（评审文档 §8 的准入条件达成）。

| 读数 | 值 |
|---|---|
| 生成物总字节（414 份）| 13,734,285 → **12,957,327**（**−777 KB，−5%**；按 M2 的 3ms/KB ≈ 省 2.3s gcc）|
| 指纹 | **351 份变**（63 份不变）—— 已重取基线 `tools/codegen-baseline-2026-10-05-w2.sha256` |
| 变化性质（逐份差分核过）| 351 份**全是纯删除**（没人调用的描述符表 + `extc_print`）；其中 **184 份**另有一行变化：`static __attribute__((unused)) void extc_print` → `static void extc_print`（那份**真的**打印结构化类型，属性本来就该摘）|
| `tools/gate_checkc.py --scope full` | **1,035 份 gcc+clang `-c -O2`：失败 0** ✓ |
| `./check.sh quick` | **72/0** ✓ |
| `[wl] prim` / `[dce] prim` / `prim-out` / `func` / `scoped` | 8,323 / **0** / 2 / 5,466 / 125 **不变** |

⇒ 这正是 W2 行里说的"只允许**旗子判错**的产物变"：变的就是 `trap(msg)` 把整张描述符表连带拖出来的那 351 份。
**新基线**：`tools/codegen-baseline-2026-10-05-w2.sha256`（W1/W2a 期间的对照基线是
`tools/codegen-baseline-2026-10-05.sha256`，留在仓库里当历史）。

### 4.5 下一步（W2b-2 / W3）

1. **pool 运行期**纳入登记/闭包（`[dce] prim-out` = 2 ⇒ 只剩它的头部在旧 pass 手里）。注意：**整块**纳入后
   `tests/stl/clone_warn` 这类产物会多掉 **11 个**没人调用的 `extc_pool_*`（实测），那是**对的**，
   但要逐条解释、要重取基线。
2. **其余 14 个 `need*` 旗子**换成需求边（BG-1 已经用同样的手法修好一格，剩下的照做）。
3. **W3**：worklist 覆盖函数/实例/描述符（`func` → 0）。

## 5. 动手前的问题——**全部已答**（读代码所得，别再猜）

| 问题 | 答案 |
|---|---|
| **入口集合** | `main`（`cgIsMain`）· **`@export` 函数**（外部链接）· **被 `dyn` 表点名的函数** · **模块级全局的初始化式**引用到的实体 |
| **原型/定义** | **分两遍发射、最后拼接**（`codegen.c:8279` 注释）⇒ worklist **两遍都要管** |
| **描述符** | **已是按需的** ✓（`descRef` 938 → `genStructDesc` 1092/1100）⇒ 只需登记引用边 |
| **"全发"在哪** | `m->funcs` 的三处循环（4688/4710/4721）—— worklist 要替掉的就是它 |
| **发射顺序** | 四段：原型/typedef → 描述符 → **body pool** → 收尾拼接（运行时块 + dyn 表）⇒ **只段内自由** |
| **⚠️ 危险格** | **`dyn` 表是"删除 pass 之后才拼接"的**（6561 显式跳过被它点名的函数）⇒ **必须当根**，否则**删活代码** ✗ |
| **模块状态** | **没有模块 init 函数** ⇒ 模块级 `let` 即 C 全局，其初始化式是引用边 |

## 6. 与 AST/parser 的对接（硬规则）

* **codegen 只读 AST** ✓ —— 写任何 AST 节点成员会撞 **AST 冻结闸门** ✗；
* **新事实进 `plan` 侧表** ✓（先例：`namedByDynTable = planDynTable(f)`，`codegen.c:5849`）；
* **worklist 是 codegen 内部结构** ⇒ **不需要动 AST / plan / parser** ✓；
* 若某事实要跨 pass 存活：**进 plan 侧表，不进 AST 字段** ✗。

## 7. 验证配方

```sh
# ① 行为不变（纯重构步）：414 份产物哈希 diff
cd ~/extC_Compiler
#    基线：W1/W2a 期间用 tools/codegen-baseline-2026-10-05.sha256；
#          W2b-1 起（BG-1 有意改产物）用 tools/codegen-baseline-2026-10-05-w2.sha256
while read -r h f; do ./build/extc -w --no-line-map -o /tmp/o.c "$f" 2>/dev/null && \
  printf '%s %s\n' "$(sha256sum /tmp/o.c|cut -d' ' -f1)" "$f"; done < tools/codegen-baseline-2026-10-05.sha256 > /tmp/fp-new.txt
diff /tmp/leadchk/fp-before.sha256 /tmp/fp-new.txt | head

# ①b 单开关回退（W2a 的登记路径整段关掉 ⇒ 回到 W1 行为，指纹应与上面的基线一致）
#    EXTC_DBG_PRIMSCAN=1 <同上>

# ② 预言机读数（W2a 之后：`[dce] prim` 必须是 0；`[wl] prim` 是新机制的数字）
EXTC_DBG_DCE=1 ./build/extc -w --no-line-map -o /tmp/o.c <产物> 2>&1 | grep -E '^\[(wl|dce)\]'

# ③ 闸门（安全网，不是质量依据）
./check.sh quick          # 期望 72/0
python3 tools/gate_checkc.py --scope full   # 全语料 gcc+clang -c -O2（改了发射内容时必跑）
tools/golden.sh --strict-bytes
```

## 8. 本轮**不做**（边界）

* 不动 **C 发射层**（表达式/语句/描述符的印法）—— 它是好的（可读、确定性、双编译器 `-Werror` 干净）；
* 不动 **`dropUnusedLocals`**（335 行，**活函数内部**的局部/载荷绑定，按需发射帮不上）⇒ **另立项**；
* 不去"精确化"`need*` 旗子的语义（注释写明漏发=生成物坏掉 ✗）—— 是**换机制**，不是调参；
* **BG-1 的 `__attribute__((unused))`**（`codegen.c` 里 `extc_print` 定义处）在 **W2 完成后**才能删。

## 9. 记账（已知的坑与教训，别重踩）

1. **判据的覆盖率不可知** ⇒ 判据只能当安全网，**质量看代码是否好维护**（用户 2026-10-05 的纠正）；
2. **工具教训**：判据前先 `make`；**退出码不要过管道**（`cmd | head; echo $?` 拿到的是 `head` 的）；
   这两条曾让一个**不存在**的 bug 被写进文档 ✗；
3. **往被广泛导入的 stdlib 模块加 helper 会牵动发射判定**（BG-1 就是这么炸的）；
   `tests/coro` / `tests/pool` 是"只导入、不打印结构化类型"的哨兵 ✓
4. **`static inline` 不是"未使用"的通用解**：GCC 认，Clang 报 `-Wunneeded-internal-declaration` ✗；
   `__attribute__((unused))` 两边都认 ✓；
5. **审计 §0.2/§0.3 已经写下本方案的等价物**（手术 B「发射需求闭包」/ C「命名单一来源」；
   R2/R7 双份真源；R13 粒度建模错）⇒ **不是新发明，是执行已写的契约**。

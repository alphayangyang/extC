# P1-8 分配点 / 屏障微优化：取证、实现与实测

> 任务：task-19（P1-8）。写域：`src/back/codegen.c`、本文件。不改其他源码、不 commit。
> 硬约束（用户经 lead 转述）：**不许靠抬 r（thr 倍数）或抬 M_max 换性能；内存只能降不能升**。
> 本文所有 Ir 均为 `valgrind --tool=callgrind` 的 `PROGRAM TOTALS`（确定性、与机器负载无关）。

## 0. 结论摘要

1. **第 (1) 项 extc_modI 是负结果**：`-O2` 下常量除数的取模已被 GCC 折成 magic 乘（`imul/ror/cmp`），
   四个 bench 形状 + gate 形状里剩下的 `idiv` **全部**落在运行期文本自身的除法（`extc_rc_pressure` 的 `/8`、
   `thrUpd` 的 `/4`），没有一条来自 `extc_modI` 调用点。**不改**。
2. **第 (2) 项**：清扫档（`EXTC_RC_NOCOUNT=1`，四个 bench 与 gate 全在此档）`extc_rc_initRef` 实际**不发任何有效代码**
   （shade 被 `#if` 编掉、自赋值是死存、`inc` 是空函数）⇒ 0 Ir，无可删。事件档（TIER 1）的 shade 运行期是死代码
   （`EXTC_RC_SWEEP_FREE=1` ⇒ `extc_rc_shouldSweep` 编译期 0 ⇒ `extc_rc_begin` 不可达 ⇒ `extc_rc_wb` 恒 0），
   但 GCC 证不出（`begin` 里 `r->marking=1`/`wbAdd` 在代码上可达）⇒ 每次 shade 仍付 ~6 Ir。
   **唯一保留改动**：事件档把 `initRef` 换成 `extc_rc_inc`（§3、§5），实测 promote 事件档 −1.09%。
3. **第 (3) 项**：`storeRefOwnedReg` 的「17 Ir」**不可复现**。计数档每次赋值的代价是 ~149 Ir，但 90% 在运行期函数
   （`holdUpdSlab` ×2 ≈105、`ownSlab` ×2 ≈38）里；清扫档屏障本身只有 **10.0 Ir/赋值**（同 runtime 文本 A/B，§4）。
   **codegen 侧没有杠杆**，不动。
4. 本补丁对 **TIER 2 / TIER 3 单位与无 rc 单位逐字节零差异**（§6.1 逐项 cmp），所以 bench / gate 判据在构造上不受影响；
   事件档唯一差异是 `initRef` → `inc` 一行。
5. 给 runtime-facts 的建议（更大杠杆、3 倍收益）：`#if EXTC_RC_SWEEP_FREE` 下把 `extc_rc_shade`/`extc_rc_shadeOld`
   整体 no-op（§3.5 实测）。若落地，本补丁变成冗余但无害。

## 1. 口径与环境

| 项 | 值 |
|---|---|
| FREEZE v4 `src/back/codegen.c` | md5 `cf2885440981b5c9d154927fb61441b9`，15994 行 |
| FREEZE v4 `src/back/reclaim.c` | md5 `ff42799bfa62c2c9b0ac5b238e7d05e9`，3607 行 |
| FREEZE v4 `tests/reclaim/driver.c` | md5 `9aae9cb59ff28ef4674c7c1690702d8e`，1216 行 |
| 工作树 `src/back/codegen.c`（补丁后） | md5 `d339382889b87c0dd18b357c69e2e779`，16017 行 |
| 工作树 `src/back/reclaim.c` | **在途**（runtime-facts 正在改；测量期间 md5 从 `d8af3294…` 漂到 `adb8c58c…`，3666 行），故绝对 Ir 有污染 |
| 我的构建 | `make BUILDDIR=build-cf -j 8` ⇒ `build-cf/extc` md5 `ec65092378265035342d35fa9bbf291d`，0 warning |
| A/B 基线 | `/tmp/mine2-src`：把本补丁的条件改成 `if (0)`（只差一行），`make BUILDDIR=build-mine2` ⇒ `/tmp/mine2-src/build-mine2/extc`；两编译器共用同一份在途 `reclaim.c` |
| 生成 | `build-cf/extc -w --no-line-map -I stdlib <src> -o x.c` |
| 编译 | `cc -O2 -g -D_POSIX_C_SOURCE=200809L`（bench 墙钟同形；gate 用 `-O1 -fwrapv`） |
| 工具 | `valgrind --tool=callgrind`、`callgrind_annotate`、`objdump -d`、`taskset -c 6` |

**A/B 方法学**：两编译器由同一 src 生成，唯一差异是补丁那一行条件；生成物用同一份运行期文本 ⇒ 差值是补丁的净效应。
另有一类对照（§3.5 的 shade-off）是在**已生成的 C** 里把运行期函数体改成 no-op（单行插入，保留同行语义），
用来估计「运行期侧闸门」的上界；每处对照都核对了 stdout sha256 一致。

## 2. 第 (1) 项：`extc_modI` 的常量除数已被 GCC 强度削减（负结果）

发射侧（名称选择，未改）：`src/back/codegen.c:2007`：

```c
(sint ? "extc_modI" : "extc_modU")
```

运行期定义：`src/back/codegen.c:12364`（FREEZE v4 口径）：

```c
static inline int64_t extc_modI(int64_t a, int64_t b, const char *f, int l) {
    if (b == 0) extc_trapMsg(f, l, "division by zero");
    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, "integer overflow in division");
    return a % b;
}
```

调用点形态（生成物）：`extc_modI((int64_t)(v), (int64_t)(100), "…", 行号)`。

`objdump -d` 统计（`-O2`，四个 bench 形状）：`idiv` 命中 **promote 5 / cycleratio 4 / sizeratio 5 / chase 4**，
逐条核对归属后**全部**落在运行期文本：`/tmp/p18/promote.c:802/809/810`（`extc_rc_pressure` 的 `/8` 等）、
`:2648/2650/2651/2676`（`thrUpd` 的 `/4` 等）。**没有一条对应 `extc_modI` 调用点**。

常量除数已被折成 magic：`bench/gc/v2/src/promote.extc:35` 的 `i % 10 == 0` 反汇编为

```asm
movabs $0xcccccccccccccccd,%rdx
imul   %rdx,%rax
ror    $0x3,%rax
cmp    $0x1999999999999999,%rax
imul   …          ; 余数还原
```

无 `idiv`。`cycleratio` 的除数是运行期变量 `k` ⇒ 那里的 `idiv` 不可避免；gate 形状
`tests/reclaim-shapes/cyclic_pressure.extc` 的 `extc_modI` 计数为 **0**。

⇒ **结论：不需要专用 inline 形式；gate 指令数不可能被该项影响。不改。**

## 3. 第 (2) 项：`initRef` 与「分配期标黑」是否重复

### 3.1 生成形态

`tests/reclaim-shapes/cyclic_shape.extc` 默认产物（`/tmp/p18/cyclic.c:3646-3651`）逐字：

```c
ExtcRcRegion *__extc_rcr0 = extc_rc_regionOf((void *)&(__extc_a[1]), 1);
#line 32 "tests/reclaim-shapes/cyclic_shape.extc"
{ if (extc_rc_shouldSweep(__extc_rcr0)) extc_rc_maybeCollect(__extc_rcr0, (void *const *)0, 0); }
dnode * head = ((dnode *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(dnode), &__extc_rct0));
extc_rc_initRef((void **)&head, (void *)head);
```

即：**每次 `new` 后必定跟一条对同一槽的 `initRef(slot, slot)`**（非 fresh 的声明同样发：`var prev = head` ⇒
`extc_rc_initRef((void **)&prev, (void *)prev)`，`/tmp/p18/cyclic.c:3655`；null 初始化也发）。

### 3.2 分档分析（关键：默认档根本不发代码）

运行期原文（`src/back/reclaim.c`，FREEZE v4 行号）：

- `extc_rc_initRef`（`:1815` 起）：

  ```c
  static inline __attribute__((always_inline)) void extc_rc_initRef(void **slot, void *v) {
  #if !EXTC_RC_NOCOUNT
      extc_rc_shade(v);   /* no-count tier: local slots carry no barrier ... */
  #endif
      *slot = v;
      extc_rc_inc(v);
  }
  ```

- `extc_rc_inc` 在 `EXTC_RC_NOCOUNT` 下是空函数（`:1663-1664`：`static void extc_rc_inc(void *p) { (void)p; }`），
  计数档才转 `extc_rc_holdUpd(p, +1)`（`:1697-1698`）。
- 分配期标黑（`extc_rc_alloc` 文本，`:1289` 起）：

  ```c
  s->occ[wi] |= bi;
  if (r->marking) s->mark[wi] |= bi; /* allocated during marking means marked black */
  ```

  ⇒ 标黑只在 **marking / sweeping 期**发生，且只在计数档可达。

因此：

| 档 | `initRef` 的真实成本 | 与「分配期标黑」是否重复 |
|---|---|---|
| 清扫档 `NOCOUNT=1`（四个 bench + gate 全在此档，TIER 2/3） | **0 Ir**：shade 被 `#if !EXTC_RC_NOCOUNT` 整段编掉；`*slot = v` 与 C 初始化写同一存储 ⇒ 死存被 DCE；`inc` 空 | 不适用（分配期也不标黑） |
| 事件档 TIER 1（`NOCOUNT=0, SWEEP_FREE=1`） | 每次 ~6 Ir（shade 的 `extc_rc_wb` 早退 + 传参） | 语义上重复（新对象在 marking 期必已被标黑 ⇒ shade 必命中 `skip:shade-black`），但**没有发射侧可用入口**（`inc` 不做 shade） |
| 计数+清扫档 TIER 3 | shade 是**活屏障**（新对象可能在 marking 前分配后被读到） | 分配期标黑只在 marking 期，不能替代 |

⇒ 第 (2) 项的正确答案是：**清扫档与 alloc 标黑无重复可删（成本为 0）；事件档的 shade 是运行期死代码，
可以省，但只能由发射侧「换成不带 shade 的入口」来省**——这就是本补丁。

### 3.3 实现：`cgInitRefStore`

`src/back/codegen.c:7404-7425`（新增；docstring 见下，逐字）：

```c
/**
 * @brief Account for a **new** reference slot whose value is already in place (a declaration).
 *
 * The counted tiers need one increment per stored pointer; the sweep tier also needs the shade,
 * because a white object stored into a slot while a marking is in flight would otherwise be missed.
 * The **event tier** has neither: EXTC_RC_SWEEP_FREE 1 makes extc_rc_shouldSweep a
 * compile-time 0, so extc_rc_begin is never called and extc_rc_wb -- the participant count that is
 * the first test in extc_rc_shade / extc_rc_shadeOld -- stays 0 for the whole run: every shade in
 * this unit is dead code. What remains of initRef is its self-store (the C initializer already
 * wrote the slot, so the store reads and writes the same storage) plus the increment; the increment
 * alone is extc_rc_inc, so the event tier emits that and drops the dead half.
 *
 * Kept per-site (not "the event tier never emits initRef" everywhere): using this in the
 * sweep/mixed tier would remove a live write barrier.
 */
static void cgInitRefStore(CG *g, const char *slotExpr) {
    if (g->rcSweepFree == 1 && !g->rcNoCount) {
        cgLine(g, "extc_rc_inc((void *)(%s));", slotExpr);
        return;
    }
    cgLine(g, "extc_rc_initRef((void **)&(%s), (void *)(%s));", slotExpr, slotExpr);
}
```

两个发射点（均为「值已在槽里」的初始化语义，因此可安全去掉自赋值）：

- `src/back/codegen.c:7480`：`cgValueRefStore` 的 `init` 分支（整块拷贝/多叶路径，原逐叶发
  `extc_rc_initRef((void **)&(d), (void *)(d))`）：
  ```c
          if (init) {
              /* the value is already in the target (the declaration is the initialization): one
               * increment per leaf, through the entry that knows which tier can drop the dead half */
              dbgRcSite(g, g->blkLevel, 0);
              cgInitRefStore(g, d);
  ```
- `src/back/codegen.c:7876-7879`：`REF` 槽声明分支（`initRefNoHold` 的 `else`）：
  ```c
      if (planRcNoDec(g->curFunc) && g->rcNoCount)
          cgLine(g, "extc_rc_initRefNoHold((void **)&%s, (void *)%s);", nm, nm);
      else
          cgInitRefStore(g, nm);
  ```

条件 `g->rcSweepFree == 1 && !g->rcNoCount` **正好是 TIER 1**（`rcSweepFree` 三态：1=事件档、0=默认待分析、
-1=用户强制关；只有升级块 `src/back/codegen.c`（FREEZE v4 `:15604-15612`，工作树 `:15913-15921`）会把
`rcSweepFree=1` 与 `rcNoCount=0` 同时置位）。⇒ 清扫档 / 计数+清扫档 / 无 rc 单位**一律走原路径**。

### 3.4 实测（同 runtime、仅差补丁）

| 形状 / 档 | 基线（`initRef`） | 补丁（`inc`） | 差 |
|---|---|---|---|
| `bench/gc/v2/src/promote.extc`，`EXTC_RC_SWEEPFREE=1` 强制事件档，N=500000 | 1,380,289,371 Ir | 1,365,221,270 Ir | **−15,068,101（−1.09%）** |
| `tests/reclaim-acyclic/acyclic_heapref.extc` 默认事件档，N=200000 | 56,162,951 Ir | 56,162,948 Ir | −3 Ir |

第二行说明：真实事件档程序的热赋值走 `storeRefOwnedReg`（不是 `initRef`），`initRef` 只在循环外出现一次
⇒ 本补丁对**真实**事件档程序的收益接近 0；它的价值在「分配密集且每次 new 都 initRef」的形状（promote 类）。
发射差异（`acyclic_heapref`）：

```diff
3794c3794
<     extc_rc_initRef((void **)&(head), (void *)(head));
---
>     extc_rc_inc((void *)(head));
```

### 3.5 运行期侧更大的杠杆（建议 runtime-facts，超出我的写域）

事件档下 `extc_rc_shade`/`extc_rc_shadeOld` 是运行期死代码，但每处仍付 `extc_rc_wb` 早退。在**生成的 C** 里
把这两个函数体改成 no-op（等价于运行期加 `#if EXTC_RC_SWEEP_FREE` 闸门）后：

| 形状 | 基线 | 现补丁 | 再加 shade 闸门 | 闸门相对基线 |
|---|---|---|---|---|
| promote 事件档 500k | 1,380,289,371 | 1,365,221,270 | 1,333,450,397 | **−3.39%** |
| acyc 事件档 200k | 56,162,951 | 56,162,948 | 54,962,942 | **−2.14%** |

⇒ 运行期闸门的收益是本补丁的 2-3 倍，且覆盖我覆盖不到的 `storeRefOwnedReg`（其内部也调 shade）。
若 runtime-facts 落地该闸门，本补丁变冗余（`inc` 与 `initRef` 会生成同一段代码），但无害、不需要回退。

## 4. 第 (3) 项：`storeRefOwnedReg` 每次赋值的成本（「17 Ir」辨伪）

发射（未改）：`src/back/codegen.c:8995` `extc_rc_storeRefOwnedReg((void **)&(%s), (void *)(%s), %s);`
（注释在 `:8969`：「Only done in the no-count tier: with counting on, storeRefOwnedReg still needs...」）。
运行期文本 `reclaim.c:1857-1896`（FREEZE v4）：`shade(v) → shadeOld(slot) → 跨区 rsInc/rsDec → *slot=v → holdUpd(±1)`。

### 4.1 计数档（TIER 1）200k 迭代的函数分解

`tests/reclaim-acyclic/acyclic_heapref.extc` 默认事件档，N=200000，总 **56,162,948 Ir**（280.8 Ir/迭代）：

| 函数 | Ir | 占比 | 调用次数 | 单次 |
|---|---|---|---|---|
| `extc_rc_holdUpdSlab` | 20,999,917 | 37.4% | 400,000（2/迭代） | ≈52.5 |
| `main`（self） | 14,600,629 | 26.0% | — | — |
| `extc_rc_ownSlab` | 7,600,011 | 13.5% | 400,000 | ≈19.0 |
| `extc_rc_freeSlot` | 6,799,966 | 12.1% | 200,000 | ≈34.0 |
| `extc_rc_cascadePush` | 3,799,999 | 6.8% | 200,000 | ≈19.0 |
| 用户行（`main` 的 extC 源行） | 1,800,151 | 3.2% | 200,000 | 9.0 |
| `memset` | 400,002 | 0.7% | 200,000 | 2.0 |

⇒ 每次引用的计数档屏障 ≈ shade 6 + ownSlab×2 38 + holdUpd×2 105 ≈ **149 Ir**；若这次赋值把计数打到 0
（本形状每轮如此）再付 freeSlot 34 + cascadePush 19 ≈ **53 Ir**。合计 200 Ir 级 —— 与本形状 280.8 的量级一致。
「17 Ir」在任何形状/档位都**不可复现**；它更可能是把「清扫档屏障」的个位数记到了计数档头上。

### 4.2 清扫档（TIER 2）屏障本身的成本

`EXTC_RC_SWEEPFREE=0` 强制清扫档，N=200000，总 **38,768,264 Ir**（193.8 Ir/迭代）。
把运行期 `extc_rc_storeRefOwnedReg` 的函数体替换为纯存储（`{ *slot = (void *)v; (void)host; return; }`，
单行插入定义处 `acyc_t2.c:1522`）后：**36,768,302 Ir** ⇒ 屏障净成本 **1,999,962 Ir / 200,000 迭代 = 10.0 Ir/赋值**。
两版 stdout sha256 完全一致（`05db7717d1dc382d…`）⇒ 对照有效。

⇒ **结论**：清扫档屏障已经极便宜（10 Ir，且其中大部分是 `*slot=v` 本身）；计数档的 149-202 Ir 中
codegen 能省的只有 shade（~6 Ir），其余全在 `holdUpdSlab`/`ownSlab`/`freeSlot`/`cascadePush` —— 属运行期。
**codegen 侧对第 (3) 项没有可动的杠杆。**

## 5. 改动清单（patch）

唯一改动文件：`src/back/codegen.c`（新增 23 行、改 2 行 + 1 处注释措辞）。完整 unified diff，a/ 侧与 FREEZE v4 逐字节相同
（md5 `cf2885440981b5c9d154927fb61441b9`）；打上后与本工作树 `src/back/codegen.c` 逐字节相同（已验证）：

```diff
--- a/src/back/codegen.c
+++ b/src/back/codegen.c
@@ -7402,6 +7402,29 @@
 }
 
 /**
+ * @brief Account for a **new** reference slot whose value is already in place (a declaration).
+ *
+ * The counted tiers need one increment per stored pointer; the sweep tier also needs the shade,
+ * because a white object stored into a slot while a marking is in flight would otherwise be missed.
+ * The **event tier** has neither: EXTC_RC_SWEEP_FREE 1 makes extc_rc_shouldSweep a
+ * compile-time 0, so extc_rc_begin is never called and extc_rc_wb -- the participant count that is
+ * the first test in extc_rc_shade / extc_rc_shadeOld -- stays 0 for the whole run: every shade in
+ * this unit is dead code. What remains of initRef is its self-store (the C initializer already
+ * wrote the slot, so the store reads and writes the same storage) plus the increment; the increment
+ * alone is extc_rc_inc, so the event tier emits that and drops the dead half.
+ *
+ * Kept per-site (not "the event tier never emits initRef" everywhere): using this in the
+ * sweep/mixed tier would remove a live write barrier.
+ */
+static void cgInitRefStore(CG *g, const char *slotExpr) {
+    if (g->rcSweepFree == 1 && !g->rcNoCount) {
+        cgLine(g, "extc_rc_inc((void *)(%s));", slotExpr);
+        return;
+    }
+    cgLine(g, "extc_rc_initRef((void **)&(%s), (void *)(%s));", slotExpr, slotExpr);
+}
+
+/**
  * @brief When a whole-value store carries reference leaves, emit the reference-slot accounting
  *        **leaf by leaf**.
  *
@@ -7451,10 +7474,10 @@
         const char *p = *(const char **)vecAt(&rel, i);
         const char *d = arenaPrintf(g->arena, "%s%s", dstBase, p);
         if (init) {
-            /* the value is already in the target (the declaration is the initialization): read and write the
-     * same storage once, which is a write plus an increment of the target */
+            /* the value is already in the target (the declaration is the initialization): one
+             * increment per leaf, through the entry that knows which tier can drop the dead half */
             dbgRcSite(g, g->blkLevel, 0);
-            cgLine(g, "extc_rc_initRef((void **)&(%s), (void *)(%s));", d, d);
+            cgInitRefStore(g, d);
         } else if (hostUnknown) {
             /* When the host expression cannot be proved, the **slot address** itself may carry the answer
              * (it falls inside an rc slot => that is the memory of some region): try the reverse lookup
@@ -7853,7 +7876,7 @@
     if (planRcNoDec(g->curFunc) && g->rcNoCount)
         cgLine(g, "extc_rc_initRefNoHold((void **)&%s, (void *)%s);", nm, nm);
     else
-        cgLine(g, "extc_rc_initRef((void **)&%s, (void *)%s);", nm, nm);
+        cgInitRefStore(g, nm);
 } else if (g->needReclaim && !s->u.var.isGlobal && s->u.var.init) {
     /* A **whole-value** declaration's reference leaves (`var b: wrap = wrap { n: keeper }`) need
      * accounting too: a bare C initialization emits no `inc` at all, so `b.n` is another way for "the
```

md5：

| 文件 | 前（FREEZE v4） | 后（工作树） |
|---|---|---|
| `src/back/codegen.c` | `cf2885440981b5c9d154927fb61441b9` | `d339382889b87c0dd18b357c69e2e779` |
| `build-cf/extc` | — | `ec65092378265035342d35fa9bbf291d` |

未改：`src/back/reclaim.c`（在途、由 runtime-facts 负责）、`tests/**`、`docs/**` 其他文件。

## 6. 验收

### 6.1 构建与发射等价性（判据的关键：TIER 2/3 逐字节不变）

`make BUILDDIR=build-cf -j 8` ⇒ rc=0、**0 warning**、`codegen.c` 零警告。

两编译器（补丁 vs `if (0)` 基线，同一份在途 `reclaim.c`）生成物 `cmp`：

| 单位 / 档 | 结果 |
|---|---|
| `bench/gc/v2` promote / sizeratio / chase / cycleratio（默认 TIER 2） | **逐字节相同** |
| `tests/reclaim-shapes/cyclic_pressure.extc`（gate 形状，TIER 2） | **逐字节相同** |
| `tests/reclaim-acyclic/acyclic_heapref.extc` + `EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=0`（TIER 3） | **逐字节相同** |
| `acyclic_heapref.extc` 默认（TIER 1） | 1 行不同：`initRef` → `inc`（§3.4） |

四个 bench 的 `.c` 逐字节相同 ⇒ 编译出的二进制也逐字节相同（`cmp` 已验证）⇒ **bench 不可能回退**。

### 6.2 套件 / 门禁（含归因）

| 项 | 结果 | 归因 |
|---|---|---|
| `tests/reclaim/run.sh`（150 条断言 + ASan + 不内联档 + 分段扫描） | 断言 **150/0**、ASan 干净、不内联档 150/0；但 rc=1：唯一 FAIL「着色缓存自检：guard 版编译失败」，错误在 `extc_reclaim_runtime.inc:2572:63: error: comparison between pointer and integer [-Werror]`（`extc_rc_slabOf(pp->v) == (uintptr_t)s`） | **在途 `reclaim.c`**；两编译器同一份 runtime 文本，同样失败 |
| `tests/reclaim-perf/gate.sh`（gate 形状 cyclic_pressure，N=200000，LIMIT 88,029,115） | 冻结编译器 `build-frz/extc`：**86,316,977 ≤ 88,029,115 ok**（−92.6%）；我的树上两编译器都 **615,687,657 Ir FAIL**，数字完全相同 | 615M 是**在途 runtime 文本**造成的（与补丁无关：A/B 数字逐位相同）；判据本身在 TIER 2 产物逐字节不变下不受本补丁影响 |
| `tools/check.py` smoke | io/coro 37/0、gate-reclaim-switch 33/0、gate-reclaim-count 33/0 通过；golden 段 2 个 `.extc` 编译失败（`tests/stl/varArray_mutview_lost.extc:23`、`varArray_snapshot.extc:33`）—— **HEAD 原版同样失败（pre-existing）**；gate-work 段 5 pass / 1 FAIL（`cross` 拿不到 `[work]` 行） | 两编译器跑 gate-work 日志**逐字相同** ⇒ 非本改动 |
| `bench/gc/v2/run.sh` 四形状墙钟（`taskset -c 6`，5 reps） | promote 0.24/0.17/0.16/0.16/0.15 s；sizeratio 0.11-0.12 s；chase 0.19/0.20/0.20/0.22/0.25 s；cycleratio 0.04 s | 二进制逐字节相同 ⇒ 差异全属外部噪声；另注意运行期打印 `[mmax] memory wall unreachable: M_max=… floor=1048576; using floor`（他人在途特性） |

### 6.3 复现命令

```sh
# 构建（不用 build/：多 agent 共用会互相覆盖）
make BUILDDIR=build-cf -j 8

# 发射 A/B
R=$PWD
build-cf/extc -w --no-line-map -I $R/stdlib $R/tests/reclaim-shapes/cyclic_pressure.extc -o /tmp/cp_new.c
/tmp/mine2-src/build-mine2/extc -w --no-line-map -I $R/stdlib $R/tests/reclaim-shapes/cyclic_pressure.extc -o /tmp/cp_base.c
cmp /tmp/cp_new.c /tmp/cp_base.c    # 期望：相同

# Ir
cc -O2 -g -D_POSIX_C_SOURCE=200809L -o /tmp/p.bin /tmp/cp_new.c
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.out /tmp/p.bin 200000
callgrind_annotate /tmp/cg.out | grep 'PROGRAM TOTALS'

# 门禁
EXTC=$PWD/build-frz/extc tests/reclaim-perf/gate.sh
EXTC=$PWD/build-cf/extc tests/reclaim/run.sh
```

## 7. 未验证 / 风险

1. **在途 `reclaim.c` 污染绝对 Ir**：本文所有 Ir 是「同 runtime 文本 A/B」的差值或档间比较；一旦 runtime-facts
   收尾，绝对数字会变（gate 从 615M 回到 86M 量级），差值结论（本补丁的净效应）不受影响。
2. **事件档收益只对一个形状实测**（promote 强制事件档 −1.09%）；其它事件档形状的收益取决于「`new` 后立即
   `initRef`」的出现密度，未做形状普查。
3. **「17 Ir」的出处未找到**：全仓 `grep` 无记录（`docs/analysis/RUNTIME.md:442` 只描述分支结构），
   只能报告「不可复现」而非「错在哪」。
4. **`idiv` 归属**按运行期文本行号对齐（`#line` 使运行期映射到生成 .c 行）；若运行期文本再变，行号会漂。
5. **事件档 shade-off 的上界**用「生成物里改运行期函数体」估计，未真正编译 `#if EXTC_RC_SWEEP_FREE` 变体；
   若 runtime-facts 落地，实际收益可能略低于 1,333,450,397（例如 `wb` 之外还有别的引用路径）。
6. **gate-work 的 `cross` FAIL** 未深入（两编译器逐字相同，判为非本改动；但其是否为 pre-existing 还是
   在途 runtime 引起，未与 HEAD 单独对照）。


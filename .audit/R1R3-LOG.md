# R1/R3 批次进展（逃逸/生命周期分析的健全性）

按主人 2026-09-30 的三个决定施工：
① 掩码 **64 位**，参数 >64 ⇒ **编译期报错**；② 新的拒绝消息 = **规则 + 逃逸路径 + 一条出路**；
③ **能判的就判精确，判不了的才拒**。

## 一、闸门⑤·逃逸健全性（判据先行）

- 语料：`tools/escape-corpus/`（17 份）。每份是审计里"**记账比真实情况小**"的洞的最小复现 ——
  它们**被接受**而现在生成物在 ASan 下真的 UAF / SEGV / 编不过。
- 判据：语料必须按各自的 `gate-expect` 表现：
  - `reject`（默认）：编译器必须**报错**（带源位置）；
  - `run`：必须**接受**，且生成物在 ASan 下**跑干净**并打印 `gate-expect-out:` 那串
    （**有些洞的正确修法是"编译对"而不是"拒绝"** —— P0-2 就是，见下）。
- `control_*.extc` 是孪生对照（同一形状、已能正确拒绝）：**永远不许进基线**，否则闸门会退化成
  "一律拒绝就算过"。
- 基线 `tools/gate-escape-known-bad.txt` = **本批的施工单**（11 条），棘轮：修好一条删一条。
- 已接进 `check.sh` **quick**（新增第 54 节）。

## 二、已完成的修复（2/13）

### INV-P：效果摘要按参数下标的**三处 32 上限**（P1-4）

现象（审计）：**同一个 store 形状**，4 个参数时被正确拒绝，40 个参数时却被接受。

实测定位（二分实验）：阈值正好在 **32 个参数**。链条上有三层各自的 32 位：

| 层 | 原来 | 现在 |
|---|---|---|
| 摘要字段（`FuncDef.addrMask/contMask/otherMask/home*Mask`） | `unsigned`（32 位） | `uint64_t` + `EFF_MAX_PARAMS 64` |
| 掩码移位 | `1u << j` | `((uint64_t)1 << j)`（10 处） |
| 实参/参数循环 | `j < 32`（3 处）、`i < 32`（1 处） | `< EFF_MAX_PARAMS` |
| **消费侧局部变量** | `unsigned stored/cont/pub/addr/all`（5 处，把 64 位截回 32 位） | `uint64_t` |
| 参数上限契约 | 无 | `>64 ⇒` 编译错误：`` `stash` has 65 parameters; the compiler's limit is 64 `` |

**判据**：40 参数探针现在报
`argument 33 of `stash` carries a reference into a deeper scope (depth 2) than the place the callee may store it (depth 1)`
（与 4 参数孪生程序同一判据）；65 参数报上限错；42/64 参数都拒绝；两个对照（4 参数、无遮蔽孪生）仍拒绝；
基线 13 → 12。

### INV-S：`0` 当哨兵 ⇒ 求成最大值（P0-2）

两处副本（`check_top.c` 的 arena 通道与 zone 通道）都写成：

```c
int nd = 0;
for (k...) { ...; if (nd == 0 || d < nd) nd = d; }   // nd==0 既是"没见到"又是"depth 0（最浅）"
if (nd == 0 && rec->n > 0) nd = rec->argDepth[0];    // 补丁反而把它变成最大值
```

`argDepth = {0, 1}` 因此算出 `nd = 1`，`want = (nd <= 0) ? ARENA_HOME : nd` 就把**调用方本帧的块
arena** 交给了被调方（应为 `__extc_home`）⇒ 块一退，被长期持有的对象就成了悬垂。

修法：加 `bool seen`，真正求**最小值**；删掉那条补丁。**实测**：生成物从
`stash(dest, &(deep), 5, &__extc_a[1])` 变成 `stash(dest, &(deep), 5, __extc_home)`，
ASan 从 heap-use-after-free 变成**干净**并打印 `v=5`。

⇒ 所以这条**从"必须拒绝"改判为"应当接受"**（`gate-expect: run`）：它的正确终态是编译对，
硬要它拒绝就是误拒 —— 这正是主人决定③（能判就判精确）的一个实例。

## 二·补：INV-C 的第一次尝试**失败并已回退**（记录教训）

**做法**：把 `exprBorrowed` 的 `default: return false` 换成**通用子节点遍历**
（`astWalkExprChildren` + 访问器），理由是"新节点默认安全，而不是靠列举"。

**结果**：目标两条（P0-1、P0-6a）**都没修好**，却造成**广泛误拒**：
`tests/run.sh` 3 例红（`showcase-pool-identity`、`task-place-release`、`tour-plots`），
`check.sh quick` 从 54/0 掉到 **44/10**。典型诊断：

```
examples/task-place-release.extc:98: error: in instance `vector$vector_i32`:
  cannot store a borrowed value into something that outlives this call
```

**根因（教训）**：通用遍历会走进**子女不属于"值的内容"**的节点 ——
`EX_EXT`（spawn 的那个调用：它像语句，不是值的一部分）、`EX_NEW`（计数）、以及
`EX_BIN`/`EX_UN` 之类只靠"类型里有没有引用"就该提前退出的节点。于是 stdlib 里大量
**合法**存储被判成"借来的"。⇒ **INV-C 不是"遍历一切"，而是一份经过语料逐条验证的载体清单。**

**同时确认了两条更根本的事实**（决定下一步顺序）：

1. **P0-1 不在 `exprBorrowed` 这条路上**：`EX_TRY` 的洞在**逃逸集合的记录侧**
   （`c->escapees` 由 `isEscapeeName` / `callHomeDepth` 填充），要让那条**登记遍历**穿透 `EX_TRY`。
2. **P0-6a 的形状是"经局部变量洗白"**（`var q = p  G = q`）：`exprBorrowed(q)` 看的是
   `placeRoot(q)->depth`（局部的**词法**深度 = 1），而不是"q 装着一个借来的东西"这个**来源事实**。
   这正是定案 97 的收窄契约（"知道初始化器的人随后收窄"）与 INV-F（事实挂在 `Sym` 上）该管的。

**已回退**：`git checkout -- src/check_escape.c`，回退后 `tests/run.sh` **324/0**、
`check.sh quick` **54/0**（教训留下，代码不留）。

## 二·补2：P0-1 已修（逃逸集合的记录侧）——**又一个"终态是编译对"的**

`out.put(x)?` 是 `EX_TRY` 包着调用，而**语句级**的逃逸登记（`markNamesInStmt` 的 `ST_EXPR` 分支）
只认 `EX_CALL`/`EX_METHOD`/`EX_ASSOC` ⇒ 这个调用的实参从不进逃逸集合 ⇒ `callHomeDepth` 把接收者
按 depth 1 算 ⇒ 传给被调方的是**本帧的块 arena**（块一退，被调方存进去的东西就死）。

修法（小、局部）：登记前先**拆掉 `EX_TRY`**（`?` 决定调用**之后**发生什么，不决定调用能发布什么）。
注意 `markNamesInExpr` 本身**有** `EX_TRY` 分支 —— 漏的只是语句级那个按顶层种类分派的 switch。

**实测**：生成物 `list_pushOne(&(l), 7, &__extc_a[1])` → `list_pushOne(&(l), 7, __extc_home)`；
ASan：heap-use-after-free（`tryb3.extc:40 in main`）→ **干净**，打印 `head = 7`。

⇒ **与 P0-2 一样，正确终态是"接受且跑干净"，不是"拒绝"**。闸门⑤因此把这条改成
`gate-expect: run`（编译 + ASan + 输出判据），基线 11 → 10。

**统计（到目前为止 3/13）**：3 条修好的洞里，**2 条的终态是"编译对"**（P0-2、P0-1），只有 P1-4 是"应当拒绝"。
这条经验很重要：闸门语料必须能表达两种终态，而且**每修好一条都要重新判断它的终态是哪一种**
（"探针原来被接受"不等于"修好之后应当被拒绝"）。

## 二·补3：P0-3 已修（自由泛型的零值检查从不记录）

**根因**：`recordZeroCheck` 开头 `if (!c->curFunc || !c->curFunc->owner) return;` ——
自由泛型函数（`fn mk<T>() -> T`）没有 `owner` ⇒ **永不记录** ⇒ replay 无从执行 ⇒
`mk<slice<u8>>()` 生成 `__extc_reference_has_no_zero_value__`，产物根本编不过；
而**方法版**（`boxT<T>::stash`）走的是同一条 replay、被正确拒绝（同一件事两个答案）。

**为什么可以去掉**：replay 循环**已经**遍历自由函数实例
（`for j < c.funcInsts.len ... refCheckApplies(rc, fi) ... runRefCheck(...)`），
而 `refCheckApplies` 只按 `rc->func == fi->tmpl` 匹配、不依赖 owner。那道早退是
"只 replay 类型实例"时代的遗留。

**实测**：`error: in instance `mk_slice_u8`: `local` has no zero value (it contains a reference)` ——
与方法版同一判据、同一句话。基线 10 → 9。

## 二·补22：**addr4 落地 —— 批次收口（13/13，闸门⑤基线清零）**

补21 的三步收窄，逐步把副作用挤掉，最终**全部达标**：

| 步骤 | 改动 | addr4 | pool/stl | tests |
|---|---|---|---|---|
| 全量版（补21） | 专用槽 + 无条件发布 + `recordLvlFact` | ✓ 跑干净 | ✗ 3 例误拒 | 325/0 |
| **第 1 步** | 去掉 `recordLvlFact`（对**每次** `mut ref` 调用压电平要求） | ✓ 跑干净 | ✗ 2 例 | 325/0 |
| **第 2 步** | 专用槽只在 `dj != 0` 时写 | ✗ 又 UAF | ✓ 0 例 | 325/0 |
| **第 3 步** | 专用槽在 `dj != 0` **或**来源可提升（`ss->origin` 是 fresh 分配）时写 | ✓ 跑干净 | ✓ 0 例 | 325/0 |

⇒ 关键的第三条：**"能提升"才发布**。`dj == 0`（引用型来源在调用点 `refDepth` 尚未填上）时，
只有当来源背后真的是一个可搬的分配（`var n: mut ref node = new node`）才写专用槽；
写不动的来源不发布——发布它只会给电平加压力，那正是 pool/stl 三例被误拒的原因。

**全量验收（收口）**：

| 检查 | 结果 |
|---|---|
| 闸门⑤·逃逸健全性 | **失败 0 / 基线 0 / 过期 0** —— 17 份语料全部达标（12 份应当拒绝、5 份应当接受且 ASan 干净）|
| `tests/run.sh` | **325 / 0** |
| `tests/arena` · `tests/coro` · `tests/pool` · `tests/stl` | 6 ok · 29/0 · 0 失败 · 0 失败 |
| `check.sh quick` | **54 / 0** |
| 闸门①（checkc 全量）·②（ASan+UBSan 全量）·③（差分）·④（规模） | 全绿，**四条基线全空** |

**R1/R3 批次完成：13/13 个条目全部落地，闸门⑤的棘轮基线清零。**

## 二·补21：addr4 的**机制成功、副作用失败**（已回退，下一步只剩"收窄"）

**做法**（补10 的方案，三处）：

1. `Sym` 加**专用槽** `callSrc / callSrcDepth / callMinReq`（**不复用** `otherSrc`——那条路被元素写共用，
   补10 实测会把 `store-promotion`/`promote-slice`/`promote-through-mid` 打红）；
2. `promoteFieldsAt` 增加 `callSrc` 分支：在返回路径上把站点提到所需层（`want = min(callSrcDepth, callMinReq)`）；
3. 发布处：`dj == 0` **不再跳过**；`callSrc = splace`；并补 `recordLvlFact(c, splace, h)` 交给末轮重放。

**结果：机制完全成功** ——

```
修前 生成物 n = extc_arena_alloc(&__extc_a[1], …)  → ASan heap-use-after-free（freed by extc_arena_destroy）
修后 程序 exit=42、stdout 空、stderr 0 字节       → 站点被提升到 home arena，UAF 消失 ✓
```
这与"直接孪生" `b.p = n; return b`（本就接受且 ASan 干净，`rc=42`）**完全一致**，
说明 addr4 的正确终态确实是"**接受且跑干净**"（与 P0-1/P0-2 同类），不是"拒绝"。

**副作用：3 例误拒**（先抄名字再回退）：

```
tests/pool/run.sh :  FAIL rt_nest  -> 编译/运行失败
                     FAIL rt_rett  -> 编译/运行失败
tests/stl/run.sh  :  FAIL custom   -> 编译/运行失败
```

**已回退**（工作区与补20 末逐字节一致：哈希 `94686e88…`、闸门⑤ 1 条基线、`tests/run.sh` 325/0、
pool/stl 0 失败、`check.sh quick` 54/0）。

### 下一轮：三步收窄（按嫌疑排序，每步只跑一次全套件）

嫌疑最大的是**第 3 步里"无条件"的两件事**——`recordLvlFact(c, splace, h)` 对**每一次** `mut ref` 调用
都压了一条电平要求，而 `callSrc` 也是无条件写的（包括不可提升的来源）：

1. 先只去掉 `recordLvlFact` 那一行（保留专用槽 + 发布），看 addr4 是否仍跑干净、三例是否恢复；
2. 若不行，改成**只在 `dj != 0`** 时写 `callSrc`（即"能判才发布"）；
3. 若仍不行，再限制到"来源是**可提升**的形状"（`EX_IDENT` 绑定到 `new`/fresh），
   并把不可提升的情况按"**证不出提升 ⇒ 拒绝**"收口（与"局部不可提升 ⇒ 拒绝"同一规则）。

判据（每步都一样）：addr4 **接受且 ASan 干净**；`tests/run.sh` 325/0；pool/stl 0 失败；
闸门⑤ 基线清空；其余闸门不动。

## 二·补20：**P0-17 落地**（`mut ref` 实参就是写权限 ⇒ 证明失效）——第 13/13 条

**根因**：非空证明栈（`c->narrow`）只在**赋值**与取址时失效，**被调方经 `mut ref` 改写**不算。
`stack::pop(self: mut ref stack)` 里 `self.top = t.next` 把 `s.top` 置空，而调用方的
`if s.top != null { var v = s.pop()  s.top.value }` 里那条证明还留着 ⇒ 生成物解引用 NULL（ASan SEGV）。

**修法**：在 `checkCallRefArgs` 里对**每个 `mut ref` 实参**做一次写点失效：

```c
for (i < params->len && i < args->len) {
    Param *p = …;  if (p->type->kind != TY_REF || !p->type->mut) continue;
    Expr *place = a->kind == EX_REF ? a->u.ref.operand : a;
    Sym *rs = placeRoot(c, place);
    if (rs) unNarrow(c, rs->cname);      /* 路径事实 `s.top` 随根一起被丢掉（前缀规则） */
}
```

**判据条件取 `mut ref` 而不是"摘要说它会存"**：`mut ref` 参数**按定义**就是写权限；
摘要是"往调用方容器里存"的上界，不是"经引用写"的上界。

**判据**：探针现在报 `` `?ref node` is a nullable reference (`?ref T`), so field access needs a
non-null one ``（过期证明没了 ⇒ 悬垂解引用在编译期被拦下）；`tests/run.sh` **325/0**；
`check.sh quick` **54/0**；闸门⑤ **2 → 1 条基线**；闸门①/ASan/差分/规模 全绿。

**至此施工单只剩 1 条**：`P0-16_outparam_d`（addr4，`n = new node` 的形状）。

## 二·补19：**P0-6a 落地**（来源链：`var q = p` 的借用跟着走）——第 12/13 条

**探针的实质**（比审计的"载体形状"更准）：`P0-6a_carrier_lit` 是**经局部变量洗白**——
`fn stash(p: slice<u8>) { var q: slice<u8> = p  G = q }`。`exprBorrowed(q)` 只问"绑定的**词法深度**
是不是 0"，`q` 是本帧局部 ⇒ 答 false ⇒ 写进全局被放行 ⇒ `G` 读到兄弟块复用的死栈槽。

**修法**：`exprBorrowed` 的 `EX_IDENT/FIELD/INDEX` 分支里，除了"绑定本身是参数"，
再跟一层**来源链**（`Sym.origin`，也就是定案 97 的"知道初始化器的人随后收窄"）。
`var q = p` 让 `q` 的来源就是参数 `p` ⇒ 借用跟着走 ⇒ `G = q` 被拒。

**关键约束（本轮差点吃亏，已写成规则）**：来源只跟**裸绑定**（`EX_IDENT`）。
第一版把 `EX_FIELD`/`EX_SLICE`/`EX_REF` 来源也跟了，于是

```
self.top = t.next   →  error: cannot store a borrowed value into something that outlives this call
```

—— `t` 的来源是 `self.top`（`EX_FIELD`），而"从 `self` 指向的对象里读一个引用、再写回同一个对象"
两侧同寿，**什么都没被延长** ⇒ 纯误拒。而且它让 **P0-17 的语料看起来被修好了**
（那条探针也被同一规则拒了），**其实洞还在** —— 一次"假绿"。
收窄到 `EX_IDENT` 之后：P0-6a 真拒 ✓、P0-17 回到"仍被接受、留在基线" ✓（诚实）。

> 规则（写进记录）：**修好一条探针时，必须看清它是因何被拒**；闸门只会告诉你"被拒了"，
> 不会告诉你"拒对了没有"。这轮的 P0-17 就是这样被抓出来的。

**判据**：`P0-6a` 拒绝；`P0-17`/`P0-16d` 仍被接受（留在基线）；`tests/run.sh` **325/0**；
`check.sh quick` **54/0**；闸门⑤ **3 → 2 条基线**（0 新增 / 0 过期）；其余闸门全绿。

## 二·补18：**P0-6d 落地**（参数归属改按符号身份）——第 11/13 条

按补17 的四步实施，一次成功，且**转储判据与预判一字不差**：

```
修前  [effects-closed] stash … toParam[Addr=0x2 Cont=0x0] other=0x0   → 被接受 ✗
修后  [effects-closed] stash … toParam[Addr=0x0 Cont=0x0] other=0x1   → 拒绝 ✓
      error: argument 3 of `stash` carries a reference into a deeper scope (depth 2)
             than the place the callee may store it (depth 1); the callee's effects could not
             be fully analyzed
```
—— 与**无遮蔽孪生** `control_m2`（本就被正确拒绝）完全同一句话。

**改动的四处**：

| 位置 | 改动 |
|---|---|
| `FuncDef`（`ast.h`） | `void *paramSyms[64]; int nParamSyms;` —— 参数的绑定，按声明序。
`void *` 是因为 `Sym` 定义在 check_internal.h 里、ast.h 看不到；只有检查器读它。
（**定长数组**：补17 那次用 `Vec` 忘了 `vecInit` 直接段错误，定长数组没有这个坑。） |
| 函数体前导（`check_top.c`） | `declare` 出参数 Sym 的那一刻就登记进表（并先 `nParamSyms = 0`，泛型体可能被走两次） |
| 新增 `rootSymNoScope(e)` | **不查作用域**取根绑定：`EX_IDENT` → `identBindOf(e)`（解析时就写在节点上），`FIELD/INDEX/DEREF/SIGN` → 递归 |
| 新增 `paramIndexOfExpr(c, f, e)` + 两处值分类 | 与 `f->paramSyms[]` **比指针**；地址流与内容流两处从 `paramIndex(f, 名字)` 换过来 |

**为什么必须"不查作用域"**（补17 证出来的结构性原因）：摘要 pass 在所有函数体检查完之后才跑，
那时 `c->curFunc == NULL`、**没有任何作用域** —— 这正是它一直只能按名字匹配参数的原因。

**判据**：m3/m2 都被拒；`tests/run.sh` **325/0**；`check.sh quick` **54/0**；
闸门⑤ **4 → 3 条基线**（0 新增 / 0 过期）；闸门①/ASan/差分/规模 全绿。

## 二·补17：**P0-6d 的机制查清了** —— 摘要是"没有作用域的 pass"，所以它只能按名字

### 决定性证据（两支程序只差一行遮蔽）：

```
control_m2  [effects-closed] stash complete=1 toParam[Addr=0x0 Cont=0x0] toHome[...] other=0x1
            [refargs]        stash complete=1 addr=0x0 cont=0x0 other=0x1
            → error: argument 3 of `stash` carries a reference into a deeper scope (depth 2) ...
P0-6d(m3)   [effects-closed] stash complete=1 toParam[Addr=0x2 Cont=0x0] toHome[...] other=0x0
            → 无诊断（被接受）✗
```

- **m2**：值 `q` 的名字不是参数 ⇒ 落 `otherMask`（位 0）⇒ 调用点走"检查**每个**带引用的实参"
  ⇒ 抓到 `a[..]`（深度 2 > 1）✓
- **m3**：值 `s` 是**遮蔽参数的局部**，却仍被记成 `Addr=0x2`（**参数 1**）⇒ 调用点只查参数 1
  对应的那个实参（`"shallow"`，安全）⇒ 放过 ✗

### 为什么补16 的"身份版查表"没生效（结构性原因，很重要）

**效果摘要是在所有函数体都检查完之后才算的**（那是 `c->curFunc == NULL` 的时刻）⇒
**没有作用域**，`placeRoot` / `lookup` 都无从解析 ⇒ 我在 `paramIndexOfExpr` 里的
"`c->curFunc != f` ⇒ 退回按名字"那条兜底正好被走中 ✗。
**这就是摘要按名字匹配参数的根本原因：它拿不到作用域。**

### 下一轮的正确修法（小、明确）

1. 把参数 Sym 表从 `Checker` **挪到 `FuncDef`**，并且**用定长数组**（`Sym *paramSyms[64]; int nParamSyms;`）
   —— 不用 `Vec`：本轮我用 `Vec paramSyms` 忘了 `vecInit`，直接段错误（`rc=139`，tests 2/313），
   定长数组没有这个坑，也不需要动 `FuncDef` 的构造点。
2. 根 Sym 从**节点自己**取，不查作用域：
   `rootSymNoScope(e)` = `EX_IDENT` → `identBindOf(e)`（parser 解析时就写在节点上），
   `FIELD/INDEX/DEREF/SIGN` → 递归其 `obj/operand`。
3. `paramIndexOfExpr` = `rootSymNoScope(e)` 与 `f->paramSyms[]` **比指针**。
4. 判据：m3 的摘要应从 `Addr=0x2` 变成 `other=0x1` ⇒ 被拒；`control_m2` 仍拒；
   `tests/run.sh` 325/0；闸门⑤ 4 → 3 条基线。

**本轮状态**：段错误后从快照回退（`/tmp/rev/snap/`），当前 build 零告警、
`tests/run.sh` **325/0**、`check.sh quick` **54/0**、闸门⑤ **4 条基线 / 0 新增 / 0 过期**，
二进制哈希 `feeaa1c0…`（= 补15 末的已验收状态）。补16 的"侧表版"改动随回退一并撤掉
（它本就没修好探针，不影响进度）。

## 二·补16：INV-I 的**身份版参数归属**落地（P0-6d 探针仍未拒，接受点已缩小到两处）

**这轮落地**（已验证零回归，保留）：

| 位置 | 改动 |
|---|---|
| `Checker`（`check_internal.h`） | `Sym *paramSyms[64]; int nParamSyms;` —— 当前函数体参数的 Sym 侧表 |
| `check_top.c` 参数声明处 | 声明一个参数就把它登记进侧表；进入函数体时清零 |
| 新增 `paramIndexOfExpr(c, f, e)` | **按符号**回答"这个值来自哪个参数"（`placeRoot` 取根 Sym 与侧表比对），比不到就 -1 |
| 值分类两处（地址流/内容流） | `paramIndex(f, placeRootName(...))` → `paramIndexOfExpr(c, f, e)` |

判据：`tests/run.sh` **325/0**、`check.sh quick` **54/0**、闸门⑤ **4 条基线 / 0 新增 / 0 过期**、
闸门①/ASan 全绿；对照 `control_m2` 仍正确拒绝。

**但 P0-6d 探针（`m3`）仍被接受** ⇒ 接受点在别处。本轮已把范围缩小到：

1. **被调方那次存储检查本来就不会报**：`b.s = s` 里 `s` 是**本帧局部** ⇒ `exprBorrowed(s)` 为假
   ⇒ `checkStoreEscape` 不报错。所以本该由**摘要**把它送出去。
2. **摘要现在应当记成 `otherMask`（目的参数 0）**，而调用点对 `otherMask != 0` 走"检查**每个**
   带引用的实参"那条路（`if (contMaybe && (!complete || callee->otherMask != 0))`），
   那里应当捕获 `a[..]`（深度 2 > 允许的 1）——**为什么没有捕获，是下一轮的定位题**。
   候选：调用点的 `h` 算得比 1 深；或那条路被 `contMaybe` 之外的守卫挡掉；
   或摘要里 `otherMask` 的位被 `homeContMask` 之类覆盖。

（`control_m2`（无遮蔽孪生）被拒的机制正是"`q` 不是参数 ⇒ 落 `otherMask` ⇒ 调用点全查"，
与上面第 2 点同一条路 ⇒ 说明这条路**能**报错，差别一定在"m3 的什么让摘要或调用点变了样"。）

## 二·补15：**P0-6b 落地**（`needsHome` 早退对全局不成立）——第 10/13 条

**根因**：存储检查里有一句"赦免"：

```c
if (c->curFunc && c->curFunc->needsHome) return bad;   /* 注释：目的地与 home 同寿 */
```

它成立的**前提**是"目的地和这次调用的 home arena 活得一样长"。**目的地是全局时不成立**：
静态存储比任何 arena 都长寿 ⇒ 没有任何 home arena 能让这个借用变安全。审计 P0-6b 的复现里
`fn stash(p: slice<u8>) -> hbox { G = p  var b = new u8[4]  return {p: b} }` 被接受并读到
`G = AAAAA...`；把同一函数改成返回 `i32`（**没有** home arena）反而被正确拒绝 —— 一个"有 home
反而更宽松"的反直觉缺口。

**修法**：不再假设，而是**问目的地住在哪**：

```c
Sym *destRoot = target ? placeRoot(c, target) : NULL;
if (c->curFunc && c->curFunc->needsHome && !(destRoot && isGlobalSym(c, destRoot)))
    return bad;
```

**判据**：P0-6b 探针变**拒绝**；`tests/run.sh` **325/0**；`check.sh quick` **54/0**；
闸门⑤ **5 → 4 条基线**（0 新增 / 0 过期）；闸门①/ASan/差分/规模 全绿。

## 二·补14：P0-6d 的第一次尝试**失败并已回退**（拿到了两条关键情报）

**做法**：`valTracesToParam` 里把"名字比较"换成"身份 + 深度 0"——
`placeRoot(c, val)` 取根 `Sym`，要求 `root->depth == 0`（参数/全局才在帧外），再比名字。

**结果**：

| 项 | 结果 |
|---|---|
| P0-6d 探针 | ✗ **仍被接受** |
| `tests/run.sh` | ✗ **5/3**（先抄下名字再回退） |

失败用例名（存档）：**`container-nested`、`container-of-view`、`generic-effects-per-instance`**。

**两条情报（下一轮直接用）**：

1. **根的"词法深度"不是正确的判别式**。那三个失败用例都是"值经**容器/拷贝**仍然追溯到参数"
   的正当程序：根的绑定是局部（depth > 0），但**值**来自参数。
   ⇒ "追溯到参数"是关于**值的来源**的，不是关于**绑定的作用域**的。
2. **P0-6d 探针的接受点不在 `valTracesToParam`**：改了这个谓词，探针没变。
   ⇒ 得先找出真正放行它的那个谓词（同族的候选：`destIsParam`、`paramIndex`、
   `markNamesInStmt` 里 `paramIndex(f, rn) >= 0`），**先定位再改**（与本批次反复学到的同一条）。

**正确的实现方向（INV-I 的完整版）**：给 `Param` 加一个 `Sym *sym` 指针，在函数体前导处声明参数时填上，
然后把所有"按名字找参数"的地方（`paramIndex` / `destIsParam` / `valTracesToParam` /
`markNamesInStmt` 的那一处）统一改成**比较 `Sym *`**。
这才是"身份而非拼写"；我这次的"深度 0"只是个近似，既漏（探针没修好）又误伤（三个正当用例）。

**回退后状态**：build 零告警、`tests/run.sh` **325/0**、闸门⑤ **5 条基线 / 0 新增 / 0 过期**、
二进制哈希 `3aadb56a…` 与补13 末态**逐字节相同**。

## 二·补13：**P0-6c 落地**（`placeDepth` 兜底方向修反）——第 9/13 条

**根因**：`placeDepth` 最后一行 `return root ? root->depth : 0;` —— `placeRoot` 只认
`EX_IDENT`/`EX_FIELD`/`EX_INDEX`/`EX_SLICE`，**认不出来的形状一律答 0**，而 0 的语义是
"活得最久" ⇒ **保守方向恰好相反**：

```
G = v            // 被正确拒绝（本帧的局部）
G = [v, v][0]    // 被接受，然后读到已死的数组（审计 P0-6c）
```

**修法（INV-U）**：未识别形状该问的是检查器对"值"一直在问的那个问题——**它指向的东西有多深**：

```c
return root ? root->depth : exprRefDepth(c, e);
```

对 `[v, v][0]` 就是 `v` 的深度；对调用结果就是被调方分配的层。
不会递归回自己：`exprRefDepth` 只在表达式**本身是地方**时才问 `placeDepth`，
而这个分支正是 `placeRoot` 已经答 NULL 的那一支。

**判据**：P0-6c 探针变**拒绝**；`tests/run.sh` **325/0**；`check.sh quick` **54/0**；
闸门⑤ **6 → 5 条基线**（0 新增 / 0 过期）；闸门①/ASan/差分 全绿。

## 二·补12：**P1-5 落地**（`fresh` 集合的写点失效）——第 8/13 条

**根因**：`collectFreshLocals` 只在 `ST_VAR` 时按**声明初始化器**收录名字，之后**从不失效**。
探针里 `var n: mut ref i32 = new i32` 让 `n` 进集合，随后 `n = id(target)` 把它**重新指向调用方的
深层局部**，而 `classifyStoredValue` 仍按"fresh ⇒ 天然安全"提前返回 ⇒ 摘要什么都不记、
`effComplete` 还是 true ⇒ 调用点整段生命周期检查被跳过（ASan `stack-use-after-scope`）。

**修法**：在 `collectFreshLocals` 的走查里加 `ST_ASSIGN` 分支 —— **整对象重指向**到一个非 fresh 的
值时，把名字从集合里踢掉（`freshDrop` 做一次压实）。这就是 fresh 集合的**写点失效**。

**闸门当场抓到的过宽（很重要的一条经验）**：第一版把 `n.value = v` 这类**字段写**也算了进去
（`placeRootName(n.value)` 也是 `n`）⇒ 把真正 fresh 的 `n` 踢出集合 ⇒ 调用点检查转而拒绝
`stash(dest, ref deep, v)` —— 正是 **P0-2 那个"应当接受且 ASan 干净"的探针** ✗。
收窄为"目标必须是**裸标识符**（`EX_IDENT`）"后即正确。

> ⇒ 这条验证了闸门⑤的两种期望设计是对的：**`run` 类探针抓住了一个"过度拒绝"型的回归**，
> 而单纯的 must-reject 语料永远抓不到它（拒绝变多看起来总是"更安全"）。

**判据**：P1-5 探针现在报
`argument 2 of `stash` points into a deeper scope (depth 2) than the arena this call may store it in (depth 1)`；
闸门⑤ **7 → 6 条基线**、0 新增、0 过期；`tests/run.sh` **325/0**；`check.sh quick` **54/0**；其余闸门全绿。

## 二·补11：**P0-5 落地**（延迟解析改不动点）——第 7/13 条

**根因**：`callChecks × 实例` 的延迟解析是**单趟**。解析一个调用点会**新建**被调实例
（`funcInstance` 按需 intern），而那个新实例**自己的**延迟调用点没有机会被解析 ⇒
`fn f<T>` / `fn g<T> { f(x) }` / `fn m<T> { g(x) }`（内层先声明）生成物里出现
一个从未生成过的 `f_T` ⇒ C 编不过；把声明顺序倒过来就能编过，所以它看起来像"声明顺序怪癖"。

**修法**：外壳加不动点循环——每轮记录 `funcInsts.len` / `tt->instances.len`，
两者都不再增长就停（上限 64，与其它不动点一致）。`resolveDeferredCall` 是幂等的
（同一组类型实参 intern 出同一实例），重复跑不花钱。

**判据（新的正例）**：`examples/generic-instance-order.extc`（就是 g3 的形状）
- 生成物过 `gcc -std=c11 -O2 -c` 与 `clang -std=c11 -O2 -c` ✓
- 跑出 `r = 3` ✓
- 它在 `examples/` 里 ⇒ 被**闸门①（--check-c 全语料）**与套件同时覆盖 ✓

**过程中的两个小坑（都记下来省下一次的时间）**：
1. 例子里 `main` 返回 `i32(r)` = 3 ⇒ 套件把非零退出码当"运行失败"。**例子必须 `return 0`**，
   值用打印表达。
2. `expect` 的写法：套件里 `tests/annot` 用的是 `// expect-error:`；我没有再猜输出断言的格式，
   改成"打印 + 退出 0"后套件即绿（输出一致性由闸门①的编译/运行覆盖）。

**当前状态**：`tests/run.sh` **325/0**、`check.sh quick` **54/0**、闸门①（checkc 全量）✓、
闸门⑤ **7 条基线 / 0 新增 / 0 过期**；构建零告警。

## 二·补10：addr4 的拦路虎=**发布被 `dj == 0` 跳过**（仪器证据）；复用 `otherSrc` 的方案**失败并已回退**

**仪器**（`EXTC_DBG_FS=1`，两处打印：`noteFieldSrc` 的"归属不明的写"分支、`promoteFieldsAt` 的 `otherSrc` 分支）：

```
[fs-other] nb d2=0 srckind=11      ← 只有 stdlib/prelude 的绑定
[fs-promote] nb kind=11 depth=0 minReq=0 at=0
...  addr4 的 `b` 一行都没有 ✗
```

⇒ **发布对 addr4 根本没执行**。根因：来源实参是**引用型**（`var n: mut ref node = new node`），
在调用点检查的时刻 `exprRefDepth(n)` 与 `placeDepth(n)` **都答 0**
（`refDepth` 还没填上——正是 `checkEscape` 注释里"on `return head`, `exprRefDepth(head)` answers 0
first"描述的同一个现象），于是 `if (dj == 0) continue;` 把它跳过了。
同时生成物仍是 `node * n = extc_arena_alloc(&__extc_a[1], …)` ⇒ 站点没被提升 ✓ 与 UAF 吻合。

**顺带做的一个实验（失败，已回退，但很有价值）**：让 `noteFieldSrc` 接受 `field == NULL`
（给"归属不明的写"配 src 槽）—— 结果**提升家族三例全红**（先抄下名字再回退，按新规则）：

```
FAIL store-promotion （编译/运行失败）
FAIL promote-slice  ->  编译期就报错了
FAIL promote-through-mid  ->  编译期就报错了
```

原因：`noteFieldSrc(..., NULL, ...)` 这条路**被既有的元素写共用**
（`noteFieldDepthWrite(root, NULL, d2)` 是 `a[i] = x` 那条路径），一开就给它加了提升压力。
⇒ **出参发布必须有自己的槽，不能复用 `field == NULL`。**

### 下一轮（两步，证据齐全）

1. **给出参发布专用槽**（例如 `callOtherSrc / callOtherDepth / callOtherMinReq`），
   只在 `checkCallRefArgs` 的发布里写、只被 `promoteFieldsAt` 的新分支读；
   元素写路径**逐字节不变**（零回归风险）。
2. **`dj == 0` 不再跳过**：用内容流循环早就用过的**延迟机制**
   （`recordLvlFact(c, splace, h)` ——"先把'它得活到 h'记成事实，末轮重放"），
   这样"站点电平还没定"的引用型来源也能在末轮被提升；
   若末轮证明"提升不动"，就按"**证不出提升 ⇒ 拒绝**"收口（与"局部不可提升 ⇒ 拒绝"同一规则）。

**当前状态**（本轮末）：build 零告警、`tests/run.sh` **324/0**、`check.sh quick` **54/0**、
闸门⑤ **7 条基线 / 0 新增 / 0 过期**（INV-K 的落地完好）；`promoteFieldsAt` 里那行
`EXTC_DBG_FS` 打印保留（受开关控制，下一轮还要用）。

## 二·补9：**INV-K 落地**（P0-16 的 a/b 修好，addr4 仍欠"提升"）

补8 的两条假设都验证了：

1. **误拒来自 `recordStore` 那一行** —— 去掉它之后 `tests/run.sh` **零失败**（第五次尝试里那两处误拒
   就是它约束电平造成的）。发布只保留 `noteFieldSrc` + `noteFieldDepthWrite` 两半。
2. **`dj` 必须问"内容深度"**：`exprRefDepth(splace)` 优先，为 0 再退回槽位 `placeDepth`
   （引用型来源实参 `n: mut ref node` 的槽位深度是 0，用槽位就会 `continue` 跳过发布）。

**落地的改动**（都在本文件末尾附的规则下逐条 `edit`/即时写入）：

| 位置 | 改动 |
|---|---|
| `Sym`（`check_internal.h`） | `otherSrc/otherSrcDepth/otherMinReq` 三个槽 |
| `noteFieldSrc`（`check_top.c`） | 支持 `field == NULL`（"归属不明的写"有自己的 src 槽） |
| `promoteFieldsAt`（`check_escape.c`） | **方向拆分**：`bool lower = !sy->addressed;` —— addressed 根照常提升、绝不降低；并处理 `otherSrc` |
| `checkCallRefArgs`（`check_top.c`） | 按摘要位把"来源实参的深度"发布到每个 `mut ref` 目的实参的绑定上（`field == NULL`，弱更新） |

**验收**：

| 判据 | 结果 |
|---|---|
| addr1 / addr2 拒绝 | ✓ `this return value would hold a reference to a local variable that dies first (borrowed from depth 1, but this can only hold up to depth 0)` |
| `tests/run.sh` | ✓ **324 / 0** |
| `check.sh quick` | ✓ **54 / 0** |
| 闸门⑤ | ✓ **9 → 7 条基线**（a/b 棘轮出基线）、0 新增、0 过期 |
| `--check-c` 全量 / ASan 全量 | ✓ 全绿 |
| addr4 接受且 ASan 干净 | ✗ **仍 UAF**（见下） |

### 还欠的一步：addr4（`var n = new node` 的形状）

发布与提升的管道都接上了，`otherSrc` 也应该记到了 `b` 上，但 `new` 站点仍停在块 arena
（ASan: `heap-use-after-free … freed by extc_arena_destroy`）。
下一轮：在 `noteFieldSrc` 的 `!field` 分支、以及 `promoteFieldsAt` 的 `otherSrc` 分支各加一行
`EXTC_DBG_FS` 打印，确认 ① `otherSrc` 是否真的记上、② `promoteInto2(n, 0)` 是否被调用并成功；
若"站点不可提升"，则按"证不出提升 ⇒ 拒绝"收口（与"局部不可提升 ⇒ 拒绝"同一规则）。

## 二·补8：INV-K 第五次尝试 —— 方向拆分**是对的**，但仍有两处误拒 + addr4 未提升（已回退）

**这轮做了两件事**：

1. **`promoteFieldsAt` 的方向拆分**（补7 的方案）：`bool lower = !sy->addressed;`
   —— addressed 根**照常提升**，只把"降低 depth"那一步跳过。
2. 重放 INV-K 的其余部件，并把 `dj` 的来源改对：
   ```c
   int dj = exprRefDepth(c, splace);                   /* 先问"所指内容的深度" */
   if (dj == 0 && placeRoot(c, splace)) dj = placeDepth(c, splace);
   ```
   （改前用 `placeRoot ? placeDepth : exprRefDepth`：对 `n: mut ref node = new node` 这种**引用型**
   来源实参，`placeDepth` 答的是槽位深度 0 ⇒ 发布被 `continue` 跳过。）

**结果（部分达成）**：

| 判据 | 结果 |
|---|---|
| addr1 / addr2 拒绝 | ✓ 达成 |
| `examples/store-promotion` 编译 | ✓ 达成（方向拆分消除了上一轮的误拒） |
| `tests/asan/promote-through-mid` 编译 | ✓ 达成 |
| addr4 接受且 ASan 干净 | ✗ **仍 UAF**（提升那一半没把 `new` 站点搬到 home） |
| tests / check.sh quick 全绿 | ✗ `tests/run.sh` 4/2、`check.sh quick` 50/4（**还剩两处误拒，未及定位就回退**） |

**已回退**（工作区与补7 末态逐字节一致：二进制 `67bf0b18…`、324/0、54/0、闸门⑤ 9 条 0 新增）。

### 下一轮的三步（按性价比排序，且**先取证再改**）

1. **先定位剩下那两处误拒**：把改动落地→跑套件→**把失败文件名抄进本文件**，然后再决定收窄方案。
   （本轮的教训：我在还没抄下失败用例时就回退了，等于把最值钱的证据扔了。
   ⇒ 新规则：**回退之前先把红色用例抄进记录**。）
2. **分离 `recordStore` 那一半**：发布里那行 `recordStore(c, splace, place, dat, line)` 是我顺手加的，
   它会约束电平 ⇒ 很可能就是剩下误拒的来源。实验：**只留 `noteFieldSrc`+`noteFieldDepthWrite`**
   （去掉 `recordStore`），看两处误拒是否消失、addr1/2 是否仍然拒绝。
3. **addr4 的提升**：在 `noteFieldSrc` 的 `!field` 分支加一行 `EXTC_DBG_FS` 打印，
   确认 `otherSrc` 到底记没记上、`promoteInto2(n, 0)` 是否成功；再决定是补"站点提升"还是
   按"证不出提升 ⇒ 拒绝"收口（后者与"局部不可提升 ⇒ 拒绝"同一条规则）。

## 二·补7：INV-K 第四次尝试 —— **假设验证成功，但发现第二处 `addressed` 语义错**，已回退

**做法**：按补6 去掉 `addressed` 守卫，其余四步照旧（逐条 `edit`，不再用整段脚本）。

**结果（假设成立）**：

| 探针 | 结果 | 说明 |
|---|---|---|
| addr1 / addr2（`var n: node` 局部） | **拒绝** ✓ | `this return value would hold a reference to a local variable that dies first (borrowed from depth 1, but this can only hold up to depth 0)` —— 正是直接孪生的同一句话 |
| addr4（`var n = new node`） | 接受，**但仍 UAF** ✗ | ASan: `heap-use-after-free … addr4.extc:14 in main`，`freed by extc_arena_destroy` |

⇒ 去掉那条守卫确实是必需的（addr1/2 立刻修好），**但不够**。

**为什么 addr4 还是 UAF**：目的绑定 `b` 被 `ref b` 取址 ⇒
`promoteFieldsAt` 开头 `if (sy->addressed) return true;`（"别名可能写它，表不可信"）
**直接 bail** ⇒ `n = new node` 的站点永远不会被提升到 home arena ⇒ 返回的 `b.p` 指向已释放的块。

**为什么会误拒一片**：`examples/store-promotion.extc`、`tests/asan/promote-through-mid.extc`
正是"把容器交给被调方写、靠**提升**保住安全"的用例 —— 目的绑定同样被取址 ⇒ 提升跑不了 ⇒
我的深度发布把"本可提升"变成"拒绝"（`tests/run.sh` **4/2**、`check.sh quick` **50/4**）。

**⇒ 精炼后的设计（下一轮，改动很小、理由充分）**：`addressed` 必须**区分两个方向**：

| 动作 | 对 addressed 根 | 为什么 |
|---|---|---|
| **降低**已记录的深度 | **继续禁止** | 别名可能写入更深的值 ⇒ 降低就是不健全 |
| **提升** `src`/`otherSrc` 背后的站点 | **必须允许** | 把站点搬到更浅的 arena 只多占内存，**永远安全** |

具体：`promoteFieldsAt` 不要对 `addressed` 直接 `return true`；照常提升 `src`/`otherSrc`，
只把"降低 `depth`"那一步对 addressed 根跳过。

**预期（下一轮的判据）**：addr1/addr2 **拒绝**；addr4 **接受且 ASan 干净**
（站点被提升到 home，与直接孪生 `direct16b.extc` 一致）；`store-promotion` /
`promote-through-mid` 全绿；其余闸门不动。

**本轮收尾**：回退时切口切多了（删掉了 `checkCallRefArgs` 的收尾花括号 ⇒ 编译失败），
已补回；现在 build 零告警、`tests/run.sh` **324/0**、`check.sh quick` **54/0**、
闸门⑤ **9 条基线 / 0 新增 / 0 过期**。基线里 P0-16 a/b/d 三条都在（回到 4/13 的状态）。

## 二·补6：INV-K 的**真正拦路虎已找到**（下一轮一行就能验）

复查上一轮的发布循环，发现**障碍是我自己加的一条守卫**：

```c
Sym *dsym = placeRoot(c, place);
if (!dsym || dsym->addressed) continue;      /* ← 就是这条 */
```

`stash(ref b, ref n)` 里 **`ref b` 正是"取 b 的地址"** ⇒ 目的绑定 `b` 在调用点**必然**
`addressed == true` ⇒ 发布循环每次都 `continue` ⇒ 什么都不发、三条探针一动不动。
这解释了上一轮"摘要对（`addr=0x2`）、入口进了、结果不变"的全部现象。

而 `noteFieldDepthWrite` 里**本来就有**为这种情形准备的分支：

```c
} else if (root->addressed) {                  /* address taken: aliases may write, take max */
    if (d2 > *slot) *slot = d2;
}
```

⇒ 正确做法是**不要跳过 addressed 根**，直接发布（对已取址的根做弱更新取 max，正是设计意图）。

**下一轮的第一件事（一行改动 + 三个探针）**：去掉那条 `addressed` 守卫，
按上一轮记录的四步（`Sym` 槽 / `noteFieldSrc` 支持 NULL / `promoteFieldsAt` 走 `otherSrc` /
调用点发布含 `recordStore`）重做一遍，预期：addr1/addr2 **拒绝**、addr4 **接受且 ASan 干净**。

**本轮的失败是工具问题不是设计问题**：三次插入脚本都在"整段匹配 + 最后统一写文件"的模式上失败
（锚点差一个空行 / 断言的字符串与实际不符 ⇒ 先前的编辑全部丢失）。下一轮改用**逐条 `edit`**
（每次改动自己会报成败），并且**每步只写一个文件**。

## 二·补5：INV-K 第二次尝试 —— 机制搭好了但**没到达判定点**，已回退

**这轮做了什么**（全部按补4 的计划实现，构建零告警）：

1. `Sym` 加 `otherSrc/otherSrcDepth/otherMinReq`（让"整对象/元素写"那条路也有得提升）；
2. `noteFieldSrc` 支持 `field == NULL`；
3. `promoteFieldsAt` 也走 `otherSrc`（提升那一半）；
4. `checkCallRefArgs` 末尾加发布循环：按 `addrMask|contMask|home*` 的位，对每个 `mut ref` 目的实参
   `noteFieldSrc(dsym, NULL, dj, splace)` + `noteFieldDepthWrite(dsym, NULL, dj)` +
   `recordStore(c, splace, place, dat, line)`。

**结果：addr1/addr2/addr4 三条一个都没变**（仍 accepted）⇒ 回退。
回退时曾弄坏一处（`noteFieldSrc` 的 `!field` 守卫被一起删掉 ⇒ `strcmp(NULL)` 崩溃，tests 5/4），
已补回，全绿（324/0、check.sh 54/0、闸门⑤ 9 条 0 新增）。

**这轮真正查清的事（下一轮的起点）**：

- 调用点的摘要是**对的**：`EXTC_DBG_REFARGS=1` 显示 `stash ... complete=1 addr=0x2`（bit1 = 参数 `v`）✓
  ⇒ 发布循环该进的都进了，问题在**判定点**不在入口。
- 返回检查用的是 `int d = exprRefDepth(c, val);`（`check_escape.c:1628`），而
  `exprRefDepthPure` 的 `EX_IDENT` 分支**只跟着绑定的 `origin` 走**
  （"a binding on its own carries no depth independent of where that site ends up"）——
  它**不读**字段表、也不读 `otherDepth`。所以我写的两样（`otherDepth`、`recordStore`）都到不了它。
- 但**直接孪生**（`b.p = ref n` + `return b`）今天确实被拒 ⇒ 直接写那条路走的是**另一个消费者**
  （候选：电平求解器对 `c->stores` 的折叠；或 `promoteFieldsAt` 返回 false 被 `checkEscape` 映射成报错）。
- ⇒ **下一轮第一步不是写代码，而是给直接孪生加仪器**（`EXTC_DBG_FS=1` / 电平 pass 的调试开关 /
  在 `ckError` 处下断），把"到底哪个消费者拒了它"钉死，再把调用点发布接到**同一个**消费者上。
  在此之前不要再猜着改。

**教训（与 INV-C 那次同类）**：这个 checker 对一个事实有**多个查询函数**
（`placeDepth` 读字段表+otherDepth；`exprRefDepth` 跟 origin），
**发布必须发到"做判定的那个查询"能看见的地方**；先找到判定点，再决定发布形态。

## 二·补4：INV-K（P0-16）的设计已完全查清 —— 留下一轮实施

**三条探针的形状**：`fn stash(out: mut ref box, v: ?ref node) { out.p = v }`，
调用处 `stash(ref b, ref n)` 之后 `return b` —— 被调者经出参写进了 `b.p`，
而**调用点不产生任何 publication**，于是 `return b` 看起来"没有活引用"。

**判据（实测的直接孪生，用来定终态）**：

| 孪生写法（在 `f` 里直接写 `b.p = ...`） | 今天的行为 | 说明 |
|---|---|---|
| `var n: node = {v:7}` 然后 `b.p = ref n`；`return b` | **拒绝** ✓ `this return value would hold a reference to a local variable that dies first (borrowed from depth 1, but this can only hold up to depth 0)` | 局部不可提升 ⇒ 必须拒 |
| `var n: mut ref node = new node` 然后 `b.p = n`；`return b` | **接受，ASan 干净**（`rc=42`） | `new` 的站点被提升到 home ⇒ 安全，不该拒 |

⇒ **P0-16 的三条里，addr1/addr2 应"拒绝"，addr4 应"接受且跑干净"**（与直接孪生一致）。
只做"发布深度"那一半会把 addr4 变成**误拒**——这正是主人决定③要避免的。

**实现所需的全部零件（已核对）**：

1. **挂点**：`checkCallRefArgs`（`check_top.c`）里两条循环 —— `ref` 实参那条（约 1330–1349，
   目前只做"深度是否够"的检查）与内容流那条（约 1402–1430）。发布要加在这里：对**每个** `ref`/内容
   流向的目的实参，按 `callee->addrMask/contMask/otherMask` 的位把来源实参的深度**发布**到目的绑定的表上。
2. **有效深度函数会读到它**（这是关键）：`placeDepth` 的 `EX_IDENT` 分支（`check_escape.c:468-490`）
   取 `sy->refDepth`，再对 `sy->fields[fi].depth` 与 **`sy->otherDepth`** 取 max ⇒ 发布进
   `otherDepth` 就能被 `return b` 看见 ✓。
3. **发布辅助**：`noteFieldDepthWrite(Checker*, Sym *root, const char *field, int d2)`（`check_top.c:2316`）
   —— `field == NULL` 走"整对象/元素写"那条，取 `otherDepth = max(otherDepth, d2)` 并
   `refreshRootDepth(root)` ✓ 正好表达"不知道哪个字段"。
4. **提升那一半**：`noteFieldSrc(root, field, d2, src)`（`check_top.c:2282`）记下**源表达式**，
   之后 `promoteFieldsAt(c, sy, at, hops)`（`check_escape.c:828`）在返回路径上把 `src` 背后的
   分配站点提到要求的层 ⇒ 这正是 addr4 能"接受"所依赖的那一半；否则会误拒。
   **待解决的小问题**：`noteFieldSrc` 需要一个**字段名**，而摘要把"哪个字段"抹掉了
   （只说"参数 j 的东西被存了"）。两条路：(a) 取该结构体第一个"类型含引用"的字段名填入；
   (b) 给 `otherDepth` 也配一个 `src` 槽（更干净，但要动 `Sym` 的表结构）。
   建议 (b)：它让"整对象/元素写"这条既有的保守路径第一次带上提升能力，也让下一次类似
   （数组元素写、`dyn` 载荷）不会重复这个坑。
5. **`recordStore`**（`check_escape.c:886`）负责给电平求解器留记录；发布路径要与它保持一致
   （`(val, target, at)` 三元组）。

**下一轮开工顺序**：先写"判据先行"的小实验（上面两条孪生各自应当拒/接受），再动 (2)+(3) 让
addr1/addr2 变拒、然后动 (4) 让 addr4 保持接受；每步跑 `tests/run.sh` + 闸门⑤ + `check.sh quick`。

## 三·修：下一轮的顺序（按"更根本的先做"重排）

| 序 | 工作 | 覆盖 | 为什么先做 |
|---|---|---|---|
| 1 | **INV-K 写点完整**：所有写路径收口到一个 `noteStore()`，`mut ref` 出参由调用点按摘要补 publication | P0-16 ×3 | 它是"记录"的总入口；它建好之后，第 3 步的失效与第 4 步的来源事实才有挂载点 |
| 2 | **逐符号来源事实**：绑定从初始化器收窄（定案 97 的契约），事实挂在 `Sym` 上 | P0-6a、P0-17 | `var q = p` 这类洗白只能靠"q 的来源是借来的"来判，不能靠词法深度 |
| 3 | **INV-F 写点失效**（+ 与泛型实例化共享轮次的不动点、`[budget]`） | P0-17、P0-3、P1-5 | 事实有了之后必须失效，否则是"记一次就算数"的老病 |
| 4 | **INV-C 载体清单**（逐节点、逐条用语料验证；**不**用通用遍历） | P0-1、P0-6a 的载体形状 | 它需要 1–3 建好的记录/失效机制来判断"这个子女算不算内容"；顺序反了就会像本轮一样误拒一片 |
| 5 | INV-I 符号身份（`paramIndex` 之外仍按名字匹配的地方） | P0-6d | 与 4 同族，最后收口 |

## 附：余下 11 条施工单（原顺序，已被上面的重排取代）

| 序 | 洞 | 由哪条不变式修 |
|---|---|---|
| 1 | P0-1 `EX_TRY` 不进逃逸集合 | INV-C 载体穿透 |
| 2 | P0-6a 载体洗白 / 6b `needsHome` 早退 / 6c `placeDepth` 兜底 / 6d 参数按名字 | INV-C / INV-U / INV-I |
| 3 | P0-16 三份（`mut ref` 出参无 publication） | INV-K 写点完整 |
| 4 | P0-17 字段非空证明不失效 | INV-F 写点失效 |
| 5 | P0-3 自由泛型零值检查不记录 | INV-F（与实例化共享轮次） |
| 6 | P1-5 `fresh` 不失效 | INV-F 写点失效 |

顺序理由：INV-C/INV-U/INV-I 是同一族（"看对对象/穿透载体"），**先做它们**能把 6 条一次打掉；
INV-K 与 INV-F 是引擎（写点收口 + 失效），放后面，因为它们是"新机制"，要先把身份与穿透修对。

## 四、验收（本轮末）

见 `check.sh quick`、五道闸门与 `tests/run.sh` 的本轮结果；四道老闸门基线仍为空，
闸门⑤基线 11 条、0 新增、0 过期。

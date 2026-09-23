# ARENA-MEMORY.md —— arena 与「长运行服务」的内存：一次未完成的尝试

> **状态（2026-09-24）：文中的机制描述的是当时那版代码，其中"无条件兜底"那一条已经被删掉了**
> —— 收尾 pass 现在只在**逃逸**时才把站点放进家 arena（`src/check_top.c` 的收尾 pass 与
> `levelPass`）。所以读这份文档时：**§1 的根因、§3 的四个坑、§6 的验收判据仍然有效**
> （它们讲的是设计与判据，不是当前代码长什么样）；**§2 说的"已经做进去的两处"要按现在的代码复核**。
> 这份保留下来，是因为它是"哪条路试过、为什么没走通"的**设计史**，
> 与 [`ARENA-NOTES.md`](ARENA-NOTES.md) 的分工一致：那份放仍然有效的坑，这份放**当时那一轮**的完整经过。

> 起因：主人给的形状（`inner` 有 1/4 概率 append 到 `outer`、1/4 覆盖 `outer2`、
> 一半什么都不做），在 2e6 轮的循环里，**峰值 98 MB**，而同一形状自己内联写容器只要 50 MB。
> 结论先说：**根因找到了，机制也找到了，但落地时踩了四个坑，最后只收了"精度"那两处，
> 内存那一步没做进去。这份文档把"差一步"的确切位置写清楚。**

## 1 根因（已确证，不是猜测）

`__extc_home` 是"调用者选的那只 arena"，可以活到调用者的帧、甚至整个进程。
而**收尾 pass 里有一条无条件兜底**：

```c
/* check_top.c 收尾 pass（HEAD 原文）*/
if (site->kind == EX_NEW || site->kind == EX_GENCALL)
    if (site->arenaLevel != ARENA_HOME) site->arenaLevel = ARENA_HOME;   /* 有家 ⇒ 全进家 */
```

⇒ 只要函数"有家"，里面**每一个** `new` 都被钉进家 arena，
**包括那些根本没逃出本帧的**（每轮造了就用完的临时对象）⇒ 到帧结束才回收 ✗

实测（`/tmp/rg/r1.extc`，2e6 轮，值拷贝 append + `varArray`）：

| 形状 | 峰值 RSS |
|---|---|
| `new` 每轮 + 值拷贝 append（走 `varArray::push`） | **98 MB** |
| 同一形状，容器**自己内联**写（不走 varArray） | 50 MB |
| 每轮只造了又扔（不 append） | 1.7 MB |

50 MB 那一档是**真的活着**的数据（容器里 1e6 × 24B + 倍增垃圾），
所以 98→50 的那 48 MB **全部**是"没逃出去却被钉在家"的每轮垃圾。

## 2 已经做进去的两处（精度，落地了 ✓）

1. **`valDepthForStore`：值拷贝不背源对象的寿命**（`check_escape.c`）
   存进去的是**字节的副本** ⇒ 源对象活多久与目标格子无关。
   判据 `typeCannotCarryRef`（跟 `exprRefDepth` 开头那个早退同一条）。

2. **`stmtStoresThroughDeref`：往参数里写纯值不算"发布"**（`check_top.c`）
   `varArray<T>::push` 的体是 `self.buf[self.len] = v` —— 值拷贝 ⇒
   整条传染链 `push → grow → …` 的第一环断掉，`main` 不再"因为有家而有家"。
   ⚠️ **判据必须是纯语法的**：这个函数跑在**查函数体之前**，那时 `v->type` 还是 NULL ✗
   （第一版依赖 `typeContainsRef` ⇒ 恒为假 ⇒ `fn stash(dest, v) { @overwrite var n = new node  dest.v = n }`
     丢了家 arena ⇒ `dest.v = n` 误拒。这就是 `tests/asan/overwrite-callee` 抓出来的 ✓）

3. **`promoteInto2` 补齐载体形状**（`EX_GENCALL` / `EX_SLICE` / `EX_FIELD` / `EX_INDEX`）
   —— 否则"装着新分配的聚合值"整类提不动（`{ buf: new T[cap], … }` 就是最典型的一处）✗

## 3 没做进去的那一步，以及为什么（四个坑，都实测过）

目标：把收尾 pass 那条无条件兜底改成**按站点判**——只有"真逃出去"的才留在家。
需要回答的唯一问题是：**`ARENA_HOME` 里混了两种东西，怎么分开？**
（"真逃出去了" vs "只是因为所在函数有家"）

| # | 试法 | 结果 |
|---|---|---|
| 1 | 在 `promoteInto2` 里记 `lexicalLevel`（词法层号），收尾 pass 按 `lexicalLevel >= 1` 放回块层 | `fn build() -> mut ref node { var head = new node  return head }` ⇒ `head` **已在 ARENA_HOME** ⇒ `promoteInto2` 走**早退**、没打标记 ⇒ 被放回块层 ⇒ `build` 一返回就 release ⇒ 调用者手里是已释放的链表（`examples/escape-promotion` 死循环 / 求和是乱数）✗ |
| 2 | 在早退处也打标记 | `withCap` 的 `return v?` 分支**绕过** `checkEscape` ⇒ 还是没标记 ✗ |
| 3 | 在 `checkEscape` 开头无条件先 `promoteInto(c, val, at)` | **把一个已经提到家 arena 的站点又拉回块层**（`dest.v = n` 那次 `at=1` 覆盖了 `at=0` 的决定）⇒ `overwrite-callee` / `arena_stale_origin` 误拒 ✗ |
| 4 | 用独立的 `escaped` 标记（只由 `promoteInto` 打）+ 收尾 pass 按它判 | `withCap` 的 `new T[cap]` 走的是 `EX_TRY` 返回分支，`promoteInto` 到不了 ⇒ **必须**同时保留 `RefCheck.depth` 的就地重算；而 `head` 在 `return head` 时 `promoteInto` 也没打到（未查清）⇒ `escape-promotion` 仍坏 ✗ |

**卡住的确切位置**：`build` 里 `return head` 走到了 `checkEscape`（`[ce0]` 有日志），
但**没有**进 `promoteInto2` 的 `EX_IDENT` 分支（`[idx]` 无该条），也没有报错。
⇒ 说明 `promoteInto` 在那条路上返回了 false 而没人管它。
**下一步就从这一句查起**（`check_stmt.c:510` 那句 `promoteInto(c, s->u.ret.value, 0)`）。

## 4 另一条没走的路（也记下来，免得以后重复想）

把 `promoteInto2` 的 `EX_IDENT` 早退（`if (sy->depth <= at) return true;`）去掉，
改成"无论如何都下潜"——能绕过坑 1/2，但代价是**每次提升都要下潜整棵树**，
而且 `sy->depth` 那半本身是"初始化式与槽位同层"的论据，删掉它要重新论证。
没试；如果要试，先量清楚成本。

## 5 复现材料

- `/tmp/rg/r0.extc`：每轮只造了又扔（1.7 MB，紧的 ✓）
- `/tmp/rg/r1.extc`：值拷贝 append + 覆盖（**98 MB** ← 主人的形状）
- `/tmp/rg/r2.extc`：递归函数里每轮 append（18.7 MB）
- `/tmp/rg/r3.extc`：容器自己内联写（50 MB，**这就是目标线**）
- `/tmp/rg/oc3.extc`：`@overwrite` + 出参形状（1.5 MB）

量法：`./build/extc --run X.extc && /usr/bin/time -v ./build/X 2>&1 | grep Maximum`
⚠️ **不要用"`--run` 的 stderr 非空"判失败** —— `--run` 会把程序输出也打到 stderr，
我在这上面误判了三次 ✗ 要看**退出码**。

## 6 验收判据（任何进一步改动都必须同时满足）

1. `tests/run.sh`：通过 255 / 失败 0
2. `tests/arena-soundness/run.sh`：洞还在 = 0（3 条该拒的仍然拒）
3. `tests/arena-promoted/run.sh`：22/22，且 ASan 干净
4. `tests/asan/run.sh` = 8、`tests/arena/run.sh` = 5
5. `tests/attacks/` 通过集合与 `BASELINE` **一字不差**
6. `tools/golden.sh check`：差异必须**逐条说得清**（本次改动 = 0 字节变化 ✓）

跑法：`bash tools/arena-fix/check.sh`（⚠️ 全套 ~7 分钟；**中间要能看进度**，
我只用 `tests/run.sh` 就抓到了 `examples/escape-promotion` 的死循环 ✓）

---

# 附录 A：约束解算（层 2）—— 机制已跑通，验收未过（2026-09-23 晚）

## A.1 做成了什么

按"数据流"的思路重写了分配归属的决定，**两条都实现并测过**：

1. **记账**：`promoteInto2` 的入口顺手记一条事实 `(值, 目标层号)`（`LvlFact`），
   并把它**取最小**写进站点自己的 `Expr.minAt`。
   - 关键：**不另写遍历去找存储点** —— 每一处"往外存"本来就要过 `promoteInto`，
     所以"存储点"和"约束点"是同一段代码的两个副作用，物理上不可能分叉 ✓
2. **解算**：收尾处把事实表**反复重放到不动点**（`promoteInto` 只往"更长寿命"改 ⇒ 单调 ⇒ 必然收敛）。
   实测：`r1` 43 条事实，**1 轮收敛**（`EArenaSite` 那个"跑四轮"是同一件事）。
3. **定案**：`minAt == 0` ⇒ 家 arena；`minAt >= 1` ⇒ 第 k 层块口袋；`minAt == -1`（没被任何约束碰过）
   ⇒ 回到它自己那层 `lexicalLevel`。

**实测：`r1` 的 98 MB → 50.8 MB**（和"容器自己内联写"的 50.8 MB **完全一致** ⇒ 每轮临时对象
真的每轮回收了）。生成 C 里 `it` 落在 `__extc_a[2]`（循环体那层 ✓），`grow` 的缓冲区留在家里
（它确实要活到容器那一级 ✓）。

## A.2 三个坑（都实测踩到）

| # | 现象 | 根因 |
|---|---|---|
| 1 | **编译器自己被 OOM kill** | `recordLvlFact` 没排除"解算期" ⇒ 重放一次又记一条 ⇒ 事实表无限长 × 32 轮。修法：`Checker.lvlSolving` 旗子 + 快照 `nFacts` ✓ |
| 2 | `examples/alloc-in-block` 生成的 C 编不过（`__extc_home` 未声明） | 解算把站点定成"进家"，而 `needsHome` 是**解算之前**算的 ⇒ codegen 没吐隐藏参数。修法（已验）：没家的函数不许把站点留在家（检查阶段本来就该报错了）✓ |
| 3 | `generic_return_local`（**应该报错**）通过了 ⇒ **健全性问题** | 还没查清。这一条**不能留**，是本次回退的直接原因 ✗ |

## A.3 下一步（从这里接）

1. **先修坑 3**：`tests/errors/generic_return_local.extc`（泛型体 `return local`，`T = slice<u8>`）
   必须重新拒掉。怀疑点在"解算改变了检查期读到的深度"这条链上。
2. **再修坑 2 的正解**：解算之后**重跑一次 `needsHome` 的传递闭包**
   （现在是"没家就不许留家"的兜底；正解是让 `needsHome` 反映解算结果）。
3. 然后重跑 A.4 的判据。

⚠️ 剩下的 4 条失败（`container-nested` / `container-of-struct` / `print-desc-nested` / `varArray-asSlice`）
都是**读出来是垃圾** —— 怀疑同一个"内层容器的 buf 被放在太浅的口袋"的形状，跟坑 3 很可能同源。

## A.4 判据（与 §6 相同，一条都不能少）

`tests/run.sh` 255/0 · 反例库洞还在=0 · 转正库 22/22 · asan 8 / arena 5 · 攻击库与 BASELINE 一字不差

## A.5 环境教训（这次浪费了好几轮）

- **`/tmp` 是 tmpfs**：shell 一崩就清空 ⇒ 测试程序必须放 `build/tmp/`（仓库内、不会被清）✗
- **别在一条命令里 `sleep` 很久**等后台任务（本会话被当成挂住，反复重置 shell）；
  启动后台任务后**立刻返回**，下一次调用再查日志 ✓
- 一次编辑别用大段 heredoc + 脚本重写整文件：这次 `ast.h` 被削掉 337 行、
  `check_top.c` 的一处插入被崩掉吞掉，各浪费一轮 ✗

---

# 附录 B：第二次实施（约束解算）—— 机制通、健全性修好、剩一族误拒（2026-09-23 夜）

> ⚠️ **先读这一条**：本附录里说的代码在**分支 `lvl-solver`** 上（`e4642d3` + `6d90954`），
> **不在 `main`**。`main` 是绿的（`tests/run.sh` 255/0）。

## B.1 ⚠️⚠️ 环境的坑：会话为什么会莫名重置（浪费了好几轮，务必先看）

现象：会话突然 `[shell exited: code 1]`，工作目录被重置。我先后误判成
"编译器 OOM"、"`sleep` 坏了"，**都不对**。真凶是宿主截图里的这条：

```
EISDIR: illegal operation on a directory,
realpath '\\wsl.localhost\Ubuntu-26.04\home\alphayang\extC_Compiler\.git'
```

harness 在操作前后对一批路径做 realpath，而它**把 `.git` 这个目录当文件**去 realpath
⇒ EISDIR ⇒ 会话重置。在 `/tmp` 下搭同样的 `.git` 目录结构**复现不出来** ⇒
是 **WSL UNC 路径（`\\wsl.localhost\…`）+ 目录**这个组合的问题，跟被执行的命令无关。

**后果（我踩的）**：崩溃时机随机 ⇒ 看起来像"某条命令有毒" ⇒ 我反复换写法，
方向全错。**副作用还有两个更要命的**：
1. **`/tmp` 是 tmpfs，会话一崩就清空** ⇒ 我放在那儿的测试程序反复消失，
   好几次"编不过"其实是文件没了 ✗ ⇒ **测试程序一律放 `build/tmp/`（仓库内）** ✓
2. **`git stash` 也丢过一条**（崩在 stash 过程中）⇒ 别依赖 stash 保存成果，
   **要留的改动要么 commit，要么 `git stash create` 之后记住那个 commit 号** ✓
   （这次是从 `git fsck --lost-found` 的悬空提交里捞回来的）

**绕法**：在 WSL 侧用一个**不含 `.git`** 的镜像目录跑命令；或修 harness 让 realpath 能接受目录。

## B.2 做成了什么（机制已验证）

| 项 | 结果 |
|---|---|
| **约束解算机制** | ✅ 通。`r1`（主人的 50/25/25 形状，2e6 轮）**98 MB → 50.8 MB**，与"容器自己内联写"的**目标线一分不差** |
| 生成 C | `it` 落在 `__extc_a[2]`（循环体那层 ⇒ **每轮回收** ✓）；`grow` 的缓冲区留在家（它确实要活到容器那一级 ✓） |
| 收敛速度 | 43 条事实，**1 轮收敛**（`promoteInto` 单调 ⇒ 必然收敛 ✓） |

实现（三块，都在 `lvl-solver` 分支）：
1. **记账**：`promoteInto2` **入口**顺手记 `(值, 目标层号)`（`LvlFact`），取最小写进 `Expr.minAt`。
   - 关键设计：**不另写遍历去找存储点** —— 每一处"往外存"本来就要过这个函数，
     所以"存储点"和"约束点"是同一段代码的两个副作用，**物理上不可能分叉** ✓
2. **解算**：收尾处把事实表**重放到不动点**（上限 32 轮，与 `promoteInto` 自己的环保护同数）。
3. **定案**：`minAt == 0` ⇒ 家；`minAt >= 1` ⇒ 第 k 层块口袋；`minAt == -1`（没被碰过）⇒ 回 `lexicalLevel`。

## B.3 修掉的坑（都实测过）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | **编译器自己被 OOM kill** | `recordLvlFact` 没排除"解算期" ⇒ 重放一次又记一条 ⇒ 事实表无限长 × 32 轮 | `Checker.lvlSolving` 旗子 + 快照 `nFacts` ✓ |
| 2 | `examples/alloc-in-block` 生成的 C 编不过（`__extc_home` 未声明） | 解算把站点定成"进家"，而 `needsHome` 是**解算之前**算的 ⇒ codegen 没吐隐藏参数 | 兜底：没家的函数不许把站点留在家 ✓ **正解仍是解算后重跑 `needsHome` 闭包** |
| 3 | **`tests/errors/generic_return_local`（该拒）通过了 ⇒ 健全性问题** | 我的"重算"把**绑定**（`EX_IDENT`）的深度算成 0 —— 那个 0 是"站点还没定"的哨兵态，不是真值 ⇒ 实例复查把它放过了 | 重算**只允许**用在"深度真由分配决定"的形状上（`depthComesFromAlloc`），其余一律不下调 ✓ |
| 4 | `alloc<T>` 站点被当成"要活到帧外" | `Expr.minAt = -1`（未定初值）只在 `new` 那一支写了，**`alloc` 那一支漏了** ⇒ `arenaAllocZero` 的 0 被当成"frame-external" | 两支都写 ✓ |

## B.4 剩下的问题（一件事，位置已锁定）

**症状**：`varArray` 那族**误拒**，共 12 条
（`container-nested` / `container-of-struct` / `container-of-view` / `instance-closure` /
`list-return` / `out-param` / `print-desc-nested` / `ref-field-write` / `varArray*` …）：

```
in instance `varArray_slice_u8`: this return value would hold a reference to something
that dies first (depth 1, but this can only hold up to 0)
```

**已锁定的根因**（诊断输出）：

```
[at] varArray_slice_u8 what=this return value depth=1 at=0 kind=4 dca=0 svd=0
```

- `kind=4` = `EX_IDENT` ⇒ 报错那条记录的 `rc->val` 是个**绑定**（`var v = …` 里的 `v`）
- 它的深度其实由**它的来路**（`var v: varArray<T> = { buf: new T[cap], … }` 里的 `new T[cap]`）决定
- 而 `depthComesFromAlloc` 原来对绑定答 false ⇒ 重算被跳过 ⇒ 用了解算前的旧数 1 ⇒ 误拒

**已经写好的修法（`6d90954`，未验证完）**：`depthComesFromAlloc` 改成带 `Checker*` 的
`depthComesFromAlloc2`，对 `EX_IDENT` **跟着 `sym->origin` 走**（`EX_DEREF` / `EX_FIELD` 同理）。
⚠️ **必须用带 checker 的版本**：wrapper 传 `NULL` ⇒ 绑定答 false
（我第一版就用了 wrapper，`[at]` 里 `dca=0` 一直不变，白查一轮 ✗）。

**下一步就一件事**：把这条走通（跟着绑定来路），重跑 B.5 判据。
⚠️ 判据里 **`generic_return_local` 必须仍然被拒** —— 它是"什么都放宽"的哨兵。

## B.5 判据（与 §6 / A.4 相同，一条都不能少）

`tests/run.sh` 255/0 · 反例库洞还在=0 · 转正库 22/22 · asan 8 / arena 5 ·
攻击库与 `BASELINE` 一字不差 · `tools/golden.sh check` 差异逐条说得清

## B.6 教训（给下一次的自己）

1. **先修环境，再修代码**。会话反复重置时，第一件事是看宿主报的**原文**，
   不要猜"是不是我把它跑爆了"。这次猜错两轮。
2. **测试程序放仓库里**（`build/tmp/`），别放 `/tmp`。
3. **要留的改动马上 commit 或记下 commit 号**，别指望 stash 活着。
4. **一次调用只做一件事**（改源 / 构建 / 跑测试分开）：
   崩在"改完源又 make 又跑"的调用上时，我分不清是三者哪一个。
5. **诊断打印要打"决定分支的那个数"**，不是"我觉得可疑的数"：
   这次真正定位到的是 `dca=0`（决定"跳不跳重算"的那个布尔），
   在此之前我打了一堆 `frozen/now/kind` 都没用。

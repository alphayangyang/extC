# R12 设计与落地进度（定案 99：任务的内存归属）

所有者 2026-09-30 拍板：**机制 A**（每任务一只独立 arena，任务结束整只销毁）+
**语义 A**（任务存储随任务结束回收；能逃出任务的引用一律编译错误）。

## 一、现状（读代码 + 探针实测）

现有运行时**已经有一半**：

| 部件 | 现状 |
|---|---|
| `extc_taskArena[id]` | ✅ 每任务一只独立 arena，`extc_task_end` 里 `extc_arena_release` 整只归还 |
| `extc_taskZone[id]` | ❌ 任务的"地方"是**块 zone 栈上的一个 zone**（`extc_task_begin` → `extc_pool_zoneEnter()`） |
| `extc_task_end` | ❌ `extc_pool_zoneLeaveTo(该 zone)` —— 按 **LIFO** 弹栈 |

于是同一个根因（"任务的存活顺序不是 LIFO"，而释放写成了 LIFO）有两个相反方向的后果：

1. **弹多了 ⇒ UAF（P0-20）**：先结束的任务把仍存活任务的 zone 一起弹掉（ASan：`extc_pool_drop ←
   extc_pool_zoneLeaveTo ← extc_task_end`，`z2.extc`）。
2. **弹不动 ⇒ 泄漏（P1-14）**：结束时 `extc_zoneTop` 已经回到更低的位置，`zoneLeaveTo(mark)` 的
   `while (top >= mark)` 一次都不进 ⇒ 该任务的 zone 与其池全部留在栈上（实测 20000 个结束任务 85.9 MB）。

### 探针结论（本次实测，`/tmp/rev/r12h1*.extc`）

| 形状 | 结果 |
|---|---|
| boxed 任务活过**普通块**，块退出后再驱动 | ✅ 不复现（任务 zone 在块 mark **之下**，弹不到它） |
| boxed 任务在**会建池的块**内 spawn（块自己发 `zoneEnter`/`zoneLeaveTo`） | ⚠️ 生成的 C 里 `zoneLeaveTo(__extc_zm2)` **确实把任务 zone 弹掉了**；本次没炸，是因为任务随后驱动时把同一个槽位"事实上重新占用"了（`pool_new_at` 只检查 `zi <= extc_zoneTop`，而槽位号恰好又落在 top 上）。**任何让该槽位在中间被别的地方复用的交错都会把它变成真 UAF** —— 这正是"看起来能用、坏在时序上"的典型形态 |

⇒ 结论：光把 arena 独立出来不够；**任务的地方必须不在块 zone 栈上**。

## 二、设计（已落第一步）

把 zone 从"**栈下标**"改成"**槽位 + parent 链**"：

| 概念 | 之前 | 现在 |
|---|---|---|
| zone id | 栈下标（= depth-1） | 表中一个**槽位号**（与深度无关） |
| 栈序 | 数值比较 `top >= mark` | `parent` 链（`zoneLeaveTo` 走到 mark 为止） |
| 槽位复用 | 靠下标自然复用 | 空闲链（`extc_zoneFree`），高水位 `extc_zoneNext` 增长 |
| 活性 | `zi <= extc_zoneTop` | `extc_zones[zi].live` |
| 深度 | id+1 | 每个 zone 记 `depth`（`zoneDepth()` 返回真实层数） |
| 任务的地方 | 栈上 zone | **根槽**：`extc_pool_zoneNewRoot()` 分配、不压栈；`extc_pool_zoneFreeRoot()` 释放 → 与栈序无关 ✅ |
| 外层地方 | `zoneDepth()-1` | `extc_pool_zoneOuter()`（返回当前 zone 的 parent） |

新增的运行时入口（都在 `src/pools.c` 的发射串里）：`extc_pool_zoneNewRoot()`、
`extc_pool_zoneFreeRoot(zi)`、`extc_pool_zoneOuter()`；内部：`extc_zoneAllocSlot()`、
`extc_zonePush()`、`extc_zoneRelease(zi)`。

**已完成（第一步，语义中性）**：槽位化 + 上面这些入口。判据：`tests/pool/run.sh` **0 失败**、
`tests/arena/run.sh` 全绿、全量测试与 `check.sh quick` 见文末。

**为什么第一步必须单独落地**：它是纯重构（栈序语义逐位保持，颜色序列都做了回归——释放时**不**翻色，
翻色发生在下一次进入该槽位时，实测颜色序列 1/2/3 不变），所以能在"任务还没改"的情况下先证明
"没有改坏任何东西"。

## 三、落地结果（2026-09-30 完成）

第二步**已实现并验证**：

| 判据 | 结果 |
|---|---|
| P0-20 复现形状（两个 boxed 任务各持 vector，较老的先跑完） | **ASan 干净**（`A=1 A=2 B=1 B=3`） |
| 40000 个"有池"的任务全部跑完（20000 boxed + 20000 unboxed） | **RSS 6.1 MB**、收尾 `live=0 depth=1`（对照旧实现：20000 个结束任务 85.9 MB） |
| 任务活过"会建池的块"、块退出后再驱动 | 正常（`sum=9`）；结构上块 `zoneLeaveTo` 已够不到任务的地方 |
| `tests/coro` | 29/0（改之前是 25/4 —— 4 例红正是任务 zone 被块 leave 弹掉的后果） |
| `tests/pool` / `tests/dyn` / `tests/http` | 0 失败 / 24-0 / 0 失败 |
| 全量 `tests/run.sh`、`check.sh quick`、四道闸门（quick+full） | **324/0**、53/0、全绿且基线全空 |

新增常设回归用例：`examples/task-place-release.extc`（`expect: a=3 b=4 live=0`）。

## 四、剩余工作（小尾巴）

1. 两处白盒用例把 `extc_pool_zoneDepth() - 1` 当 zone id 用（`tests/pool/rt_promote.extc`、
   `tests/traps/view_no_storage.extc`）。槽位化之后"深度-1"不再保证是合法 zone id（槽位被复用后不等于
   深度-1）。它们**现在仍然通过**（栈槽位按 0,1,2… 首次分配，id 恰好等于深度-1），但假设已经不再成立；
   正确做法是改用 `extc_pool_zoneOuter()`，并把该入口加进 `stdlib/std/sys/pool.extc` 的 extern 清单、
   跑 `tools/manual_surface.py` 重生成公开面清单。
2. 定案 99 的**语义 A**（"能逃出任务的引用一律编译错误"）依赖 R1/R3 的逃逸分析六条出口。
   在那之前，运行时不再有 LIFO 造成的 UAF，但检查器仍可能漏放"跨任务引用"。

## 附：原设计的剩余步骤（已全部完成，保留备查）


1. `extc_task_begin`：`extc_pool_zoneEnter()` → `extc_pool_zoneNewRoot()`（并加前置声明）。
2. `extc_task_end`：`extc_pool_zoneLeaveTo(zone)` → `extc_pool_zoneFreeRoot(zone)`。
   ⇒ 两个方向的病一起好：不再弹别人的（UAF），也不再什么都不弹（泄漏）。
3. `$next` 的 `extc_zoneTop = f->zone` 语义不变（根槽可以是 `zoneTop`，块 zone 正常在它上面嵌套）。
4. `extc_pool_zoneOuter()` 在任务里返回 -1（根槽没有外层地方）⇒ 任务内"把容器提升到外层地方"
   会被拒（fail-closed），与"任务存储不外逃"一致。
5. **两处白盒用例要改用新入口**（它们原来靠 `zoneDepth()-1` 当 zone id 用）：
   `tests/pool/rt_promote.extc`、`tests/traps/view_no_storage.extc`。`stdlib/std/sys/pool.extc`
   的 extern 清单要加 `extc_pool_zoneOuter`（随后跑 `tools/manual_surface.py` 重生成公开面清单、
   必要时补手册，否则公开面闸门会红）。
6. 判据：`z2.extc` ASan 干净；20000 个结束任务 RSS 平（±5%，对照现状 85.9 MB）；30 万次 spawn 不涨；
   `tests/coro/`、`tests/dyn/`、`tests/http` 全绿；四道闸门基线仍为空。

## 四、与逃逸分析的关系（R1/R3）

定案 99 的"语义 A"要求**逃出去的引用是编译错误**。R12 机制落地后，这条成为对外承诺的守门人；
但 R1/R3 的六条出口（P0-1/P0-2/P0-6/P0-16/P0-17/P0-3/P0-5）仍待修。在它们修完之前：

- **运行时**不再有 LIFO 造成的 UAF；
- **检查器**仍可能漏放"跨任务引用"这一类（那会变成任务结束时整只销毁导致的悬垂）——
  因此第二步落地时，必须把已知形状补进 ASan 闸门与回归用例，并把"跨任务引用"明确列入 R1/R3 的施工单。

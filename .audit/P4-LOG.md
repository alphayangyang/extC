# P4 批次进展（2026-09-30）

按主人当天拍板的三条定案施工：**101**（`for` 上界与循环条件）、**100**（嵌套预算进手册）、
**99**（R12 任务内存归属，机制 A + 语义 A）。三条都落地，四道闸门基线仍为空。

## 一、定案 101①：`for i in lo..hi` 的上界**进入循环时求值一次**

| 项 | 内容 |
|---|---|
| 改动 | `parser.c` 的 range 脱糖：上界存进隐藏 `let __extc_hi`，条件读它（不再把上界表达式直接放条件里） |
| 后果 | `for i in 0..n { n = 0 }` 现在按进入时的 `n` 跑完；`for i in 0..f()` 的 `f()` **只调一次** |
| 证据 | 新正例 `examples/for-range-bound.extc`（`calls=1 rounds=3 once=5 twice=1 len=4`，含 `while` 对照）；全量测试 324/0；生成物 `int32_t __extc_hi = n;` 在循环之前 |

## 二、定案 100：嵌套上限写进手册

`docs/manual/15-errors.md` 新增「9.0 编译器的资源上限（成文限制）」：块/表达式 **64 层**、
类型 **96 层**、超限给带位置的诊断；并写明这是**实测校准的临时值**（release ~250 崩、ASan ~96 崩），
把递归改成显式栈之后会提高。HTML 已用 `tools/build_manual.py` 重生成，
`check_manual.py --gate` 与 `check_manual_examples.py --gate` 均通过。

## 三、定案 99 / R12：任务的内存归属（**机制 A + 语义 A 的机制那一半**）

详见 [`.audit/R12-DESIGN.md`](R12-DESIGN.md)。要点：

**根因**：`extc_taskArena[id]` 早就独立，但任务的**地方（zone）**挂在**块 zone 栈**上，
`extc_task_end` 用 `extc_pool_zoneLeaveTo` 按 **LIFO** 弹 ⇒ 同一根因两个反向后果：
先结束的任务弹掉仍存活任务的 zone（**UAF**，P0-20）；调用者 zone 更低时一个都不弹
（**每任务泄漏 ≈4.2 KB**，P1-14 实测 20000 个 85.9 MB）。

**两步落地**：

1. **zone 槽位化**（语义中性）：zone id 从"栈下标"改成"表中槽位"，栈序交给 `parent` 链，
   `zoneLeaveTo` 按链走到 mark；活性检查从 `zi <= zoneTop` 改成 `live` 标志；
   `zoneDepth()` 返回真实层数。判据：`tests/pool/run.sh` 0 失败、颜色序列 1/2/3 逐位不变。
2. **任务的地方 = 根槽**（不在块栈上）：`extc_pool_zoneNewRoot()` / `extc_pool_zoneFreeRoot(zi)`；
   `extc_task_begin/end` 改用它。⇒ 释放与栈序**无关**：既不弹别人的，也不什么都不弹。

**判据（实测）**：

| 项 | 结果 |
|---|---|
| P0-20 复现形状（两个 boxed 任务各持 vector，较老的先结束） | **ASan 干净** |
| 20000 boxed + 20000 unboxed 任务全部跑完 | **RSS 6.1 MB**、`live=0 depth=1`（对照旧实现 85.9 MB） |
| `tests/coro` | **29/0**（改前 25/4：4 例红正是任务 zone 被块 leave 弹掉） |
| `tests/pool` / `tests/dyn` / `tests/http` | 0 失败 / 24-0 / 0 失败 |
| 新增回归用例 | `examples/task-place-release.extc`（`expect: a=3 b=4 live=0`） |

## 三·补：P1-13 同族（第二轮的四个泄漏）

第一轮把"任务的地方"做成根槽后，追 P1-13（泛型句柄不结束）时又查出**同族的三处**，一并修掉：

| # | 缺口 | 后果（实测） | 修法 |
|---|---|---|---|
| 1 | `coroCheckDeferred` 只给**非模板**函数算 `coroNeedsZone`（`cf->tmpl` 被跳过） | 泛型协程的分配落在**调用者**的 zone 里 | 补一趟循环：实例取模板刚算好的答案（`cf->tmpl->coroNeedsZone \|\| cf->coroBoxed`） |
| 2 | `$next` 的**发射**也跳过实例（`f->tmpl` 那一项） | 句柄调度直接调 `$step`：既不切任务的地方、也**从不** `extc_task_end` ⇒ `live=20000` | 发射条件改成只排除"还有类型参数的模板"（`f->typeParams.len > 0`）；调度处的 `coroNeedsZone ? $next : $step` 随之走 `$next` |
| 3 | `extc_task_end` 用 `extc_arena_release`（**保留一个 spare 块**），而每任务一只 arena ⇒ spare 无人复用 | 200000 个结束的 boxed 任务 RSS **29 MB**（≈147 B/任务 = 一帧块） | 按定案 99 换成 `extc_arena_destroy`（release + `free(spare)`） |

**实测（修前 → 修后）**：泛型+池 20000 任务 `live=20000` → **`live=0`**、RSS 9.5 MB → **2.3 MB**；
20000 boxed + 20000 unboxed 有池任务 6.1 MB → **3.0 MB**；200000 个 boxed 平凡任务 29 MB → **7.6 MB**
（剩下的 7.6 MB 是**任务表随累计 spawn 增长**，属 R12 裁定第 6 条"槽位复用"，未做）。

## 四、验收

| 项 | 结果 |
|---|---|
| `./tests/run.sh` | **324 通过 / 0 失败** |
| `./check.sh quick` | **53 节全绿** |
| 闸门① 全语料 `--check-c`（gcc+clang） | 0 失败，基线空 |
| 闸门② 正例 ASan+UBSan | 0 失败，基线空 |
| 闸门③ 差分 fuzz | 0 失败，基线空 |
| 闸门④ 规模/嵌套 | 0 失败，基线空 |

## 五、剩余（已记录，未做）

1. **两处白盒用例的 zone id 假设**：`tests/pool/rt_promote.extc`、`tests/traps/view_no_storage.extc`
   用 `zoneDepth() - 1` 当 zone id。槽位化后该等式只在"栈槽位首次分配"时巧合成立。
   正确做法：加 `extc_pool_zoneOuter()` 到 `stdlib/std/sys/pool.extc` 的 extern 清单、改用它、
   跑 `tools/manual_surface.py` 重生成公开面清单。（**不是**当前缺陷：两处现在都通过。）
2. **定案 99 的语义 A 另一半**（逃出任务的引用 = 编译错误）依赖 **R1/R3** 的六条逃逸出口
   （P0-1/P0-2/P0-6/P0-16/P0-17/P0-3/P0-5）。运行时侧已不再有 LIFO 造成的 UAF。
3. **定案 101②**（循环条件里的分配按轮释放）——**已落地（2026-09-30 第三轮）**。

   **上一轮我记错了，这里更正**：当时报"三种探针都测不到跨轮累积"是**测量假象** ——
   我用了 `ulimit -v 4000000` 且只 `tail -1`，把 trap 消息与程序输出一起吞了。去掉 `ulimit` 后
   真相立刻出来：

   | 轮数（条件里每轮 `new i64[1000000]` = 8MB） | 修前 RSS | 修后 RSS |
   |---|---|---|
   | 200 | 7.6 MB | 9.4 MB |
   | 2000 | 9.5 MB | —— |
   | 20000 | **81.5 MB** | **9.3 MB** |

   （每轮只触碰首页 ⇒ RSS ≈ 4KB/轮，正是 20000×4KB = 80MB 的来源；虚拟地址空间则按 8MB/轮增长。）

   **修法**（与上一轮记录的设计一致）：
   - 检查器：`ST_WHILE` 检查条件时 `pushScope`（条件里的分配站点因此拿 **L+1**），并用新增的
     `Checker.allocSites` 计数前后差记下 `Stmt.condAllocs`；
   - codegen：只在 `condAllocs` 为真时把循环发成
     `while (1) { extc_arena_release(&__extc_a[L+1]); <前缀>; if (!(cond)) break; <body> }`，
     并把整个循环的 `blkLevel` 抬到 L+1（与检查器一致）；`blkMaxLevel` 对这种形状多记一层。
     没有分配的条件走原路 —— **其余生成物逐字节不变**；
   - 常设回归：`tests/arena/cond_alloc.extc`（该套件把虚拟内存卡在 150MB；条件里每轮 1MB × 300 轮，
     修前必 `out of arena memory`，修后 `rounds=300` ✓）。

4. **R12 裁定第 6 条**：任务表槽位复用（`extc_taskNext` 只增不减 ⇒ RSS 随**累计** spawn 增长；
   200000 次 spawn 实测 7.6 MB）。

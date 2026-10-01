# extC 编译器全量审阅报告

> 审阅对象：`src/`（C11 实现，38696 行，22 个 `.c` + 16 个 `.h`），外加驱动、生成物与 stdlib 的交互面。
> 审阅日期：2026-09-30。审阅方式：**逐行阅读 + 实测复现 + 交叉验证**（详见 §1）。
> 本报告的判据只有一条：**代码实际做什么**。仓库里的注释、`docs/` 与被引用为前提的"定案"一律不采信；
> 每条问题都给出可复现命令与观察到的输出。

---

## 0. 结论摘要

**测试全绿不等于编译器没坏。** 审阅开始时的实测基线：

| 基线 | 结果 |
|---|---|
| `./tests/run.sh` | **311 通过 / 0 失败**（其中 `tools/parrun.py` 三套语料 306：examples 101 + errors 191 + traps 14；其余为 arena/warnings/asan/annot 等分节） |
| `./check.sh quick` | **51 节通过 / 0 失败** |
| 编译器自身 ASan+UBSan 跑全语料（961 个 `.extc`） | 0 条报告 |
| 变异 fuzz（1858 个用例） | 0 次崩溃 |

而全量审阅 + 定向攻击得到的是：**173 条真实问题**（审阅单元自评级别：P0 18 · P1 65 · P2 48 · P3 42）。
经我复核、合并重复项、并把"静默错值/生成物内存不安全"的几条从 P1 提为 P0 之后，本报告按
**22 条 P0**（正文逐条详述）；P1 65 条（§3.7 全表）、P2 48 条（§4）、P3 42 条（§5）；完整索引见附录 B。
它们全部落在回归用例的盲区里：测试只覆盖"作者想到的形状"，类型/生命周期/代码生成的**组合面**几乎没被覆盖。

> 级别口径见 §1.3。"P0" 指后果（崩溃、内存不安全、静默错值），不指触发频率。

> **配套文档**：审计中"不是写错了、而是没定过/定过但过时"的那批问题，已收敛为
> [`docs/DESIGN-RESOLUTIONS.md`](../DESIGN-RESOLUTIONS.md)（18 条设计裁定 R1–R18，含 6 条新拍板、
> 决策优先级规则、与既有定案的对照、以及必须回填的 22 处过时文本清单）。修代码前先读它。

### 0.1 最严重的一批（每条都能复现）

| # | 问题 | 后果 |
|---|---|---|
| P0-12 | **f64 整数值字面量被发成 C 整型常量**（`codegen.c:2203`，`%.17g`） | `1.0/2.0` 生成 `(1 / 2)` = **0**；`1e9*1e9` 在 `int` 里溢出；`-0.0` 变 `+0.0` |
| P0-7 | 数组 `==` 的临时量被提到 `while` 条件之外（`codegen.c:1194` + `:4053`） | 条件只求值一次 → **死循环 / 结果错**（`rounds=101`） |
| P0-15 | `for` 脱糖把步进放在循环体末尾，`continue` 跳过步进（`parser.c:2343/2394/2457`） | **任何含 `continue` 的 `for` 都死循环**（`--run` 超时） |
| P0-1 | `call(...)?`（`EX_TRY`）不进逃逸集合（`check_top.c:2022`） | 生成物真实 **heap-use-after-free**（ASan） |
| P0-2 | 调用点 arena 深度把 `0` 同时当"未设置"与"参数/全局"（`check_top.c:6024`，重复于 `:6330`） | 生成物真实 **heap-use-after-free**（ASan） |
| P0-16 | 被调者经 `mut ref` 出参写入调用者对象，不产生任何 publication（`check_internal.h:497` 的契约没有实现） | 生成物真实 **heap-use-after-free**（ASan） |
| P0-6 | 借用经"载体"洗白后写入全局（`check_escape.c:649/147/726/1447` 四条逃逸漏洞） | 全局持有指向已死栈帧/arena 的视图；ASan 确认 UAF |
| P0-17 | 字段非空证明在 `self: mut ref` 方法调用后不失效（`check_lookup.c:105`） | 生成物解引用 NULL → **SIGSEGV**（ASan） |
| P0-21 | worker 用到的共享 helper 被全程改走 TLS arena（`check_expr.c:149`） | 主线程调用同一 helper → 生成物 **NULL 解引用 SEGV**（ASan） |
| P0-22 | `dropRuntimeDefs` 的 `continue` 跳过循环推进（`codegen.c:5798`） | 编译 `stdlib/stl/hashSet.extc`（52 行）**永不返回**：100% CPU 死循环，无诊断 |
| P0-20 | 任务结束时按 LIFO 弹"任务的地方"（`coroutine.c:84`） | 弹掉仍存活任务的 zone → 生成物 **heap-use-after-free**（ASan） |
| P0-13 | `p[i]` / `p[a..b]` 对 `?ref [N]T` 不做非空检查（`check_expr.c:1684`，同源 `:1749`） | 生成物解引用 NULL → **SIGSEGV**（ASan） |
| P0-3 | 自由泛型函数 `fn mk<T>() -> T { var b: T return b }` 的零值检查从不记录（`check_top.c:4865`） | 静默生成 `(cellfn){0}`，**调用空函数指针 → SIGSEGV** |
| P0-18 | `i8`/`i16`/`u8`/`u16` 的算术在 C `int` 宽度算且不回截（`codegen.c:1379`） | `a+a` 是 200 而 `var x: i8 = a+a` 是 −56，**`a+a == x` 为 false**（类型系统自相矛盾） |
| P0-19 | `return e?` 的载荷从不做相容性比较（`check_stmt.c:1085`） | `option<i64>` → `option<u8>` 把 1000 **静默变成 232** |
| P0-4 | 泛型实例超限后 `funcInstance` 返回 NULL 被直接解引用（`check_top.c:4528` → `check_expr.c:2405`） | **编译器 SIGSEGV**（65 层 `box<...>`） |
| P0-14 | `parallel::run()` 零实参：在判 arity 之前取 `args[0]`（`check_expr.c:3027`） | **编译器 SIGSEGV**（rc=139） |
| P0-10 | 深度嵌套输入无上限：`dfStmt`/`dfBlock` 互递归（`dataflow.c:316-322`）、parser 递归下降 | ~1000 层块 / 16000 层括号 → **编译器 SIGSEGV**（栈溢出） |
| P0-5 | 延迟泛型调用点只解析一遍（`check_top.c:5596`） | 生成物调用从未生成的 `f_T`，**声明顺序决定成败** |
| P0-11 | `d.run { }` 不计入块层级 ⇒ `__extc_a[]` 少一格（`codegen.c:3642`） | 生成物 **stack-buffer-overflow**（ASan） |
| P0-8 | `copyInto` 的 primitive 没有 `!v.data` 保护（`codegen.c:768`） | 从 NULL `memmove`，`-O0` 下 **SIGSEGV**（`-O2` 下 UB 被"优化掉"，静默"成功"） |
| P0-9 | 泛型方法重载后缀用**调用方**的替换上下文（`codegen.c:630`） | 定义 `w_i32_eq_i32` vs 调用 `w_i32_eq_T`，生成物编不过 |

### 0.1.1 修复进展（2026-09-30 P0 批次 ✅）

> 详细记录：[`.audit/P0-LOG.md`](../.audit/P0-LOG.md)。这一节只回答"报告里的条目现在什么状态"。

**已修并带回归用例（9 条）**：P0-22（`dropRuntimeDefs` 死循环）、P0-11（`d.run{}` 少算一层 + `blkMax` 指数）、
P0-13（`?ref` 下标/切片无非空检查）、P0-14（`parallel::run()` 零实参崩）、P0-8（`copyInto` 空视图 memmove）、
P0-12（f64 整数值字面量发成整型）、P0-4（`funcInstance` NULL 崩溃），
外加闸门①首捕的"协程 typedef 门控"（`cType` 登记需求）。

**四道闸门已落地并接进 `check.sh`**（脚本 `tools/gate_{checkc,asan_corpus,difffuzz,scale}.py`，
公共实现 `tools/gatecommon.py`，基线 `tools/gate-*-known-bad.txt` 采用**棘轮**：修好就要删条目）：

| 闸门 | 挡什么 | 位置 | 当前基线 |
|---|---|---|---|
| ① 全语料 `--check-c`（gcc+clang，`-c -O2`） | "extc 报成功、生成物编不过/有可疑告警" | 完整模式 | 空 |
| ② 正例生成物 ASan+UBSan | "生成物有 UB 却看起来正常" | 完整模式 | 空 |
| ③ 常驻差分 fuzz（17 用例） | "静默算错"（浮点/窄类型/转换/求值次数/`for`+`continue`） | quick | 2 条（P0-15、P0-18） |
| ④ 规模/嵌套（12 用例，带时间与内存预算） | "编译器自己崩或挂住" | quick | 3 条（P0-10） |

**验收**：`tests/run.sh` **316 通过 / 0 失败**（+5 条新用例）；`check.sh quick` **53 节全绿**（+2 道闸门）。

### 0.1.2 P1 批次 ✅（2026-09-30，把基线清空）

> 详细记录：[`.audit/P1-LOG.md`](../.audit/P1-LOG.md)。P0 批次留下的 6 条闸门基线，**全部修完**
> ——四道闸门现在**没有任何已知坏**（`tools/gate-*-known-bad.txt` 全空）。

| 审计条目 | 修法 | 证据 |
|---|---|---|
| **P0-15** `for`+`continue` 死循环 | parser 把末尾 step 记在循环体上；codegen 在 step 前发标签、`continue` 改发 `goto`（迭代器重定向时清标记） | 三种 `for` 形式 + 嵌套全对；`examples/for-continue.extc`；协程套件 29/0 |
| **P0-18** 窄类型算术不回绕 | codegen `narrowWrap()`：按表达式自身类型回截 8/16 位结果 | `examples/narrow-wrap.extc`；差分新增 `narrow_ops` |
| **域帧作用域**（P0-11 修好后闸门新抓到的 UAF） | `extc_dom_run` 改为在 `d.run { }` 块**内**发射 | ASan 全量 211 例 0 失败；`tests/ext` 16/0 |
| **P0-10** 递归无预算 | parser 嵌套预算（块/表达式 64 层、类型 96 层）+ codegen 层级表动态分配 | 预算值实测校准（release ~250 崩、ASan ~96 崩 ⇒ 取 64）；两种构建在 64/200/1000 层都返回诊断；`tests/errors/nesting_too_deep.extc` |

**验收**：`tests/run.sh` **318 通过 / 0 失败**；闸门① **968 文件 0 失败**、闸门② 211 例、
闸门③ 20 例、闸门④ 13 例，**四道基线全空**。下一批（P2）入口见 `.audit/P1-LOG.md` §四。

### 0.1.3 P2 批次（第一批）✅（2026-09-30）

> 详细记录：[`.audit/P2-LOG.md`](../.audit/P2-LOG.md)。

| 审计条目 | 根因 | 修法 | 证据 |
|---|---|---|---|
| **P0-19** `return e?` 载荷不做相容比较 | 该分支只认 `TY_GENERIC`，而 `option`/`result` 是**枚举实例**（`TY_ENUM`+targs）⇒ 载荷从不比较，`option<i64>` → `option<u8>` 把 1000 静默变成 232 | 条件加上 `TY_ENUM`，走普通 `return` 的 `checkAssignable` | 新反例 `tests/errors/try_payload_narrows.extc`（修前"应该报错但通过了"）；实参/赋值/字段三条同族路径复验无洞 |
| **P0-7** 循环条件里的临时量被提升 | 数组 `==` 的操作数物化进**语句前缀**，前缀在 `while` 之前冲刷 ⇒ 只算一次，条件却每轮读它（`while a == mk()` 跑 101 轮） | 条件产生临时量时改发 `while (1) { <前缀>; if (!(cond)) break; … }`；无临时量时字节不变 | 差分闸门新探针 `while_eq_temp`（先红后绿）；新正例 `examples/while-condition-temps.extc` |

**验收**：`tests/run.sh` **321 通过 / 0 失败**；`check.sh quick` 53 节全绿；四道闸门（quick+full）全绿、
**基线仍全空**。

### 0.1.5 P4 批次 ✅（2026-09-30）：定案 99/100/101（含 **R12 / P0-20 落地**）

> 详细记录：[`.audit/P4-LOG.md`](../.audit/P4-LOG.md) 与 [`.audit/R12-DESIGN.md`](../.audit/R12-DESIGN.md)。

| 定案 | 内容 | 判据 |
|---|---|---|
| **99** | **R12 机制 A + 语义 A 的机制那一半**：任务的地方从"块 zone 栈上的 zone"改成**根槽**（`extc_pool_zoneNewRoot/FreeRoot`），结束时精确释放自己那一个；zone 从"栈下标"槽位化为"槽位 + parent 链" | P0-20 形状 **ASan 干净**；40000 个任务跑完 **RSS 6.1 MB**、`live=0 depth=1`（对照旧实现 20000 个 85.9 MB）；`tests/coro` **29/0**（改前 25/4）；新回归用例 `examples/task-place-release.extc` |
| **100** | 嵌套上限（块/表达式 64、类型 96）写成**成文限制**进手册 | `docs/manual/15-errors.md` §9.0；手册三道闸门通过 |
| **101①** | `for i in lo..hi` 的上界**进入循环时求值一次** | `examples/for-range-bound.extc`（`calls=1 once=5 twice=1`） |
| **101②** | 循环**条件**里的分配单独一层 arena、每轮 release | 20000 轮 × 8MB 的 RSS **81.5 MB → 9.3 MB**（与 200 轮持平）；`tests/arena/cond_alloc.extc`（150MB 上限下修前必 OOM） |

**P1-13 同族（第二轮补齐，四种泄漏全部修掉）**：① `coroCheckDeferred` 只给非模板函数算
`coroNeedsZone` ⇒ 泛型协程的分配落在调用者 zone；② `$next` 对实例不发射 ⇒ 句柄直接调 `$step`、
从不 `extc_task_end`（`live=20000`）；③ `extc_task_end` 用 `extc_arena_release` 保留 spare，
而每任务一只 arena ⇒ 200000 个结束任务 RSS 29 MB。修法：实例继承模板的 `coroNeedsZone`、
`$next` 只排除"还有类型参数的模板"、结束时按定案 99 用 `extc_arena_destroy`。
实测：泛型+池 20000 任务 `live=20000`→**0**、9.5→**2.3 MB**；200000 个平凡 boxed 任务 29→**7.6 MB**
（余下是任务表随累计 spawn 增长，属 R12 第 6 条）。

**验收**：`tests/run.sh` **324 通过 / 0 失败**；`check.sh quick` **53 节全绿**；四道闸门（quick+full）
全绿、**基线全空**。**剩余**：R12 第 6 条（任务表槽位复用，`extc_taskNext` 只增不减）、两处白盒用例的 zone id 假设
（改用 `extc_pool_zoneOuter()`）、以及定案 99 语义 A 的逃逸分析那一半（R1/R3 六条出口）。

---

### 0.1.4 P3 批次 ✅（2026-09-30）：golden 降级 + P0-9

**① golden 降级（所有者决定，落成 `DECISIONS.md` 定案 98）**：所有者原话「golden 应该降级，因为现在
需要修理，代码相同不保证正确」。证据：用 `git worktree` 干净检出 HEAD，那份哈希清单**在 HEAD 上就
已过期 162/414**（一直红着没人知道）；本批相对 HEAD 的 34 份差异 100% 可归因，且 414 份产物无一非法。
落地：`tools/golden.sh` 默认只把逐字节差异当**观察**（打印变化面；`--strict-bytes` 保留旧行为、
`--freeze` 重冻），判据只剩「生成物必须是合法 C」；`check.sh` 该节改名「生成物快照（观察项）」不再判红；
「生成物能不能用」由闸门①（全语料 gcc+clang `-c -O2`）与闸门②（ASan+UBSan 真跑）判。
快照已用 `--freeze` 重冻一次，观察值从 0 起步。

**② P0-9 泛型重载后缀用调用方上下文**：`opOverloadSuffix` 读的是**环境**替换上下文 ⇒ 定义侧（实例
上下文）发 `w_i32_eq_i32`、调用侧（`main` 里无上下文）发 `w_i32_eq_T` ⇒ 生成物编不过
（`implicit declaration`）。修法：拆出 `opOverloadSuffixIn(g, f, params, args)`，调用侧 `cMethodName`
显式传**被调方**上下文（方法实例的 targs，否则接收者实例的 targs）。新正例
`examples/operator-overload-generic.extc`（修前红、修后 `i32eq=true u8eq=false i64eq=true`）。

**验收**：`tests/run.sh` **322 通过 / 0 失败**；`check.sh quick` **53 节全绿**；四道闸门 quick+full 全绿、
**基线全空**；`tools/golden.sh` rc=0（414 份合法 C，观察值 0）。详细记录见 `.audit/P3-LOG.md`。

**顺带查出的既有问题（与本批无关，未擅自处理）**：`tools/golden.sha256` 这份"生成物逐字节快照"
**在 HEAD 上就已过期**——用 HEAD 编译器自身跑 `tools/golden.sh`，414 份里有 **162 份**不符；
本批相对 HEAD 的真实差异为 **34 份**，全部可归因（P0-8 24 份、P0-12 7 份、P0-15 2 份、P0-18 1 份），
且 414 份生成物**没有一份是非法 C**。处理方式需所有者定夺：重冻清单，或按设计决议 **R16** 把
golden 从"闸门"降级为"观察项"。

---

### 0.2 五类共性根因（比单条 bug 更重要）

1. **"集合/摘要"类分析靠一遍扫描建立，从不失效或迭代到不动点。**
   逃逸集合（`escapees`）、`fresh` 局部集合、效果掩码、延迟调用表、泛型实例表都是"边走边记、只记一次"。
   一旦程序形状要求"后来才发现某名字/某实例也是逃逸的"，记录不会再更新 ⇒ P0-1、P0-3、P0-5、P1（`fresh`）、P1（掩码）。
2. **哨兵值与合法值共用同一个数值。** `0` 既表示"没有最小值"又表示"depth 0 = 活得最久"（P0-2）；
   `0` 既表示"未设置"又表示 `num`（`coroNeedsZone` 未对实例计算）。这类问题在代码里没有任何断言兜底。
3. **检查器与代码生成各有一份"同一张表"的实现，已经分叉。** `countOwSites` vs `collectOwSites`（`ST_DOMAIN`）、
   帧结构命名（`cFuncName` vs `check_top.c:3962`）、句柄协议两份实现、实例名守卫只认 `TY_GENERIC && sdef`。
   分叉处的注释都写着"两边必须一致"，但没有任何机制保证。
4. **运行期生成文本里的数值边界没做溢出/取整检查。** `(n + 7) & ~7`（P1）、`(double)INT64_MAX`（P1）、
   `n / (threads * 8)`（P1）、`(size_t)workers * sizeof(...)`（P2）、`1u << j`（P1）——全是**生成到用户程序里**的 UB。
5. **深递归/指数路径没有任何预算或上限。** parser、dataflow、`blkMaxOfBlock`、`funcReachesItself`、level pass：
   输入规模稍大就从"慢"直接变成"挂住"或"栈溢出"。

---

### 0.3 实现 bug 还是设计缺口？22 条 P0 + R12 的分类

**判据（两个问题，顺序不能反）**：
1. **有没有成文的契约说它应该怎样？**（编号定案 / 专题文档 / 同文件里的既有不变式）
   —— 有 ⇒ 与实现不符是 **bug**；没有 ⇒ 是**设计缺口**。
2. **满足那条契约能不能靠局部改动？** —— 能 ⇒ **实现 bug**；不能（要换机制或加新机制）⇒ **设计漏洞**。

分类结果：**14 条实现 bug · 6 条机制缺口 · 2 条真设计漏洞**。

| 桶 | 条目 | 为什么 |
|---|---|---|
| **A. 实现 bug**（契约在，代码没照做；修法局部） | P0-3、P0-4、P0-7、P0-8、P0-9、P0-11、P0-12、P0-13、P0-14、P0-15、P0-17、P0-18、P0-19、P0-22 | 多数与**紧邻的注释、姊妹函数、同族三处中的另两处**直接矛盾：`genViewIndexer` 有 `!v.data` 保护而 `genViewCopier` 没有（P0-8）；`rejectNullableDeref` 在字段/`*`/方法接收者三处调用了、下标与切片两处漏了（P0-13）；`%.17g` 的整数值少了 `.0`（P0-12）；`d.run{}` 少一个 `switch` 分支（P0-11）；`continue` 不推进光标（P0-22）。本会话实测：这 14 条里的 13 条用 **1–25 行**改完，全部带回归用例 |
| **B. 机制缺口**（没有契约，或机制的**策略**不成立） | P0-1、P0-2（一半）、P0-5、P0-6、P0-16（+P1 的 `fresh`/掩码族） | **同一台机器的六个出口**：逃逸集合、延迟调用表、效果掩码都是"边走边记、只记一次、从不失效、不迭代到不动点"（§0.2-1）。策略本身不成立 ⇒ 必须新建机制（失效 + 不动点 + 符号身份），对应 **R1/R3/R4**；补一条漏记只会让下一个出口再犯 |
| | P0-10（+15 条性能项） | 登记册里**根本没有**"编译器自身资源上限"的定案 ⇒ **R8 是新拍板**。缺的是契约（"超限要诊断，不是崩"），不是某一行 |
| | P0-11 的根、P0-5/P0-9 的命名 | **双份真源**：同一个事实（块层数、实例名、重载后缀）在检查器与代码生成里各算一遍，注释写着"两边必须一致"却没有任何机制保证 ⇒ **R2/R7** |
| | P0-21 | **粒度建模错**："这次分配发生在 worker 里"是**动态**属性，却被编码成**函数级静态标记** ⇒ 主线程调同一函数就拿到 NULL 的 TLS arena。对应 **R13** |
| **C. 真设计漏洞**（机制本身表达不了需求，只能换） | **P0-20 → R12** | 任务的内存归属用"全局 zone 栈 + 按 LIFO 弹"表达，而**任务存活顺序不是 LIFO**：弹 → 弹掉仍存活任务的 zone（UAF）；不弹 → 每任务泄漏 ≈4.2 KB（实测 20000 个结束任务 85.9 MB）。`CONCURRENCY.md` 早已拍板"任务=地方根 B"（帧存自己的 zone id、一次性回收），旧实现走的是**单栈假设**，两者不可调和 ⇒ 换机制（每任务独立 arena）。这正是 R12 是**新拍板**（登记册 0 条）的原因 |
| | P0-2 的哨兵复用 | 单独看接口设计：同一个数值 `0` 同时表示"未设置"与"活得最久的 depth 0"，机制上无法自证 ⇒ 只能换编码（可选值 / 位标记） |

**还有第四类：不是编译器 bug，是验收流程的设计问题。** R16/R17/R18（闸门权威、决策登记册治理、过时文本回填）
落在这一桶。活例就是 golden：它**在 HEAD 上已经红了 162/414 却没人知道** —— 不是代码写错，
而是"验收流程"缺一条判据"**闸门变红必须等价于真缺陷**"。2026-09-30 的定案 98（golden 降级）
补的正是这条。

**边界不是刀切的**：P0-2/P0-3/P0-17/P0-21 属于"框架在、某条路没接上"还是"策略不成立"，
取决于你把线画在哪；本文按"**修法是否局部**"划线（局部 = A，要动机制 = B/C）。

**为什么这个区分决定修法**（本会话的实测分布）：
- **A 类**：改对了就行 + 配回归用例。P0 批次 8 条、P1 4 条、P2 2 条、P3 1 条 = **15 条已修完**，
  全部在一小时内可验（`tests/run.sh` 322/0、四道闸门基线全空）。
- **B 类**：必须先写契约（R1–R18 已写），再换机制（四台手术 A 分析不动点 / B 发射需求闭包 /
  C 命名单一来源 / D 预算），最后用闸门钉住 —— 否则同一个错会在别处复现。
- **C 类**：只能换机制。在 LIFO 栈上打补丁去修非 LIFO 释放，等于往错误模型里加特例。

**顺带一个结论**：这些几乎全是**编译器内部架构**的设计缺口，不是**语言设计**的错。
语言层面（语法/语义/内存模型的对外承诺）真正需要动的是 R12 那一条（"任务=地方根"的表达力），
其余 21 条改的都是编译器的分析/发射/预算机制。

---

### 0.1.6 R1/R3 批次（进行中，第 1 轮）

> 详细记录：[`.audit/R1R3-LOG.md`](../.audit/R1R3-LOG.md)。

**判据先行**：新增**闸门⑤·逃逸健全性**（`tools/gate_escape.py` + `tools/escape-corpus/` 17 份语料 +
棘轮基线 11 条），已接进 `check.sh` quick（第 54 节）。语料是"记账比真实情况小"的洞的最小复现——
现在被接受、生成物在 ASan 下真的 UAF/SEGV/编不过。闸门支持两种期望：`reject`（必须报错）与
`run`（必须接受且 ASan 跑干净）；`control_*` 孪生对照**永远不许进基线**。

**已修 13/13（R1/R3 批次收口）**（其中 4 条的正确终态是「编译对」而不是「拒绝」——闸门语料因此支持两种期望）：

| 条目 | 根因 | 修法 | 判据 |
|---|---|---|---|
| **P1-4** 参数 >32 时摘要丢位 | 三层 32 位截断：字段 `unsigned`、`1u << j`、`j < 32` 循环；**外加消费侧 5 个 `unsigned` 局部变量**把 64 位再截回 32 位 | 全部改 64 位；新增 `EFF_MAX_PARAMS 64` 契约（>64 ⇒ 带位置的编译错误） | 二分定位阈值正好 32；修后 33/34/35/64 参数都拒绝、65 报上限错；4 参数孪生对照仍拒绝 |
| **P0-1** `EX_TRY` 不进逃逸集合 | 语句级的逃逸登记（`ST_EXPR` 分支）只认 `EX_CALL`/`EX_METHOD`/`EX_ASSOC`，`out.put(x)?` 的顶层是 `EX_TRY` ⇒ 实参从不进 E ⇒ 传给被调方的是本帧块 arena | 登记前先拆掉 `EX_TRY`（`?` 决定调用**之后**发生什么，不决定调用能发布什么） | 生成物 `&__extc_a[1]` → `__extc_home`；ASan heap-use-after-free → 干净（`head = 7`）⇒ 同样**改判为「应当接受」**（闸门 `run` 期望） |
| **P0-2** `0` 当哨兵求成最大值 | 两处副本把 `0` 同时当"没见到"与"depth 0（最浅）"，于是求的是**最大值**；补丁行更是如此 | 加 `bool seen`，真正求最小值 | 生成物从 `&__extc_a[1]` 变成 `__extc_home`；ASan 从 heap-use-after-free 变成**干净**、打印 `v=5` ⇒ 该探针**从"必须拒绝"改判为"应当接受"**（闸门的 `run` 期望，正对应主人决定③"能判的就判精确"） |

**余下 11 条**（P0-1、P0-6a–d、P0-16×3、P0-17、P0-3、P1-5）按 INV-C/U/I（看对对象/穿透载体）→
INV-K（写点完整）→ INV-F（写点失效/不动点）的顺序施工。


#### 0.1.6.1 R1/R3 进度（**完成**：13/13，闸门⑤基线清零，2026-09-30）

**已修 12/13**，五道闸门全绿；闸门⑤基线只剩 2 条（= 施工单剩余项）：

| # | 条目 | 终态 | 关键机制 |
|---|---|---|---|
| 1 | P1-4 参数 >32 摘要丢位 | 拒绝 | 三层 32 位截断（字段/移位/循环）+ **消费侧 5 个局部变量**；新增 `EFF_MAX_PARAMS 64` 契约（>64 报错） |
| 2 | P0-2 `0` 当哨兵求成最大值 | **编译对** | `bool seen` 求真正的最小值；生成物 `&__extc_a[1]` → `__extc_home` |
| 3 | P0-1 `EX_TRY` 不进逃逸集合 | **编译对** | 语句级登记前先拆掉 `EX_TRY` |
| 4 | P0-3 自由泛型零值检查不记录 | 拒绝 | 去掉 `recordZeroCheck` 的 `owner` 早退（replay 早已支持自由函数实例） |
| 5 | P0-16a/b `mut ref` 出参无 publication | 拒绝 | INV-K：调用点按摘要把来源实参深度发布到目的绑定；`promoteFieldsAt` **方向拆分**（addressed 根：可提升、不可降低） |
| 6 | P0-5 延迟解析单趟 | **编译对** | 改不动点（新建实例的延迟调用点需要下一轮） |
| 7 | P1-5 `fresh` 集合不失效 | 拒绝 | `ST_ASSIGN` 整对象重指向 ⇒ `freshDrop`（字段写不算） |
| 8 | P0-6c `placeDepth` 兜底方向反 | 拒绝 | 兜底改问 `exprRefDepth`（"它指向的有多深"），不再答 0 |
| 9 | P0-6b `needsHome` 早退 | 拒绝 | 目的地是全局时该理由不成立（问 `isGlobalSym`） |
| 10 | P0-6d 参数按名字匹配 | 拒绝 | 参数 `Sym` 记在 `FuncDef` 上 + `rootSymNoScope`（摘要 pass **没有作用域**，这是按名字匹配的根本原因） |
| 11 | P0-6a 借用经局部洗白 | 拒绝 | `exprBorrowed` 跟一层**来源链**（`Sym.origin`），只跟**裸绑定**来源 |
| 12 | — | — | — |

**剩余 0 条**。最后两条的落地方式（补17–补22 的实测过程）：

- **P0-16d**（`stash(ref b, n)` 后 `return b`，`n = new node`）：出参发布需要**专用槽**（复用
  `noteFieldSrc(..., NULL, ...)` 会给既有的元素写路径加提升压力，实测把 `store-promotion`/
  `promote-slice`/`promote-through-mid` 三例打红），并且 `dj == 0`（引用型来源实参在调用点
  `refDepth` 尚未填上）时要改用 `recordLvlFact` 的**末轮重放**机制。
- **P0-17**：字段路径的非空证明在 `mut ref self` 方法调用后不失效（`check_lookup.c`）——
  需要给证明栈加**写点失效**（INV-F）。

**两条方法论教训（本批次最值钱的部分）**：
1. **发布必须发到"做判定的那个查询"能看见的地方**——这个检查器对同一事实有多个查询函数
   （`placeDepth` 读字段表 + `otherDepth`；`exprRefDepth` 跟 origin），先钉判定点再写发布。
2. **修好一条探针时，必须看清它是因何被拒**：闸门只会说"被拒了"，不会说"拒对了没有"。
   P0-6a 的第一版就靠这条抓出了"P0-17 假绿"（同一过宽规则把它一起拒了）。


---

## 1. 审阅方法与可信度

### 1.1 怎么读的

- **全量逐行**：`src/` 每个文件切成 1000–2200 行的区间，由 20+ 个独立审阅单元逐行读完（不是抽样），
  每个单元产出一份 JSON（位置、逐字引用、成因、触发条件、复现命令、置信度）。覆盖表见 §9。
- **交叉验证**：每条候选都要先"自我反驳"——查调用方、被调方、数据结构不变量，别处已保证的不得上报。
- **实测复现**：报告里的每条 P0/P1 都要求写出最小 `.extc` 程序并跑出观察结果；
  P0 全部由我（Lead）在本机独立重跑过一遍（下文标 `[Lead 复现]`），队友的独立复现标 `[报告者复现]`。

### 1.2 用了哪些实测手段

| 手段 | 结果 |
|---|---|
| 生成物 ASan+UBSan 全量扫描（examples 全部 → 编 C → 跑） | 用于确认逃逸/越界类漏洞；多条 P0 由此定案 |
| 编译器自身 ASan+UBSan 全语料扫描 | 编译器自身在**正常语料**上干净（说明缺陷要特定形状才触发） |
| `--check-c` 全语料扫描（gcc 与 clang 各一遍） | 抓到"编译器报成功、生成物编不过"的实例（含 stdlib 自带模块） |
| 变异 fuzz（截断/删行/换 token/重复块，1858 例） | 普通畸形输入不崩；崩溃需要**深嵌套**结构（见 P0-10） |
| callgrind | 定位 `blkMaxOfBlock`/`blkMaxLevel` 占 ~90% 指令（指数爆炸，见 §6.1） |
| `gcc -fanalyzer`（全 `src/`） | **0 条告警** —— 这些缺陷在语义/契约层，通用静态分析器看不见 |
| 生成物告警扫描（594 份生成物 × gcc/clang × `-fsyntax-only`/`-c -O2`） | **真实告警类别 0 条**（语料生成物很干净）；方法学要点：`-fsyntax-only` **看不到中端告警**（`-Warray-bounds`/`-Wmaybe-uninitialized`/`-Wstringop-*`），必须用 `-c -O2` 才有牙 |
| cppcheck `--enable=warning,performance,portability` | 2 条可疑点（`check.c:466`、`check_stmt.c:424`），逐条核实为“当前调用方不可达 / 死防御” |
| gcc `-Wall -Wextra -Wpedantic -Wshadow -Wcast-qual` 加严重编 | 39 条告警全是 const 修饰/超长字符串一类卫生问题，无功能缺陷 |
| `EXTC_SELFCHECK=1` / `EXTC_DBG_ARENA=1` 全语料 | 仓库自带自检在 tests/examples 上不报警；只在 stdlib 单文件（本就不能单独编译）上非零退出 |

### 1.3 严重度口径

- **P0**：编译器崩溃/内存破坏；或对常见程序**静默**生成错码、生成物有 UB/内存不安全。
- **P1**：健全性漏洞（放行了会 UB 的程序）、**报成功但生成物编不过**、误拒合法程序、窄路径错码。
- **P2**：健壮性/边界/诊断质量/性能陷阱（可构造输入让编译器挂住或明显变慢）。
- **P3**：有具体代价的卫生问题（死代码掩盖 bug、未初始化字段、重复逻辑已分叉、丢数据的窄化）。

> "P0" 看**后果**不看触发难度：65 层嵌套类型不常见，但它让编译器段错误，就是 P0。

---

## 2. P0：崩溃、静默错码与内存不安全

### P0-1 `call(...)?` 让逃逸集合失效 ⇒ 生成物 heap-use-after-free `[Lead 复现]`

- **位置**：`src/check_top.c:2022`（`markNamesInStmt` 的 `ST_EXPR` 分支）
- **代码**：
  ```c
  case ST_EXPR:
      switch (s->u.expr.expr->kind) {
      case EX_CALL: case EX_METHOD: case EX_ASSOC: {
          ...
          if (pub == 0) return false;        /* the callee stores nothing */
          return markNamesInExpr(c, s->u.expr.expr);
      }
      default:
          return false;                      /* other expression statements do not escape */
      }
  ```
- **成因**：逃逸集合 `c->escapees` 是 `isEscapeeName()`（method 接收者，`check_expr.c:3686`）和
  `callHomeDepth`（`check_top.c:1616-1622`）判断"该给被调方哪只 arena"的唯一输入。
  写成 `out.put(x)?` 的调用是 `EX_TRY` 包着 `EX_CALL`，落进 `default` ⇒ 局部容器不被标记为逃逸
  ⇒ 接收者算出 `placeDepth = 1` 而不是 `-1` ⇒ 被调方把数据分配进**调用方的块 arena**，
  块一退就全死，而调用方还拿着这些数据。
- **最小复现**（= `tests/arena-promoted/B3_exprstmt_method.extc` 把两处调用写成 `...?`）：
  ```extc
  // tryb3.extc
  use std::io
  struct box { head: ?ref node  n: i64 }
  struct node { v: i64  next: ?ref node }
  ...
  fn fill(l: mut ref box, out: mut ref box) -> i32 {
      l.pushOne(7)?            // EX_TRY：逃逸集合看不见
      out.take(l)?             // EX_TRY
      return 0
  }
  ```
  完整文件见 `/tmp/rev/findings/tryb3.extc`（本报告附录 A 有生成方法）。
  ```sh
  ./build/extc -w tryb3.extc -o tryb3.c            # 退出码 0，无诊断
  gcc -std=c11 -O1 -g -fsanitize=address,undefined -fwrapv tryb3.c -o tryb3 && ./tryb3
  # → AddressSanitizer: heap-use-after-free ... tryb3.extc:40 in main
  ```
- **对照**：同文件去掉两处 `?` 的版本（`tests/arena-promoted/B3_exprstmt_method.extc`）ASan 干净；
  `EXTC_DUMP_EFFECTS=1` 显示 `?` 版本是 `[escapes] fill { }`，对照版本是 `{ l out }`。
- **建议修法**：`markNamesInStmt` 对 `EX_TRY` 递归进 `u.try_.expr`（以及所有"包裹型"表达式）；
  更根本的做法是把逃逸集合换成 §0.2-1 的不动点迭代，并在 `EX_TRY`/`EX_LAMBDA`/`EX_DEREF` 等
  "透传"节点上建立统一的 `unwrapCarrier()` 帮助函数，避免每加一种语法就漏一处。

### P0-2 调用点 arena 深度用 `0` 当哨兵 ⇒ 生成物 heap-use-after-free `[Lead 复现]`

- **位置**：`src/check_top.c:6024`（同一逻辑在 `:6330` 的 zone 通道重复一遍）
- **代码**：
  ```c
  int nd = 0;
  for (int k = 0; k < rec->n; k++) {
      int d = rec->argDepth[k];        /* 0 = parameter/global: outside this frame */
      if (d != 0 && rec->argRoot[k] && isEscapeeName(&c, rec->argRoot[k])) d = -1;
      if (nd == 0 || d < nd) nd = d;   // ← 求的是"最大值"，不是最小值
  }
  if (nd == 0 && rec->n > 0) nd = rec->n ? rec->argDepth[0] : 0;
  ```
- **成因**：`argDepth[k] == 0` 的语义是"这个目的地活得比本帧久（参数/全局）"，是**最短**的生命周期；
  但 `nd == 0` 又被当作"还没见到最小值"。于是 `argDepth = {0, 1}` 得到 `nd = 1`，
  `want = (nd <= 0) ? ARENA_HOME : nd` 就把调用方**本帧的块 arena** `&__extc_a[1]` 传给了被调方，
  而被调方按约定会往这只 arena 里分配会被长期持有的对象。
- **最小复现**（`/tmp/x/ea2.extc`，`[Lead 复现]`）：
  ```extc
  use std::io
  struct node { value: i64  next: ?ref node }
  struct slot_t { v: ?ref node }
  fn stash(dest: mut ref slot_t, other: mut ref slot_t, v: i64) {
      @overwrite var n = new node
      n.value = v
      dest.v = n
  }
  fn caller(dest: mut ref slot_t) -> i32 {
      var deep: slot_t = { v: null }
      stash(dest, ref deep, i64(5))
      return 0
  }
  fn main() -> i32 {
      var s: slot_t = { v: null }
      caller(ref s)
      io::cout << "v=" << (*s.v!).value << "\n"
      return 0
  }
  ```
  ```sh
  ./build/extc -w ea2.extc -o ea2.c   # 退出码 0
  grep 'stash(dest' ea2.c
  # → stash(dest, &(deep), ((int64_t)(5)), &__extc_a[1]);   ← 应为 __extc_home
  gcc -std=c11 -O1 -g -fsanitize=address,undefined -fwrapv ea2.c -o ea2 && ./ea2
  # → AddressSanitizer: heap-use-after-free ... ea2.extc:17 in main
  ```
- **建议修法**：用独立的哨兵（如 `INT_MAX` 或 `bool seen`）表示"未设置"，或先算
  `nd = (n ? argDepth[0] : 0)` 再对 `k>0` 取最小；两处重复逻辑合并成一个函数。

### P0-3 自由泛型函数返回 `T` 时零值检查从不记录 ⇒ 生成 `(cellfn){0}` 并调用空函数指针 `[Lead 复现]`

- **位置**：`src/check_top.c:4865`（唯一的判定点）；记录点 `src/check_escape.c:1474/1496/1527` 全部以
  `if (!c->curFunc || !c->curFunc->owner) return;` 提前返回，而自由函数的 `owner == NULL`。
- **代码**：
  ```c
  if (rc->isZero) {
      /* Zero-value kind: does this instance's `T` have a zero value? */
      Type *zt = tsub(c, rc->declType);
      ...
      if (typeLacksZeroValue(tt, zt))
          ckError(c, rc->line, "... Give the local an initializer." ...);
  ```
- **成因**：模板体上的 `var b: T` 不能当场判定（`T` 不透明），必须留到实例化；
  但"留到实例化"的记录函数对 `owner == NULL`（自由函数）直接不记，
  于是这条检查对自由泛型函数**永远不执行**，代码生成给它发 `(cellfn){0}`（零值 = 空函数指针）。
- **最小复现**（`/tmp/f3.extc`）：
  ```extc
  struct cellfn { f: fn(i32) -> i32 }
  fn mk<T>() -> T { var b: T  return b }
  fn main() -> i32 { let c: cellfn = mk<cellfn>()  return c.f(3) }
  ```
  ```sh
  ./build/extc -w f3.extc -o f3.c && gcc -O0 -fwrapv f3.c -o f3 && ./f3
  # → Segmentation fault (rc=139)；生成物里是 `cellfn b = (cellfn){0};`
  ```
  同类第二形状：`T = slice<u8>` 时生成物出现未声明的
  `__extc_reference_has_no_zero_value__`（`--check-c` 报错）。
- **对照**：具体类型版本（不经过泛型）会被正确拒绝：
  `cannot zero-initialize \`b\`: it contains a \`fn\``。
- **建议修法**：让记录函数对"自由函数模板"也记录（`owner == NULL` 不是理由，模板身份用 `tmpl` 指针即可）；
  或在 `runRefCheck` 的自由实例路径上重新扫描模板体的零值声明。

### P0-4 泛型实例超限后 `funcInstance` 返回 NULL，调用方直接解引用 ⇒ 编译器 SIGSEGV `[Lead 复现]`

- **位置**：`src/check_top.c:4528`（两个错误返回点：4534 类型深度、4544 实例数上限）；
  未检查的调用方：`src/check_expr.c:2405`、`:3180`（都是 `inst->used = true;`）。
- **代码**：
  ```c
  for (size_t j = 0; targs && j < targs->len; j++) {
      int d = typeDepth(*(Type **)vecAt(targs, j));
      if (d > TYPE_DEPTH_LIMIT) {
          ctxError(c->ctx, line, 1, "This is a limit of the compiler, ...", ...);
          return NULL;              // ← 诊断之后返回 NULL
      }
  ```
  ```c
  FuncDef *inst = funcInstance(...);
  inst->used = true;                // check_expr.c:2405，无 NULL 判断
  ```
- **最小复现**：
  ```sh
  python3 -c "d='i64'
  for _ in range(65): d='box<%s>'%d
  open('/tmp/d65.extc','w').write('struct box<T> { v: T }\nfn z<T>() -> i64 { return i64(0) }\nfn main() -> i32 { return i32(z<%s>()) }\n'%d)"
  ./build/extc /tmp/d65.extc -o /dev/null      # → Segmentation fault (rc=139)
  ```
  64 层正常报错退出；65 层崩溃。第二形状：4300 个不同泛型实例 ⇒ 在 `check_expr.c:3180` 崩溃。
  gdb 栈：`checkExprInner (src/check_expr.c:2405) ← checkInto ← checkStmt ← checkFunc ← checkModule`。
- **影响**：编译器**在报"这是编译器限制"的路上自己崩掉**——正是这道上限想保护的输入类别。
- **建议修法**：`funcInstance` 的错误路径返回一个哨兵实例（`isError`）或让调用方检查 NULL；
  最稳妥是给"实例查询"和"实例创建"分两个 API，调用方只在 `used` 处做一次判空。

### P0-5 延迟泛型调用点只解析一遍 ⇒ 生成物引用从未生成的 `f_T`，且**声明顺序决定成败** `[Lead 复现]`

- **位置**：`src/check_top.c:5596`（`for (i = 0; i < c.callChecks.len; i++)` 单趟遍历）
- **代码**：
  ```c
  for (size_t i = 0; i < c.callChecks.len; i++) {
      CallCheck *cc = *(CallCheck **)vecAt(&c.callChecks, i);
      if (!cc->node || !cc->tmpl) continue;
      for (size_t j = 0; j < c.funcInsts.len; j++) {
          FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
          if (fi->tmpl != cc->func) continue;
          resolveDeferredCall(&c, cc, &fi->tmpl->typeParams, &fi->targs);
      }
  }
  ```
- **成因**：记录按"检查函数体的顺序"追加；`resolveDeferredCall` 只在**外层模板的实例已经存在**时才能实例化。
  若最内层模板先被检查，它的记录先被处理（此时外层实例还不存在，跳过），之后外层实例才被创建，
  但那条记录已经"处理过"了，不会再来一遍 ⇒ 调用点留在临时名字 `f_T` 上，而 codegen 故意不生成 `f_T`。
- **最小复现**：
  ```extc
  // g3.extc：声明顺序 f, g, m（内层在前）
  fn f<T>(x: T) -> T { return x }
  fn g<T>(x: T) -> T { return f(x) }
  fn m<T>(x: T) -> T { return g(x) }
  fn main() -> i32 { let r: i64 = m<i64>(i64(3))  return i32(r) }
  ```
  ```sh
  ./build/extc -w g3.extc -o g3.c && gcc -std=c11 -fsyntax-only g3.c
  # → error: implicit declaration of function 'f_T'   （生成物里 g_i64 调 f_T，而 f_T 不存在）
  # 把声明顺序改成 m, g, f（/tmp/g3r.extc）就编译通过
  ```
- **建议修法**：把这一批改成**不动点循环**（直到本轮没有新的实例/没有新的可解析记录），
  或者让 `resolveDeferredCall` 在缺少外层实例时就地创建它（按需实例化）。

### P0-6 借用经"载体"洗白后写进全局（逃逸检查四条独立漏洞）`[Lead 复现]`

这一族有四条，机制不同、后果相同：**全局变量持有指向已死栈帧/块 arena 的引用，之后照常读写**。

**(a) `exprBorrowed` 漏掉载体形状**（`src/check_escape.c:649`，`default: return false` 在 `:698`）
```c
if (root && root->depth == 0) ...     /* 只有根绑定 depth==0 才认为“借来的” */
```
`EX_STRUCTLIT` / `EX_DEREF` / `EX_ARRAYLIT` / `EX_CONV` / `EX_TRY` / `EX_LAMBDA` 在 `default` 里直接返回 false。
于是 `var q = p  G = q`、`G = holder{p: p}`、`G = *h` 都能把调用方块内的视图存进全局。
复现（`/tmp/rev/repro/b3.extc`）：
```sh
./build/extc -w b3.extc -o b3.c && gcc -O1 -fwrapv b3.c -o b3 && ./b3
# → G = ABCDEFGHIJKLMNOP        （兄弟块复用同一栈槽，全局读到“僵尸数据”）
# 直接写 `G = p` / `G.p = p` 会被正确拒绝
```

**(b) `needsHome` 早退**（`src/check_escape.c:1447`）
```c
if (c->curFunc && c->curFunc->needsHome) return bad;   /* 注释只对“目的地和 home 同寿”成立 */
```
目的地是**全局**时该理由不成立：全局比任何 arena 都长寿。
复现（`/tmp/rev/repro/c4.extc`）：`fn stash(p: slice<u8>) -> hbox { G = p  var b = new u8[4]  return {p:b} }`
被接受，跑出 `G = AAAAA...`；把同一函数改成返回 `i32`（无 home）则被拒绝。

**(c) `placeDepth` 的兜底 `return 0`**（`src/check_escape.c:147`）
```c
Sym *root = placeRoot(c, e);
return root ? root->depth : 0;      /* 0 = “活得最久”，方向恰好相反 */
```
`placeRoot` 只认 `EX_IDENT/EX_FIELD/EX_INDEX/EX_SLICE`，认不出来的一律按"活得最久"处理。
复现（`/tmp/rev/repro/n1.extc` vs `n2.extc`）：`G = v` 被拒，`G = [v, v][0]` 被接受并读到死数组。

**(d) 参数匹配按"名字"而不是符号**（`src/check_escape.c:726`，`destIsParam` 同族）
```c
for (size_t i = 0; i < f->params.len; i++)
    if (strcmp((*(Param **)vecAt(&f->params, i))->name, root) == 0) return true;
```
参数遮蔽是**合法且文档化**的（`examples/shadowing.extc` 第 4 条）。
复现（`/tmp/rev/repro/m3.extc`）：`var s = q` 遮蔽参数 `s` 后，被调方把约束记到参数 `s` 头上，
调用点检查了**另一个实参**；不带遮蔽的孪生程序（`m2.extc`）被正确拒绝。

- **建议修法**：这四条指向同一个结构性修法——把"值从哪来"的判定从**名字比较**改成**符号/来源链**，
  并把 `exprBorrowed`/`placeDepth`/`valTracesToParam` 统一到一个"载体穿透"函数上（遇到
  `EX_STRUCTLIT/EX_DEREF/EX_ARRAYLIT/EX_CONV/EX_TRY/EX_LAMBDA` 继续向下），
  再对"目的地是全局"单独一条规则：**任何携带引用的值写入全局都必须证明其来源深度为 0**。

### P0-7 数组 `==` 的临时量被提到 `while` 条件之外 ⇒ 死循环/结果错 `[Lead 复现]`

- **位置**：`src/codegen.c:1194`（`eqOperand`）+ `src/codegen.c:4053-4056`（`ST_WHILE` 在 `while(...)` 之前 `flushPrefix`）
- **代码**：
  ```c
  static const char *eqOperand(CG *g, Expr *x, Type *t) {
      const char *code = genExpr(g, x);
      if (printArgIsPlace(x)) return arenaPrintf(g->arena, "&(%s)", code);
      const char *tmp = arenaPrintf(g->arena, "__extc_q%d", g->tmpSeq++);
      pfLine(g, "%s %s = %s;", cType(g, t), tmp, code);   // 写进语句前缀
      return arenaPrintf(g->arena, "&%s", tmp);
  }
  ```
- **成因**：语句前缀在 `while` 之前被冲刷，前缀里的调用只执行一次；而循环条件每轮都读那个临时量。
  检查器只对 `??` 做了防护（`check_stmt.c:894` 的 `noHoist` + `check_expr.c:2624` 的拒绝），
  `eqOperand` 这条路径没有。
- **最小复现**：
  ```extc
  // i_while2.extc
  use std::io
  var g: i32 = 0
  fn mk() -> [2]i32 { g = g + 1  return [g, 0] }
  fn main() -> i32 {
      var a: [2]i32 = [1, 0]
      var n: i32 = 0
      while a == mk() { n = n + 1  if n > 100 { break } }
      io::cout << "rounds=" << n << "\n"
      return 0
  }
  ```
  ```sh
  ./build/extc --run i_while2.extc      # → rounds=101（正确应为 1）
  ```
  生成物：`array_2_i32 __extc_q0 = mk();  while (extc_eq(&(a), &__extc_q0, ...)) {`
- **建议修法**：`eqOperand`（及其它 `pfLine` 产生者）遇到 `c->noHoist` 场景必须改用条件内联表达式
  （如 C 的逗号表达式或 `extc_eq_tmp()` 辅助），或在 `flushPrefix` 之前断言前缀为空。

### P0-8 `copyInto` primitive 没有空存储保护 ⇒ 从 NULL `memmove` `[Lead 复现]`

- **位置**：`src/codegen.c:768`（`genViewCopier`）；姊妹函数 `genViewIndexer` 在 `:742` 有 `!v.data` 保护。
- **代码**：
  ```c
  cgLine(g, "if (n < 0 || n > d.len || n > s.len)");
  cgLine(g, "    extc_trapMsg(file, line, \"copyInto: the count is beyond a slice length\");");
  cgLine(g, "if (n > 0) memmove((void *)d.data, (const void *)s.data, (size_t)n * sizeof(%s));", ...);
  ```
- **最小复现**（`/tmp/rev/work/e_copy3.extc`：池被关闭后的视图 + `copyInto`）：
  ```sh
  ./build/extc -w e_copy3.extc -o e_copy3.c
  gcc -std=c11 -O0 -g -fwrapv e_copy3.c -o e_copy3 && ./e_copy3
  # → Segmentation fault (rc=139)（gdb: slice_i32_copy → memmove，s.data = nil, len = 4）
  # -O2 下 gcc 把 UB 调用删掉，程序“静默成功”——更糟
  ```
- **建议修法**：`genViewCopier` 复制 `genViewIndexer` 的 `if (!d.data && d.len) / if (!s.data && s.len)` 陷阱；
  `n > 0` 时两边都要有存储。

### P0-9 泛型重载后缀用调用方的替换上下文 ⇒ 定义与调用名字不一致 `[Lead 复现]`

- **位置**：`src/codegen.c:630`（`opOverloadSuffix`）
- **最小复现**（`/tmp/rev/work/b_ovl2.extc`）：
  ```sh
  ./build/extc -w b_ovl2.extc --check-c
  # → error: implicit declaration of function 'w_i32_eq_T'
  #   定义处生成的是 w_i32_eq_i32，调用处生成的是 w_i32_eq_T
  # → extc: **the generated C does not compile** -- this is an extc bug
  ```
- **建议修法**：后缀必须由**被调函数自身的单态化上下文**决定；把"当前 mono 上下文"作为参数传给
  `opOverloadSuffix`，或直接复用符号表里已算好的实例名。

### P0-10 深嵌套输入无上限：编译器栈溢出崩溃 `[Lead 复现]`

两处独立的递归没有深度上限；输入合法（语法正确）且很短小。

**(a) 数据流 pass：`dfStmt`/`dfBlock` 互递归，`Facts` 按值复制**
- 位置：`src/dataflow.c:316-322`；`Facts` 含 `DFA_MAX_VARS(256)` 条记录，每帧还要复制数份
  （`thenF/elseF/saved/body/acc/arm`）。
- 复现：把 `fn main() { ... }` 里嵌 1000 层 `{ }`：
  ```sh
  python3 -c "n=1000; open('/tmp/db1000.extc','w').write('fn main() -> i32 {\n' + '    {\n'*n + '    }\n'*n + '    return 0\n}\n')"
  ./build/extc /tmp/db1000.extc -o /dev/null      # → Segmentation fault (rc=139)
  ```
  gdb：`dfStmt (dataflow.c:322) ← dfBlock (dataflow.c:319) ← ...`，崩溃时 `depth=204`（8 MiB 栈 / ~40 KiB 每帧）。
  `depth` 参数一路传下去，却从未用于判断"太深了"。
- **同族**：代码生成侧对嵌套块**指数**爆炸（见 P2-1），40 层就挂住不返回。

**(b) parser 递归下降无深度上限**
- 复现：
  ```sh
  python3 -c "open('/tmp/dp.extc','w').write('fn main() -> i32 {\n    var x: i32 = ' + '('*16000 + '1' + ')'*16000 + '\n    return x\n}\n')"
  ./build/extc /tmp/dp.extc -o /dev/null          # → Segmentation fault (rc=139)
  ```
  gdb：`parseUnary (parser.c:2732) → parseFactor (parser.c:2676) → parseTerm (parser.c:2662) → ...`。
  8000 层正常，16000 层崩溃；`[`×20000 同理。
- **建议修法**：三条路都加"深度预算"：parser 在 `parseExpr/parseStmt/parseType` 入口维护
  `p->depth`，超限报"nesting too deep"（**这是编译器该有的诊断，不是崩溃**）；
  dataflow 改成显式栈/迭代，或把 `Facts` 放堆上并限制递归深度；
  `blkMax*` 去重 + 记忆化（见 P2-1）。

### P0-11 `d.run { }` 不计入块层级 ⇒ `__extc_a[]` 少一格，生成物栈越界 `[Lead 复现]`

- **位置**：`src/codegen.c:3642`（`blkMaxLevel` 无 `ST_DOMAIN` 分支）；`genStmtInner` 的 `ST_DOMAIN`（`:3778`）
  却通过 `genBlockBody` 让层级 +1。
- **代码**：
  ```c
  case ST_WHILE: return 1 + blkMaxOfBlock(s->u.whiles.body);
  ...
  default: return 0;      /* ST_DOMAIN（以及所有非块语句）落在这里 */
  ```
- **成因**：`extc_arena __extc_a[maxLv + 1]` 的大小来自 `1 + blkMaxOfBlock(f->body)`；
  域块里的分配/释放用的是**更深一格**的 `&__extc_a[2]`，而数组只开了 2 格（下标 0..1）。
- **最小复现**：
  ```extc
  use std::sys::domain
  use std::io
  fn main() -> i32 {
      let d = domain::single()
      var tot: i64 = 0
      d.run { var buf: mut slice<u8> = new u8[4]  buf[0] = 7  tot = tot + i64(buf[0]) }
      io::cout << tot << "\n"
      return 0
  }
  ```
  ```sh
  ./build/extc -w dom.extc -o dom.c          # 退出码 0
  grep -n '__extc_a\[' dom.c
  # 941: extc_arena __extc_a[2] = {0};
  # 953: extc_arena_release(&__extc_a[2]);      ← 越界
  gcc -std=c11 -O0 -g -fsanitize=address -fwrapv dom.c -o dom && ./dom
  # → AddressSanitizer: stack-buffer-overflow ... READ of size 8 ... in extc_arena_release
  ```
  协程变体更响：帧里只布局了 `arena1`，代码却引用 `f->arena2` ⇒ 生成物直接编不过。
- **建议修法**：`blkMaxLevel` 补 `ST_DOMAIN`（`1 + blkMaxOfBlock(s->u.domain_.body)`），
  并给这份"层级计数"与 `genBlockBody` 加同一张表驱动的断言（两者的 kind 集合必须一致）。
  这类"两个 walker 必须一致"的地方应该由一个共享的 `stmtChildren()` 生成，而不是手写两遍。

### P0-12 f64 整数值字面量被发成 C **整型常量** ⇒ 纯字面量浮点运算按整数算 `[Lead 复现]`

- **位置**：`src/codegen.c:2203`（`case EX_FVAL:`）：
  ```c
  if (isF32) return arenaPrintf(g->arena, "(float)%.9g", e->u.fval);
  return arenaPrintf(g->arena, "%.17g", e->u.fval);
  ```
- **成因**：`%.17g` 对整数值的 double 输出 `1`、`1000000000` 这样的**没有小数点/指数**的文本，
  在 C 里就是 `int` 常量。f32 分支有 `(float)` 转换救回类型，f64 分支没有任何标注。
- **实测**（`/tmp/rev/v4/fmore2.extc`）：
  ```extc
  use std::io
  fn main() -> i32 {
      io::cout << (1.0 / 3.0) << " " << (7.0 / 2.0) << " " << (1.5 / 2.0) << "\n"
      io::cout << (1000000000.0 * 1000000000.0) << "\n"
      return 0
  }
  ```
  ```sh
  ./build/extc -w fmore2.extc -o fmore2.c
  grep -o '(1 / 3)\|(7 / 2)\|(1000000000 \* 1000000000)' fmore2.c
  # → 全部命中：生成物是整数运算
  gcc -std=c11 -O0 -fwrapv fmore2.c -o fmore2 && ./fmore2
  # 实际输出: 0 3 0.75          ← 1.0/3.0 与 7.0/2.0 被整数除法截断
  #            -1.48662e+09      ← 1e9*1e9 在 int 里溢出（gcc 同时给 -Woverflow）
  # 期望输出: 0.333333 3.5 0.75 / 1e+18
  ```
  同类：`-0.0` → `double z = (-0);` ⇒ `1.0 / -0.0` 打印 `inf` 而不是 `-inf`（`codegen-2` 独立报同一根因）。
- **影响**：这是**常量级**的错误结果，任何"用浮点字面量算常数"的代码都会静默算错；
  1e9*1e9 还落在有符号溢出（UB）上。
- **建议修法**：发射后检查文本里是否含 `.`/`e`/`E`/`n`/`i`，没有就补 `.0`（或统一输出 `(double)` 前缀）；
  顺手把 `-0.0` 的符号保住（`%.17g` 给 `-0`）。

### P0-13 `?ref [N]T` 的下标/切片不做非空检查 ⇒ 生成物解引用 NULL `[Lead 复现]`

- **位置**：`src/check_expr.c:1684`（`EX_INDEX`）与 `:1749`（`EX_SLICE`）。
  `rejectNullableDeref` 只在 `EX_FIELD`(`:1615`)、`EX_DEREF`(`:2697`)、`EX_METHOD`(`:3381`) 被调用。
- **最小复现**：
  ```extc
  fn main() -> i32 {
      var p: ?ref [4]i32 = null
      return p[1]
  }
  ```
  ```sh
  ./build/extc -w nullable_idx.extc -o nullable_idx.c    # 退出码 0，零诊断
  gcc -std=c11 -O0 -g -fsanitize=address -fwrapv nullable_idx.c -o ni && ./ni
  # → AddressSanitizer: SEGV on unknown address 0x000000000004 ... in main nullable_idx.extc:3
  ```
- **建议修法**：把 `rejectNullableDeref(c, objType, objExpr, "...")` 补到 `EX_INDEX`/`EX_SLICE` 两个分支，
  与另外三处对齐（receiver 类型为 `?ref` 且未证明非空 ⇒ 编译错误）。

### P0-14 `parallel::run()` 零实参 ⇒ 编译器 SIGSEGV `[Lead 复现]`

- **位置**：`src/check_expr.c:3027`（内建分派在 arity 检查之前取 `args[0]`，`vecAt` 无边界检查 ⇒ 空 `Vec` 的 `NULL` 基址被解引用）；
  同一段逻辑在 `:3527` 有第二份拷贝。
- **最小复现**：
  ```extc
  use std::parallel
  fn main() -> i32 { var r: i32 = parallel::run()  return r }
  ```
  ```sh
  ./build/extc -w parzero.extc -o /dev/null      # → Segmentation fault (rc=139)
  ```
  对照：传 1 个实参会正常报参数错误，只有 0 个崩。
- **建议修法**：把 arity/实参检查提到取 `args[0]` 之前；两份拷贝合并。

### P0-15 `for` 脱糖把步进放在体尾 ⇒ 任何 `continue` 都死循环 `[Lead 复现]`

- **位置**：`src/parser.c:2457`（`for x in` 形式）、`:2394`（range 形式）、`:2343`（C 风格形式）。
  脱糖结果是 `while cond { BODY; step }`，`continue` 直接跳回条件、步进永不执行。
- **最小复现**：
  ```extc
  use std::io
  fn main() -> i32 {
      var i: i32 = 0
      for i in 0..5 { if i == 2 { continue }  io::cout << i << "\n" }
      return 0
  }
  ```
  ```sh
  timeout 5 ./build/extc -w --run forcont.extc ; echo $?    # → 124（挂住，不是输出 0 1 3 4）
  grep -n 'continue' build/forcont.c                        # → while (...) { ... continue; ... i += 1; }
  ```
  同一循环换 `break`、或写成 `while` 都正常 ⇒ 只有 `for`+`continue` 组合受影响。
  仓库里唯一用 `continue` 的用例（`tests/arena/control-flow.extc`）恰好是 `while`，所以 311 例全绿。
- **建议修法**：脱糖时把步进放进 `continue` 的目标（C 里就是 `for (init; cond; step)` 形式，
  或把 `continue` 重写成 `{ step; continue; }`）；更稳的是让 `for` 在 AST 里保留自己的 kind，
  由检查器/代码生成共同处理 `continue`。

### P0-16 被调者经 `mut ref` 出参写入调用者对象不产生 publication ⇒ ASan UAF `[Lead 复现；报告者原级别 P1，已提升]`

- **位置**：`src/check_internal.h:497` 的 `StoreSite` 契约（"Every store … is one of these"）没有覆盖
  "被调方通过 `mut ref` 形参写入调用者对象"这条路径；目的绑定的 `refDepth`/字段深度停在旧值，
  晋升 walk 在 `src == NULL` 处停止。
- **最小复现**（`/tmp/rev2/addr4.extc`）：
  ```sh
  ./build/extc -w addr4.extc -o addr4.c        # 退出码 0
  gcc -std=c11 -O1 -g -fsanitize=address,undefined -fwrapv addr4.c -o addr4 && ./addr4
  # → AddressSanitizer: heap-use-after-free ... addr4.extc:14 in main
  #   (freed by extc_arena_destroy, allocated in the callee)
  ```
  把同样的存储直接写在调用者里（`b.p = ref n`）会被正确拒绝；栈版本（`addr1.extc`）报 stack-use-after-scope。
- **建议修法**：调用点按被调方的效果摘要（`addrMask`/`contMask` 已经有这些信息）把
  "目的绑定经过 `mut ref` 出参被写入"记进 `StoreSite`，或至少在 `EXTC_DBG_REFARGS` 显示已知的情况下
  让晋升 walk 不要停在 `src == NULL`。

### P0-17 字段非空证明在 `self: mut ref` 方法调用后不失效 ⇒ 生成物 SIGSEGV `[Lead 复现；报告者原级别 P1，已提升]`

- **位置**：`src/check_lookup.c:105`（`unNarrow` 只在 `check_stmt.c:686` / `check_expr.c:1896` 两处被调用；
  隐式接收者取地址既不置 `addressed` 也不 `unNarrow`）。
- **最小复现**（`/tmp/rev/lookup/popstale.extc`）：
  ```sh
  ./build/extc -w popstale.extc -o pop.c     # 退出码 0
  gcc -std=c11 -O0 -g -fsanitize=address -fwrapv pop.c -o pop && ./pop
  # → AddressSanitizer: SEGV ... popstale.extc:20 in main
  #   （`if s.top != null { s.pop()  return s.top.value }` 中 pop 把 top 置空，证明未失效）
  ```
  `examples/path-narrowing.extc` 明写了这条被违反的不变量。
- **建议修法**：任何"取地址/可能改写"的调用（尤其 `mut ref self` 方法）都应使该对象的路径非空证明失效；
  把 `unNarrow` 挂到"隐式取接收者地址"的那一处。

### P0-18 `i8`/`i16`/`u8`/`u16` 算术不回截 ⇒ 值逃出声明类型 `[Lead 复现；报告者原级别 P1，已提升]`

- **位置**：`src/codegen.c:1379`（二元）、`:2260`（一元）、`:1373`（移位）。
- **最小复现**：
  ```extc
  use std::io
  fn main() -> i32 {
      var a: i8 = 100
      var x: i8 = a + a
      io::cout << "sum_as_i64=" << i64(a + a) << " x=" << i64(x) << " eq=" << (a + a == x) << "\n"
      return 0
  }
  ```
  ```sh
  ./build/extc -w i8arith.extc -o i8arith.c && gcc -O0 -fwrapv i8arith.c -o i8arith && ./i8arith
  # 实际: sum_as_i64=200 x=-56 eq=false      生成物: int8_t x = (a + a);
  # 期望（i8 语义）: 200 回绕成 -56，且 a+a == x 应为 true
  ```
  检查器认定 `a + a` 的类型是 `i8`（`var x: u16 = a+a` 报 "found `i8`"），所以这是**类型系统与实际值不一致**；
  文档承诺的"溢出按 wrap 定义"在窄类型上不成立。
- **建议修法**：窄类型（<32 位）的算术结果在发射时回截到目标类型（`(int8_t)(...)`），
  或在检查器里把窄类型运算提升到 `i32` 并**显式**在类型上体现（需要语言侧定案）。

### P0-19 `return e?` 的载荷不做相容比较 ⇒ 静默窄化 `[Lead 复现；报告者原级别 P1，已提升]`

- **位置**：`src/check_stmt.c:1085`（`wb->kind == TY_GENERIC` 对 `option`/`result` 恒假——它们是 `TY_ENUM`）。
- **最小复现**：
  ```extc
  use std::io
  fn f(o: option<i64>) -> option<u8> { return o? }
  fn main() -> i32 {
      var r: option<u8> = f(option<i64>::some(1000))
      match r { some(v) => io::cout << "v=" << i64(v) << "\n"  none => io::cout << "none\n" }
      return 0
  }
  ```
  ```sh
  ./build/extc -w optnarrow.extc -o optnarrow.c && gcc -O0 -fwrapv optnarrow.c -o optnarrow && ./optnarrow
  # → v=232        （1000 被静默截断；直接写 success(300) 反而会被正确拒绝）
  ```
- **建议修法**：`?` 的载荷走与返回语句相同的 `checkAssignable` 路径（`option<T>`/`result<T,E>` 是 `TY_ENUM`，
  需要按枚举实例的 targs 比较，而不是按 `TY_GENERIC`）。

### P0-20 任务结束时按 LIFO 释放"任务的地方" ⇒ 弹掉仍存活的任务 zone，生成物 UAF `[Lead 复现]`

- **位置**：`src/coroutine.c:84`（`extc_task_end` → `extc_pool_zoneLeaveTo(任务 zone)`）；
  codegen 侧的配合：boxed spawn 不恢复 `zoneTop`（`codegen.c:3805-3826`），而 `$next` 先恢复 `zoneTop`
  再调 `extc_task_end`（`codegen.c:4612-4613`）。
- **成因**：任务的存活顺序**不是 LIFO**（A 先建后完、B 后建未完），但结束一个任务时直接把 zone 栈
  弹到"该任务的 zone"的位置，于是把栈上更高、仍然存活的任务 zone 一起弹掉；那些任务的池块随之被释放。
- **最小复现**（报告者 `/tmp/coro/z2.extc`：两个 boxed 任务各持一个 `vector`，先跑完较老的那个）：
  ```sh
  ./build/extc -w z2.extc -o z2.c
  gcc -std=c11 -O1 -g -fsanitize=address -fwrapv z2.c -o z2 && ./z2
  # → AddressSanitizer: heap-use-after-free ... z2.extc:15 in B$step
  #   free 栈: extc_pool_drop ← extc_pool_zoneLeaveTo ← extc_task_end
  ```
- **同处相反方向**（`P2`）：unboxed 任务时调用者 zone 更低 ⇒ 一个 zone 都不弹，
  实测 20000 个结束任务 RSS **85.9 MB**（对照 1.4 MB），≈4.2 KB/任务。
- **建议修法**：任务的 zone 不参与 LIFO 弹栈——按"任务自己的 mark"精确释放（记录并只归还该任务分配的块），
  或者干脆让每个任务持有独立 arena，结束时整只销毁（`extc_arena_destroy`），不动全局 zone 栈。

### P0-21 worker 用到的共享 helper 被全程改走 TLS arena ⇒ 普通调用直接空指针崩溃 `[Lead 复现；报告者原级别 P1，已提升]`

- **位置**：`src/check_expr.c:149`（`parBodyProblem`）把 `parTlsArena` 打在**整条 worker 调用链**的函数上（函数级标志），
  codegen 于是让这些函数体里的所有分配都走 `(*extc_tls_arena)`；而该指针只在 worker 线程里被赋值，
  主线程恒为 `NULL`。
- **最小复现**（`/tmp/rev-ce/par2.extc`）：
  ```extc
  use std::parallel
  fn helper(n: i64) -> i64 {          // 既被 worker 调用，也被 main 调用
    var t: mut slice<i64> = new i64[4]
    t[0] = n
    return t[0]
  }
  fn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {
    var i: i64 = lo
    while i < hi { out[i - lo] = helper(i)  i = i + i64(1) }
    return 0
  }
  fn main() -> i32 {
    var out: mut slice<i64> = new i64[64]
    let before: i64 = helper(i64(3))            // 主线程调用同一个 helper
    if parallel::run(w, out, i64(64), i64(8)) != i32(0) { return i32(1) }
    return i32(before) - 3
  }
  ```
  ```sh
  ./build/extc -w par2.extc -o par2.c          # 退出码 0
  gcc -std=c11 -O0 -g -fsanitize=address -fwrapv -pthread par2.c -o par2 && ./par2
  # → AddressSanitizer: SEGV on unknown address 0x000000000000
  #   in extc_arena_alloc (par2.c:120)   ← *extc_tls_arena 为 NULL
  ```
- **影响**：`parallel::run` 的"worker 里调一个共享工具函数"是最普通的写法，生成物必崩；
  它同时说明"把函数级标志当作线程归属"这个建模方式是错的（同一函数可以在两种上下文里被调用）。
- **建议修法**：TLS arena 只能用于"确定在 worker 里执行"的函数体（调用图分析要区分入口），
  或让生成物使用"`extc_tls_arena ? *extc_tls_arena : 当前 arena`"的兜底；更稳的是让 worker
  自己在入口处设置 TLS，并只对直接从 worker 可达的分配点做替换。

### P0-22 编译 `stdlib/stl/hashSet.extc` 永不返回：`dropRuntimeDefs` 的 `continue` 跳过循环推进语句 `[Lead 亲测定案]`

- **位置**：`src/codegen.c:5798`（`dropRuntimeDefs` 内的 `if (!defs) continue;`），
  所属循环 `for (char *ln = rp; ln < rp + rl && !cut; )` 在 `:5714`，其推进语句在循环末尾 `:5848-5849`。
- **代码**：
  ```c
  for (char *ln = rp; ln < rp + rl && !cut; ) {        // :5714  唯一的推进在末尾
      ...
      if (isMacro) {
          ...
          totalOverride  = (long)countMentions(text, name) - (long)guards;
          insideOverride = (long)defs;
          if (!defs) continue;                 /* :5798 → 跳回循环头，ln 不动 */
      }
      ...
      if (!eol) break;
      ln = eol + 1;                                     // :5848-5849
  }
  ```
- **成因**：`continue` 不执行末尾的 `ln = eol + 1`。只要出现"这一行是 `#define`、名字以 `extc_`/`EXTC_` 开头、
  但 `defs == 0`"的状态，就永远停在**同一行**。这个状态是上一轮 `dropRuntimeDefs` 自己制造的：
  它把运行期文本切出一个**被截断的 `#define EXTC_ZON` 片段**（原为 `#define EXTC_ZONE_*`），
  下一轮扫到这行时名字后面没有可判定的非标识符字节，`defs` 算成 0 ⇒ 死循环。
- **现象与证据**（每一步都可复现）：
  ```sh
  timeout -s KILL 20 ./build/extc -w stdlib/stl/hashSet.extc -o /dev/null ; echo rc=$?   # → 124
  # 不是死锁：进程状态 R、CPU 99.8%（死锁会是 0% CPU 的 D/S 状态）
  ```
  阶段计时（`EXTC_DBG_TIME=1`）：所有 pass 都在毫秒级完成（check 0.003 s、cg-emit…cg-runtime 各 0.000–0.004 s），
  **卡在 `cg-prune` 的第二轮**。把 `src/` 拷到 `/tmp` 打点重编（仓库源码未改动）后：
  ```
  [dbg] prune round 0 start len=84050 → end len=58180
  [dbg] prune round 1 start len=58180
  [dbg] dropUnusedLocals ENTER len=58180
  [dbg] dropRuntimeDefs ENTER len=35290 rl0=9675
  [dbg] RTD   line 9130000 ln_off=10009 rl=9675        ← 同一位置被扫 900 万次
  [dbg] SPIN line=
  <<<#define EXTC_ZON>>>
       ll=16 name='EXTC_ZON' nlen=8 rl=9675 guards=0
  ```
- **影响**：`stdlib/stl/hashSet.extc`（**52 行**）作为根文件编译时 100% CPU 永不返回、无任何诊断；
  全语料 `--check-c` 扫描与 CI 同类扫描都会被它挂住（实测三次独立运行：>120 s、>90 s、>15 min）。
  用 `use stl::hashSet` 的程序（`tests/stl/clone.extc`）**不触发** —— 触发取决于该模块被当作根文件时
  运行期文本被切成的状态；但循环缺陷本身是通用的（任何"找到候选但不删"的状态）。
- **修复建议**（一行）：`if (!defs) { if (!eol) break; ln = eol + 1; continue; }`，
  或把推进写进 `for` 的第三段，使任何 `continue` 都不可能漏推进。
  同族的两个 `for(;;)` 扫描循环（`:5709`、`:5979`）应一并审计：**每个"找到候选但不删"的分支都必须推进位置**。

---

## 3. P1：健全性漏洞、误拒、产物编不过

> 共 26 条。为便于按主题修，下面按机制分组；每条都给位置与最小复现。

### 3.1 逃逸/生命周期（3 条，均 `[Lead 复现]`，与 P0-6 同族）

| # | 位置 | 症状 | 最小复现 |
|---|---|---|---|
| P1-1 | `src/check_escape.c:147` | `placeDepth` 对 `placeRoot` 解不出的地方返回 `0`（= 活得最久），方向反了；`G = v` 被拒而 `G = [v,v][0]` 放行 | `/tmp/rev/repro/n1.extc` vs `n2.extc` |
| P1-2 | `src/check_escape.c:726` | 参数按**名字**匹配；合法遮蔽（`var s = q`）后约束记到错参数上，调用点检查了另一个实参 | `/tmp/rev/repro/m3.extc`（`m2.extc` 无遮蔽则被正确拒绝） |
| P1-3 | `src/check_escape.c:1447` | `needsHome` 早退让"有 home arena 的函数"可以往**全局**存借用值；注释只覆盖"目的地与 home 同寿" | `/tmp/rev/repro/c4.extc` 跑出 `G = AAAA…` |

**建议**：见 P0-6 末尾的统一修法；另外把参数匹配从 `Param->name` 改成 `Sym *`（`declare` 时已建立绑定）。

### 3.2 泛型/实例化（4 条）

| # | 位置 | 症状 | 复现 |
|---|---|---|---|
| P1-4 | `src/check_top.c:2476` | 效果摘要掩码是 `unsigned` 32 位，函数可以有 >32 个参数：`1u << j` 是 UB，且消费端循环停在 `j < 32` ⇒ 第 33 个起的实参**从不检查** | `/tmp/rev/findings/wide2.extc`（33 参）→ ASan `stack-use-after-scope`；4 参对照 `small2.extc` 被拒绝 |
| P1-5 | `src/check_top.c:2497` | `fresh` 局部集合只在声明时建立、从不因"重新指向"失效 ⇒ 被调方效果摘要为空且 `complete=1`，调用点整段生命周期检查被跳过 | `/tmp/rev/findings/fresh2.extc` → ASan `stack-use-after-scope` |
| P1-6 | `src/check_top.c:230` | `structOf(target)` 为 NULL 时 `continue`，整段 trait/impl 一致性检查对**内建类型与视图**（`i64`、`slice<u8>`…）被跳过 | `/tmp/x/tr3.extc`：`impl Codec for slice<u8>` 返回类型不符也被接受 |
| P1-7 | `src/check_top.c:289` | 返回类型只在"两边都非 void"时比较；trait 写 `-> i64` 而 impl 写 `void` 被接受 ⇒ dyn thunk 生成 "void value not ignored" | `/tmp/x/tr2.extc` → `--check-c` 报错 |

**建议**：掩码换成 `uint64_t` + 编译期断言 `params.len <= 64`（或改成按参数下标存的动态数组）；
`fresh` 集合要么在每次赋值/重定向时失效，要么改成"来源链"判定；
trait/impl 一致性对"没有 StructDef 的目标"也要走签名比对（用 trait 的签名即可）；
返回类型比较以 `NULL == void` 的规范化形式比较（`ttIsVoid`），不要用"两边都非 NULL"当门。

### 3.3 协程（6 条，均 `[Lead 复现]`）

| # | 位置 | 症状 | 复现（本机已跑） |
|---|---|---|---|
| P1-8 | `src/codegen.c:4680` | 方法是协程时，codegen 用 `cFuncName`（`tick_count$frame`）而 checker 建的结构体叫 `count$frame` ⇒ 帧结构从未定义 | `/tmp/rev/v3/coro_m.extc` → `struct tick_count$frame` 未定义 |
| P1-9 | `src/codegen.c:4953` | 递归 + 非 void + 可能走到函数尾 ⇒ 生成 `return __extc_ret_v;`，而该槽在 `noArena` 时根本没声明 | `/tmp/rev/v3/r_falloff.extc` → `'__extc_ret_v' undeclared` |
| P1-10 | `src/codegen.c:4538` | 句柄辅助函数把 `cType(yieldType)` 拼进**标识符**：`ref` 产出类型给 `extc_coro_value_int64_t *(...)`，调用点被解析成乘法 | `/tmp/rev/v3/coro_refyield.extc` → `expected '=' ... before '*' token` |
| P1-11 | `src/codegen.c:4852` | 协程体里有 `@overwrite` ⇒ 序言生成 `&__extc_a[1]`，但 `coroFunc` 强制 `noArena`，`__extc_a` 没声明（该 cell 本身也是死的） | `/tmp/rev/v3/coro_ow.extc` → `'__extc_a' undeclared` |
| P1-12 | `src/codegen.c:4901` | 递归协程调了 `extc_rec_enter` 却在协程分支提前 return，从不配对 `cgRecLeave` ⇒ 计数器只增不减；**顺序创建 10 万个协程后**程序被假的 `recursion too deep` 杀掉（真实深度 ≤ 2） | `/tmp/rev/v3/coro_recguard.extc` → `trap: recursion too deep`（k≈100002） |
| P1-13 | `src/codegen.c:4521` | 泛型协程被 box 成句柄时，`coroNeedsZone`/`$next` 都对实例跳过 ⇒ `extc_task_end` 永不调用：任务 arena 与 zone **每次创建泄漏**（实测：30 万次创建 57 MB RSS，2000 次 1.8 MB） | `/tmp/rev/v3/coro_generic_leak.extc`（`k < 300000`）|
| P1-14 | `src/codegen.c:3841` | 以栈帧方式驱动的"带池协程"：spawn 在任务结束前就把 `extc_zoneTop` 还原，`extc_pool_zoneLeaveTo(f->zone)` 变成 no-op ⇒ 整个 place 泄漏（5 万次 spawn 263 MB） | 报告者 `/tmp/zl.extc`（`[报告者复现]`，本机未重跑） |

**建议**：协程这条线的问题集中在"帧/句柄是**合成的**结构，但三处代码各自拼名字、各自决定布局"。
把"帧结构命名 + 字段布局 + 句柄辅助函数名（用 mangle 过的稳定名而非 `cType`）"收敛到**一个**函数，
并由 checker 侧产出唯一的 `CoroLayout` 记录，codegen 只消费它；递归进出/任务结束用 `goto done` 单出口保证配对。

### 3.4 生成物与 C 语义（4 条）

| # | 位置 | 症状 | 复现 |
|---|---|---|---|
| P1-15 | `src/codegen.c:1334` | 重载运算符的右操作数是 `ref` 时无条件加 `&(...)`，不看操作数本身是否已是引用 ⇒ `a == ref b` 生成 `&(&(b))` | `/tmp/rev/work/d_opref.extc` → `lvalue required as unary '&' operand` |
| P1-16 | `src/codegen.c:2206` | 字符串字面量原样重发到 C：extC 的 `\x` 最多吃 2 个十六进制位（`lexer.c:lexEscape`），C 的 `\x` 吃到不能吃为止 ⇒ `"\x41B"` 在 extC 是 `"AB"`，在生成物里是 `0x1B` | `hexstr.extc`：输出字节 `1b 7c 41 42 0a`（应为 `41 42 7c 41 42 0a`）；gcc 只给 `hex escape sequence out of range` 警告 |
| P1-17 | `src/codegen.c:3756`/`:4024` | `cgLine` 在**没有** `flushPrefix` 的情况下求值表达式（或反之），前缀里排队的临时量在使用它的语句**之后**才生成 ⇒ 未声明标识符 | `trap(new u8[n()])` → `'__extc_n1' undeclared`；`a += (get() ?? vec2{...})` 同族 |
| P1-18 | `src/check_top.c:857` | `countOwSites` 不下探 `ST_DOMAIN`，而 codegen 的 `collectOwSites` 下探 ⇒ `d.run { }` 里的 `@overwrite` 让 `needOw` 为假、`extc_owcell` 类型从不生成，但 cell 照发 | `/tmp/x/dom_ow.extc` → `unknown type name 'extc_owcell'` |

**建议**：字符串转义要么在词法阶段就解码成字节（推荐，长度也顺带正确），要么按 C 规则重新转义后发出；
`cgLine`/`pfLine` 的"前缀必须先冲刷"应当做成 API 约束（例如求值统一走 `cgEval()`，内部先 flush）；
`countOwSites`/`collectOwSites` 共享一个 walker。

### 3.5 转换与运行期数值（3 条，均 `[Lead 复现]`）

| # | 位置 | 症状 | 复现 |
|---|---|---|---|
| P1-19 | `src/codegen.c:2615-2654` | 目标是 64 位时，`extc_narrowI`/`extc_narrowU` 的实参**先被 cast 到 64 位域**再做范围检查 ⇒ 检查恒真：`u64`→`i64` 给 `-1`、`i64`→`u64` 给 `2^64-1`，都不 trap（文档承诺"装不下 ⇒ trap"）。对照：`u32→i32`、`i32→u32`、`u8→i8`、`i16(40000)` 都正确 trap | `/tmp/rev/atk/c_u64_i64.extc`、`c_i64_u64.extc` |
| P1-20 | `src/codegen.c:7278` | `extc_convFloat` 用 `v <= (double)hi` 判上界，而 `(double)INT64_MAX` 向上取整成 `2^63` ⇒ 恰好 `2^63` 不 trap，随后 `(int64_t)v` 是 UB（`-O0`/`-O2` 结果不同） | `/tmp/t13.extc`：打印 `9223372036854775808`，UBSan 报越界 |
| P1-21 | `src/codegen.c:7326` | `extc_arena_alloc` 的 `n = (n + 7) & ~(int64_t)7` 对 `n > INT64_MAX-7` 溢出（有符号 UB）⇒ 不分配新块、`used` 下溢，返回一个"长度为 INT64_MAX、背靠 64 字节块"的切片 | `arena_ovf2.extc`：`p[1000000] = 1` → ASan `SEGV on ... WRITE`（本该是 `out of arena memory` trap） |

**建议**：检查必须在**源域**做（按源类型的符号/宽度生成不同的比较，或先在宽类型里比较再 cast）；
float→int 的上界用 `v < 9223372036854775808.0`（对 i64）这类**可精确表示**的界；
`extc_arena_alloc` 加 `if (n > INT64_MAX - 7) trap("out of arena memory")`，并让 `new T[n]` 的 `n*sizeof(T)`
做溢出检查（同一份生成文本里还有 `n * sizeof(T)` 未检查）。

### 3.6 其余 P1（2 条）

| # | 位置 | 症状 | 复现 |
|---|---|---|---|
| P1-22 | `src/check_top.c:6750` | 没有 `yield` 的协程在 `coroFrameLay` 提前返回，帧结构没有 `pc`/`arenaN`/参数成员，生成物编不过 | `/tmp/c1.extc` → `'struct c$frame' has no member named 'pc'` |
| P1-23 | `src/codegen.c:8402` | `parallel::run(..., threads=0)`：生成物里 `n / (threads * 8)` 除零 ⇒ SIGFPE；运行期注释明说 `threads <= 1` 无害，且唯一调用点固定传 `chunk = 0` | 报告者 `/tmp/t1.extc`（`[报告者复现]`） |

---
### 3.7 完整 P1 清单（按域）

> 由各审阅单元的 JSON 汇总，标题保留原文（英文标题为审阅者所写）。共 56 条。
> "复现"细节在各单元 JSON 与 `/tmp/rev/findings`、`.audit/findings` 中；
> 下表按域分组，便于按主题成批修。
> §3.1–§3.6 里详述过的条目在下表中同样出现（位置相同，可对照）。
> 另有若干条被我在正文中提升为 P0（`check_escape.c:1447`、`check_lookup.c:105` 等），它们在表中仍按报告者原级别列出。
>
> **我另外独立复现过的 P1（本机重跑，命令在附录 B 的"复现"列）**：
> `check_expr.c:3753`（泛型方法的类型参数未实例化 ⇒ `box_len` 实参类型不符）、
> `check_stmt.c:1085`（`return o?` 1000→232）、`types.c:961`（`-> void` 里 `return` 被误拒）、
> `types.c:483`（用户 `struct array_3_i32` 与内建数组类型名相撞 ⇒ 产物重定义）、
> `check_top.c:289`（trait `-> i64` 被 `void` 实现 ⇒ "void value not ignored"）、
> `check_top.c:230`（`impl Trait for slice<u8>` 跳过签名比对）、
> `check_top.c:857`（`d.run{}` 里的 `@overwrite` ⇒ `extc_owcell` 未定义）、
> `check_lookup.c:671`（聚合里的协程句柄零值 → 驱动 NULL 帧）、
> `parser.c:3143`（字符串里的原始 NUL 让 `"a<NUL>b"` 变成 `"a"`）、
> `check_expr.c:149`（worker 共享 helper 的 TLS arena → 主线程空指针，已升 P0-21）。


#### 逃逸/生命周期（P1 × 4）

| 位置 | 类型 | 摘要 | 来源 |
|---|---|---|---|
| `src/check_escape.c:147` | UNSOUND | placeDepth answers 0 ('lives forever') for a place whose root is not a binding, so a local view laundered through an index/slice of a temporary is ac… | escape-1 |
| `src/check_escape.c:726` | UNSOUND | valTracesToParam / destIsParam compare parameter *names*, so a local that shadows a parameter makes the store publish its lifetime constraint against… | escape-1 |
| `src/check_escape.c:1447` | UNSOUND | checkStoreEscape's `needsHome` early return lets any function that has a home arena store a borrowed reference into a global | escape-1 |
| `src/check_escape.c:2381` | BUG | main 里 `?` 的操作数只查 targs.len>0，非 option/result 的泛型类型被当成 option 解包 ⇒ 生成物编不过 | escape-2 |

#### 泛型/实例化/类型系统（P1 × 9）

| 位置 | 类型 | 摘要 | 来源 |
|---|---|---|---|
| `src/check_top.c:230` | UNSOUND | Trait/impl conformance is skipped entirely for targets without a StructDef (builtins and views): `impl Trait for i64` / `for slice<u8>` may contradic… | checktop-1 |
| `src/check_top.c:289` | MISCOMPILE | Trait/impl conformance compares return types only when BOTH are non-void, so a trait method returning a value may be implemented as `void` (and vice … | checktop-1 |
| `src/check_top.c:857` | MISCOMPILE | `countOwSites` does not descend into `ST_DOMAIN`, while codegen's `collectOwSites` does: an `@overwrite` inside `d.run { }` makes the compiler emit a… | checktop-1 |
| `src/check_top.c:2476` | UNSOUND | Effect-summary masks are 32 bits but a function may have more than 32 parameters: `1u << j` is UB and every argument at index >= 32 is never checked | checktop-2 |
| `src/check_top.c:2497` | UNSOUND | `fresh` local set is built once from declarations and never invalidated by retargeting: the callee's effect summary comes out empty and the call-site… | checktop-2 |
| `src/check_top.c:6750` | MISCOMPILE | MISCOMPILE: a coroutine whose body has no `yield` skips coroFrameLay's field layout, so the generated frame struct has no members | checktop-3 |
| `src/check_top.c:6850` | BUG | 泛型协程实例的帧漏掉 `var x = yield e` 引入的绑定（coroBinds 按模板函数指针匹配）⇒ 生成物编不过 | checktop-4 |
| `src/types.c:483` | MISCOMPILE | The generated C name of an array type (`array_<len>_<elem>`) and of a generic-enum instance (`<enum>_<arg>`) is not reserved: a user declaration with… | types |
| `src/types.c:961` | FALSE-REJECT | `ttIs(t, "void")` can never be true (`void` is TY_VOID, not TY_BUILTIN): a bare `return` inside any `-> void` function and every void-returning lambd… | types |

#### 表达式与检查器（P1 × 14）

| 位置 | 类型 | 摘要 | 来源 |
|---|---|---|---|
| `src/check.c:176` | MISCOMPILE | `cNameFor` 生成的 `名字__N` 会与用户自己写的 `名字__N` 撞名：产物重定义或静默错值 | lookup |
| `src/check_expr.c:108` | UNSOUND | worker 白名单把"没有 effects 子句的 extern!"当成 Thread=0（extThreadMask 缺省为 0）⇒ worker 可任意调用共享状态的 C 函数 | checkexpr-1 |
| `src/check_expr.c:149` | MISCOMPILE | worker 链上的函数被普通调用时去解引用 NULL 的 extc_tls_arena（生成物必然空指针崩溃） | checkexpr-1 |
| `src/check_expr.c:359` | FALSE-REJECT | 字面量适配先于隐式拓宽、且失败即报错 ⇒ 同一表达式因字面量左右位置不同而一收一拒 | checkexpr-1 |
| `src/check_expr.c:971` | BUG | lambda 在泛型函数里捕获 T 时，环境结构体字段保留未替换的类型参数（codegen 退化成 int） | checkexpr-1 |
| `src/check_expr.c:1376` | MISCOMPILE | `<`/`<=`/`>`/`>=` 的左操作数是 ref 时不报错，静默生成指针比较 | checkexpr-1 |
| `src/check_expr.c:3150` | FALSE-REJECT | unifyTParams cannot unify an enum instance: a generic function with an `option<T>` / `result<T,E>` parameter can never be called | checkexpr-2 |
| `src/check_expr.c:3457` | FALSE-REJECT | `dyn` 派发的实参检查不做上下文定型 ⇒ `d.area(3)` 误拒（静态调用同一行合法） | checkexpr-3 |
| `src/check_expr.c:3459` | UNSOUND | `dyn` 派发的实参检查剥掉 `ref` 再比 ⇒ 接受 `ref i64` 形参收到裸值，生成的 C 编不过 | checkexpr-3 |
| `src/check_expr.c:3753` | BUG | 方法的类型参数在签名里没被实例化 ⇒ 生成的 C 引用未定义的 `slice_U`（编不过），标量时把 `u8` 和 `i64` 当成同一个 U | checkexpr-3 |
| `src/check_lookup.c:105` | UNSOUND | 字段路径的非空证明在 `mut ref self` 方法调用后不失效 ⇒ 生成物解引用 NULL | lookup |
| `src/check_lookup.c:671` | UNSOUND | 聚合里的 `coroutine<T>` / `dyn` 句柄未被当作『没有零值』⇒ 零句柄驱动 NULL 帧（SEGV） | lookup |
| `src/check_stmt.c:1085` | MISCOMPILE | `return e?` 的载荷类型从不与函数返回类型的载荷比较（wb->kind == TY_GENERIC 对 option/result 恒假）⇒ 静默截断 | stmt |
| `src/codegen.c:5109` | BUG | worker 需要 zone 形参时 trampoline 不补传 __extc_home_zone ⇒ 产物编不过（too few arguments） | checkexpr-1 |

#### 代码生成（P1 × 23）

| 位置 | 类型 | 摘要 | 来源 |
|---|---|---|---|
| `src/codegen.c:1334` | MISCOMPILE | genBin wraps a `ref` right operand in `&(...)` without checking whether the operand is already a reference: `a == ref b` emits `&(&(b))` | codegen-1 |
| `src/codegen.c:1379` | MISCOMPILE | Integer +,-,*,<<,>>,~,- on i8/i16/u8/u16 are computed at C `int` width and never truncated: the value escapes its declared type | differential |
| `src/codegen.c:1574` | BUG | Zero value of a generic instance whose type argument is `coroutine<T>` is emitted as `(extc_coro){ .unit = false }` - a designator for a field the re… | genwarn |
| `src/codegen.c:2206` | MISCOMPILE | String literals with `\xNN` followed by a hex-digit character are re-decoded by the C compiler under different rules: silently wrong bytes and length | codegen-2 |
| `src/codegen.c:2649` | MISSING_TRAP | Sign-changing 64-bit conversions never trap: u64(i64(-1)) silently yields 18446744073709551615 and i64(u64max) yields -1 | differential |
| `src/codegen.c:3665` | UB | -Warray-bounds on generated `__extc_a[3]` with an `extc_arena[2]` declaration: a domain block nested inside an `if` walks off the frame arena array (… | genwarn |
| `src/codegen.c:3756` | MISCOMPILE/FALSE-REJECT | cgLine evaluates an expression after flushPrefix (or without one), so the temporary it queues in the statement prefix is declared after the statement… | codegen-2 |
| `src/codegen.c:3841` | MISCOMPILE | Pool-owning coroutine driven as a stack frame leaks its whole place: the spawn restores extc_zoneTop before the task ends, so extc_task_end's zoneLea… | codegen-3 |
| `src/codegen.c:4521` | MISCOMPILE | A boxed instance of a generic coroutine never ends its task: the frame's task arena and zone are leaked and the dead-handle trap can never fire | codegen-3 |
| `src/codegen.c:4538` | FALSE-REJECT | Coroutine handle helpers embed the yield type's C spelling in an identifier: a `ref` yield type emits `extc_coro_value_int64_t *` and the call site p… | codegen-3 |
| `src/codegen.c:4608` | BUG | An unstarted coroutine that touches the domain layer keeps its `$next` wrapper but loses the `$step` body: `-Wundefined-internal` / 'used but never d… | genwarn |
| `src/codegen.c:4680` | FALSE-REJECT | Coroutine declared as a method references `struct <owner>_<name>$frame` while the frame struct is named `<name>$frame` and is never defined | codegen-3 |
| `src/codegen.c:4852` | FALSE-REJECT | A coroutine body that needs hidden frame state emits `__extc_a[1]` / `__extc_home`, neither of which exists in a step function (dead @overwrite cell,… | codegen-3 |
| `src/codegen.c:4901` | MISCOMPILE | Recursive coroutine increments the recursion guard but never decrements it: sequential spawns eventually die with a bogus `recursion too deep` trap | codegen-3 |
| `src/codegen.c:4953` | FALSE-REJECT | Recursive non-void function that can fall off its end emits `return __extc_ret_v;` for an undeclared variable (generated C does not compile) | codegen-3 |
| `src/codegen.c:6779` | BUG | A `coroutine<T>` named only in a data position (struct field / array element / generic type argument) emits `extc_coro` without ever emitting its typ… | genwarn |
| `src/codegen.c:6785` | BUG | event 层按需发射的触发名单漏了 `extc_unix_` ⇒ 只用 extc_unix_listen/connect 的程序链接失败（--check-c 却报通过） | coro |
| `src/codegen.c:7065` | BUG | A `dyn Trait` used only in a data position (struct field / array element / generic type argument) emits `ExtcDynHandle` without emitting its typedef | genwarn |
| `src/codegen.c:7278` | UNSOUND | f64 -> i64 conversion at exactly 2^63 is not trapped and is UB in the generated C ((double)INT64_MAX rounds up to 2^63) | codegen-4 |
| `src/codegen.c:7326` | UNSOUND | extc_arena_alloc's size rounding overflows for n > INT64_MAX-7, returning a huge slice backed by a tiny block (wild writes) | codegen-4 |
| `src/codegen.c:7696` | MISCOMPILE | A payload-free generic enum instance is emitted as a tag+union struct while its values are bare enumerators, so the generated C does not compile (ext… | codegen-4 |
| `src/codegen.c:7708` | MISCOMPILE | A coroutine declared as a method never gets its frame struct defined, so the generated C does not compile (extc exits 0) | codegen-4 |
| `src/codegen.c:8402` | CRASH | parallel::run with threads == 0 divides by zero in the generated C (SIGFPE), although the runtime documents threads <= 1 as harmless | codegen-4 |

#### parser/lexer/驱动（P1 × 4）

| 位置 | 类型 | 摘要 | 来源 |
|---|---|---|---|
| `src/domain.c:49` | UB | 域运行期保存块内局部帧的裸指针：`d.run { }` 在块结束之后才驱动帧 ⇒ 生成物 stack-use-after-scope | misc |
| `src/parser.c:392` | FALSE-REJECT | `effects` clause rejects `Ret` when it is not the first key (`effects Addr=0 Ret=0`), although `effects Ret=0 Addr=0` is accepted | parser-2 |
| `src/parser.c:1088` | UNSOUND | A trait method's `effects Ret=0` is believed at `dyn` call sites although no impl is ever checked against it: a view into a dead frame is accepted an… | parser-1 |
| `src/parser.c:2457` | NONTERMINATION | `continue` inside any `for` loop skips the loop step, so every `for` with a continue hangs forever | differential |

#### 模块系统（P1 × 3）

| 位置 | 类型 | 摘要 | 来源 |
|---|---|---|---|
| `src/modules.c:759` | FALSE-REJECT | rwType 只下探 TY_REF 与 targs：数组 `[N]m::T` 和函数类型 `fn(m::T)->R` 里的模块限定类型名永远不被改写（误拒合法程序） | modules-1 |
| `src/modules.c:1209` | FALSE-REJECT | EX_GENCALL 的 callee 名从不改写：模块里的泛型函数一旦写显式类型实参就再也调不到（误拒 + 误导诊断） | modules-2 |
| `src/modules.c:2005` | BUG | @private 类型不设防：alias 表收录了私有声明的返回票据，裸名（含传递 import）就能使用别人的私有 struct/enum | modules-2 |

#### AST/内部契约/基础设施（P1 × 8）

| 位置 | 类型 | 摘要 | 来源 |
|---|---|---|---|
| `src/ast.c:267` | BUG | `astWalkStmtChildren` 的 ST_ASSIGN 不访问 `u.assign.opExpr`：复合赋值里的用户运算符调用对**所有**基于 walk 的谓词不可见，产物编不过 | ast |
| `src/ast.h:192` | BUG | `Expr.func` 的跨 pass 契约只写了 EX_CALL/EX_METHOD：运算符节点（EX_BIN）也带 `func`，而每个基于 walk 的谓词只认三种 call kind ⇒ `var c = a + b` 的产物编不过 | ast |
| `src/ast.h:824` | UNSOUND | `FuncDef.extThreadMask` 的文档缺省值(1=碰共享状态)没有代码实现：无 `effects` 子句的 extern 被当成 Thread=0，并行 worker 里可静默调用它（TSan 实测 data race） | ast |
| `src/check_internal.h:497` | UNSOUND | 被调者通过 `mut ref`/`ref` 出参写入的存储不产生任何 publication：目的绑定的 `refDepth`/字段深度停在旧值，晋升走不到分配站点 ⇒ 编译器接受、产物 heap-use-after-free | internal |
| `src/coroutine.c:84` | BUG | boxed spawn 不还原 extc_zoneTop，extc_task_end 随后弹掉调用方仍在使用的地方（合法程序 trap/UAF） | coro |
| `src/coroutine.c:230` | BUG | event 块用 inet_pton 却只有 DNS 块 include <arpa/inet.h> ⇒ 只声明 extc_tcp_connect_to 的程序生成 C 编不过 | coro |
| `src/pools.c:46` | BUG | 池槽位回收时不清 `extc_poolKind`：容器池继承「对象表」模式 ⇒ 合法程序运行期假 trap（abort） | misc |
| `src/pools.c:51` | BUG | `extc_pool_take/resize` 把非正字节数静默钳成 1：`n * sizeof(T)` 溢出时给出 1 字节板块而切片 `.len` 仍是 n ⇒ 堆越界写 | misc |

---

## 4. P2：健壮性、边界与诊断（48 条）

> 这些多数不会立刻造成错码，但会让合法程序被误拒、诊断指错方向、或在特定形状下出错。
> 全部条目与复现要点如下（完整命令见附录 B / 各单元 JSON）。

| 位置 | 类型 | 摘要 | 复现要点 | 来源 |
|---|---|---|---|---|
| `src/dataflow.c:104` | UNSOUND | 第 5 个字段（哪怕深度为 0）就把整个函数标成 overflow：字段表满不该升级为整表作废 | $ cd /tmp/dfa && cat fields5.extc # struct big { a..e: ?ref i32 }，f() 里 var h: big = { a:… | dataflow |
| `src/dataflow.c:403` | UNSOUND | DFA_ROUNDS=64 不是保险丝：70 步直线循环体就打满，打满即整函数结果作废 | $ cd /tmp/dfa # chain70.extc：while n>0 { v0=v1; v1=v2; ...; v68=v69; v69 = new i32; n = n… | dataflow |
| `src/plate.c:39` | BUG | `extc_memCopy` 用 `memcpy` 做板的整块拷贝：`copyIn/copyOut` 允许源与目标都是板内视图 ⇒ 重叠时 -O2 静默拷错 | cat > /tmp/rev/misc/ovl.extc <<'EOF' use std::io use std::heap fn main() -> i32 { var pl:… | misc |
| `src/pools.c:118` | BUG | `extc_dynPoolOf` 只记 pid、用 `generation == 0` 判陈旧 ⇒ 一个 place 会把 dyn 值生进**别的 place 还活着的对象表**里（地方寿命失效） | 见 /tmp/rev/misc/alias2.extc（f 帧里先建对象表再建容器 → f 退出；main 建自己的 dyn 表；再调 g）： cd /tmp/rev/misc … | misc |
| `src/main.c:132` | HYGIENE | A compiled program killed by a signal makes `--run` exit 255 with no message at all | cat > /tmp/t_crash.extc <<'EOF' fn main() -> i32 { var p: ?ref i32 = null return *p! } EO… | main |
| `src/parser.c:606` | BUG | Top-level annotations that do not apply to the declaration that follows are silently dropped (except on `let`… | cat > /tmp/t_ann.extc <<'EOF' @inline struct box { x: i32 } @inline type e = \| a \| b @bui… | parser-1 |
| `src/parser.c:708` | FALSE-REJECT | 全局声明（let/var）上的 `@noCopy` / `@poolObject` / `@sharesStorage` / `@inline` 被静默接受且完全不做任何事 | printf '@noCopy\nvar g: i32 = 0\nfn main() -> i32 { return 0 }\n' > g.extc; ./build/extc … | parser-1 |
| `src/parser.c:1096` | BUG | parseTrait throws away the method's own type-parameter list (`fn f<T>` in a trait), so the signature later fa… | cat > /tmp/t_traitgen.extc <<'EOF' trait C { fn f<T>(self: ref Self, v: T) -> i64; } stru… | parser-1 |
| `src/parser.c:1475` | FALSE-REJECT | `p->sawBuiltin` is parser-global, not per-declaration: a second `@builtin` in a struct/impl body is falsely r… | cat > /tmp/t_bi3.extc <<'EOF' struct s { x: i32 @builtin fn m1(self: ref s) -> i32 { retu… | parser-2 |
| `src/parser.c:1575` | BUG | Function type parameters are not checked to start uppercase, so `fn f<point>(p: point)` silently shadows the … | cat > /tmp/t_shadow.extc <<'EOF' struct point { x: i32 } fn f<point>(p: point) -> i32 { r… | parser-2 |
| `src/parser.c:1996` | FALSE-REJECT | `at()` only refuses string-vs-punctuation matches, so a statement-position string literal that spells a keywo… | printf 'fn main() -> i32 {\n "abc"\n return 0\n}\n' > /tmp/t_ok.extc # exit 0 printf 'fn … | parser-2 |
| `src/parser.c:3143` | MISCOMPILE | A raw NUL byte inside a string literal is silently dropped: the parser keeps only `t->text` and codegen re-de… | printf 'fn main() -> i32 {\n println("a\000b")\n return 0\n}\n' > /tmp/t_nul.extc /home/a… | parser-3 |
| `src/codegen.c:487` | MISCOMPILE | C keywords are renamed only for functions/methods: a struct/enum type name or a global variable named after a… | files /tmp/rev/work/m_kw1.extc (`struct double { x: i32 }` + `var d: double = { x: 1 }`),… | codegen-1 |
| `src/codegen.c:2077` | MISCOMPILE | genGlobalInit makes only the outermost struct literal a brace initializer: a nested literal stays a compound … | file /tmp/rev/work/t1.extc: struct point { x: i32 y: i32 } struct holder { p: point n: i3… | codegen-1 |
| `src/codegen.c:2203` | MISCOMPILE | Negative-zero float literals are emitted as the integer `-0`, losing the sign: `1.0 / -0.0` prints `inf` inst… | /tmp/rev/t_negzero.extc: `use std::io` / `fn main() -> i32 { var z: f64 = -0.0 var w: f64… | codegen-2 |
| `src/codegen.c:2605` | MISSING_TRAP | f64 -> i64 conversion accepts exactly 2^63 (`v <= (double)INT64_MAX`) and silently returns INT64_MIN | $ cd /tmp/rev/diff/findings $ cat F4_f2i64.extc fn main() -> i32 { var v: f64 = 922337203… | differential |
| `src/codegen.c:3632` | HANG | blkMaxOfBlock evaluates blkMaxLevel twice per statement, making codegen exponential (2^nesting-depth) in the … | Generate nested blocks: `python3 -c "n=32; inner='\n'.join(['{']*n + [' x = x + 1.0'] + [… | codegen-2 |
| `src/codegen.c:3632` | HANG | Exponential compile time in block-depth computation: blkMaxOfBlock evaluates blkMaxLevel twice per statement,… | for n in 24 26 28 30 32: generate `fn f() -> i64 { var x: i64 = 0` + n times `{ x = x + 1… | codegen-3 |
| `src/codegen.c:3778` | MISCOMPILE | The receiver of a domain block is generated once per `ext` task and again for the run, so an impure receiver … | /tmp/rev/t_domrecv3.extc: a global `calls: i64`, `struct holder { d: domain }` with `fn g… | codegen-2 |
| `src/codegen.c:6698` | MISCOMPILE | The "the compiler needs this name" guard skips array and enum instances, so a user declaration with that name… | cat > /tmp/t9.extc <<'EOF' struct array_2_i64 { x: i64 } fn main() -> i32 { var a: [2]i64… | codegen-4 |
| `src/codegen.c:8342` | UNSOUND | The parallel region is sized and initialized with the uncapped thread count; counts >= 2^60 wrap the size com… | cat > /tmp/t12.extc <<'EOF' use std::parallel fn w(id: i64, lo: i64, hi: i64, out: mut sl… | codegen-4 |
| `src/coroutine.c:83` | BUG | extc_task_end 用 extc_arena_release 回收任务 arena，而任务槽永不复用 ⇒ 每个跑完的装箱协程永久泄漏一块（实测 30 万任务 +32MB） | /tmp/extcw/leakbox.extc：`var h: coroutine<i64> = w(k)` + `while h.next() {}` 循环 30 万次。 gc… | coro |
| `src/coroutine.c:137` | BUG | extc_epoll_wait 的待处理队列是函数级 static，不按 epoll 实例区分 ⇒ 跨实例投递别人的 tag | /tmp/extcw/epcross.extc：三对 socketpair 全注册进 epoll A（tag 101/102/103）并各写 1 字节，epoll B 什么都不注… | coro |
| `src/coroutine.c:203` | BUG | extc_unix_connect / extc_unix_listen / extc_tcp_listen(_shared) 的失败路径不关 fd ⇒ 重试循环每次漏一个 fd | /tmp/extcw/connfail2.extc：同一抽象名字连 50 次（全失败），末尾再调一次 extc_sock_read 把事件层拉进产物。 strace -f -e … | coro |
| `src/coroutine.c:403` | BUG | extc_http_date 用固定的 64 字节上限写调用方的缓冲区，而 extC 声明的 `out: ref u8` 不带长度 ⇒ 越界写 | /tmp/extcw/httpdate3.extc：`var buf: mut slice<u8> = new u8[64]`，取 `var small: mut slice<u… | coro |
| `src/modules.c:820` | BUG | 限定类型名的诊断用 ctxError(ctx, 0, ...) 报，渲染成 `file: error:`：整条类型改写路径的错误都没有行号 | /tmp/rev/modrev/u1_line0：lib.extc = `@private struct priv { x: i32 }`，main.extc 第 3 行 `va… | modules-1 |
| `src/modules.c:1557` | FALSE-REJECT | openLookup 遇到「某个被打开模块里是 @private 的同名声明」就报错返回，压掉了另一个模块公开导出的同名符号（误拒合法程序） | /tmp/rev/modrev/u2_open：a.extc=`@private fn helper() -> i32 { return 1 }`，b.extc=`fn help… | modules-2 |
| `src/modules.c:1681` | FALSE-REJECT | impl 块只改写目标类型名、不改写 trait 名：`impl lib::Codec for mine` 报 unknown trait | /tmp/rev/modrev/u2_trait：lib.extc=`trait Codec { fn enc(self: ref Self) -> i32 }`，main.ex… | modules-2 |
| `src/modules.c:1769` | UNSOUND | isStdlibFile 用裸前缀比较：路径字符串以 stdDir 开头的任意目录都被当成标准库，@builtin / 特权模块闸门被绕过 | mkdir -p /home/alphayang/extC_Compiler/build/../stdlib-x && printf '@builtin fn extc_view… | modules-2 |
| `src/modules.c:1865` | BUG | 短名冲突检查只看单个 unit 的 use 列表：跨文件的 `x::util` / `y::util` 漏检，两个模块共用 `util$` 前缀后互相绑错类型，报错却指向无辜代码和根文件行 | /tmp/rev/modrev/u2_samename（x/util.extc 与 y/util.extc 各声明一个同名 struct `pair`，字段不同；alpha/be… | modules-2 |
| `src/check_top.c:901` | HANG | `funcReachesItself` has no memo and explores every call path up to depth 64: exponential compile time for an … | python3 - <<'EOF' N=36; src="" for i in range(1,N+1): for tag in ("l","r"): pre="@inline … | checktop-1 |
| `src/check_top.c:3091` | HANG | The level solver's step budget only prints a message: it never stops the walk, and the message claims it was … | Read src/check_top.c:3082-3106 and grep for `loud`/`steps`: `grep -rn 'loud\\|ls->steps' s… | checktop-2 |
| `src/check_top.c:4279` | UNSOUND | overflow 后调用方并不退回保守答案：整个函数的不动点被丢弃，回落到同一文件自称 unsound 的破坏性遍历 | 实测（EXTC_DBG_DFA=1 会打印命中）： $ cd /tmp/dfa && cat fields5.extc # struct big{a..e: ?ref i32}，… | dataflow |
| `src/check_top.c:6769` | UNSOUND | 跨 yield 的局部量规则（rule 2）对泛型实例失效：typeContainsRef 问的是未替换的 T | cat > /tmp/c3.extc # 具体类型：被拒 type 略 —— 见下： fn w(v: slice<u8>) -> coroutine<slice<u8>> { v… | checktop-4 |
| `src/types.c:275` | FALSE-REJECT | A module's own declaration is unreachable by its bare name when the prelude declares the same name: the plain… | cd /tmp/tt # lib4.extc: struct unit { n: i32 } fn mk() -> unit { return unit { } } # m4.e… | types |
| `src/types.c:379` | BUG | `mut` is silently discarded for every non-generic name (`mut i32`, `mut point`) and for generic enum instance… | cd /tmp/tt # t_mut1.extc: fn f(x: mut i32) -> i32 /home/alphayang/extC_Compiler/build/ext… | types |
| `src/check.c:162` | BUG | cNameFor 不登记已发出的 C 名 ⇒ 形如 `a__2` 的源码名与第二个 `a` 撞成同一个 C 标识符，生成物重定义编不过 | cat > /tmp/n1.extc <<'EOF' fn main() -> i32 { let a = 1 let a = 2 let a__2 = 3 return a +… | check |
| `src/check_expr.c:100` | UNSOUND | worker 白名单不识别 EX_GENCALL ⇒ 池原语（poolSlice/poolResize/poolGive）在 worker 里不经任何检查 | printf '%s\n' 'use std::parallel' 'use std::sys::pool::*' 'fn w(id: i64, lo: i64, hi: i64… | checkexpr-1 |
| `src/check_expr.c:586` | UNSOUND | poolResize/poolGive 不要求视图是 mut ⇒ 只读 `slice<T>` 被洗成可写 `mut slice<T>` | printf '%s\n' 'use std::sys::pool::*' 'fn main() -> i32 {' ' let rid: i64 = extc_pool_new… | checkexpr-1 |
| `src/check_expr.c:1975` | UNSOUND | `@private` is enforced for fields, methods and operators but not for associated functions: `mod::Type::secret… | mkdir -p /tmp/ce2/priv && cat > /tmp/ce2/priv/lib.extc <<'EOF' struct widget { n: i64 @pr… | checkexpr-2 |
| `src/check_expr.c:2989` | BUG | `domain::single(...)`: a @builtin with no body skips argument checking, so extra arguments are silently disca… | cat > /tmp/ce2/domside.extc <<'EOF' use std::sys::domain fn touch() -> i64 { return 1 } f… | checkexpr-2 |
| `src/check_lookup.c:63` | FALSE-REJECT | `unNarrow` 直接截断整个证明栈 ⇒ 与该绑定无关的非空证明被一起丢掉（误拒） | cat > /tmp/lk4.extc <<'EOF' struct node { v: i32 next: ?ref node } fn main() -> i32 { var… | lookup |
| `src/check_lookup.c:457` | FALSE-REJECT | `findOperator` 不查 `Type.mholder` ⇒ 泛型实例/内建标量上定义运算符被判非法（并把内部名写进诊断） | cat > /tmp/lk6.extc <<'EOF' impl slice<u8> { fn ==(self: ref slice<u8>, o: ref slice<u8>)… | lookup |
| `src/check_lookup.c:791` | FALSE-REJECT | `isPlace` 不认 `*p` ⇒ `(*p)[0..2]` 被当成『临时值』拒绝，诊断理由错误 | cat > /tmp/lk7.extc <<'EOF' fn first(p: ref [4]u8) -> u8 { let s = (*p)[0..2] return s[0]… | lookup |
| `src/check_stmt.c:606` | FALSE-REJECT | 被收窄的**字段**作为赋值目标时沿用收窄后的非空类型 ⇒ 合法的可空写入被拒 | cat > /tmp/lk5.extc <<'EOF' struct node { v: i32 next: ?ref node } struct holder { next: … | lookup |
| `src/check_escape.c:144` | FALSE-REJECT | A store into a field/element of a by-value aggregate parameter is judged against depth 0 instead of the frame… | a2b.extc in /tmp/rev/repro holds both functions: `f` (bare `slice<u8>` parameter, store t… | escape-1 |
| `src/check_escape.c:1497` | BUG | 延迟泛型检查的记录端以 owner==NULL 提前返回 ⇒ 自由泛型函数的零值检查永不执行（生成 __extc_reference_has_no_zero_value__） | cat > /tmp/z4.extc <<'EOF' fn mk<T>() -> T { var local: T return local } fn main() { var … | escape-2 |
| `src/check_top.c:4419` | FALSE-REJECT | A global whose annotated type is a nullable reference cannot be initialized with `null`: the annotation is no… | h2.extc in /tmp/rev/repro: `var B: ?ref i32 = null` -> `./build/extc h2.extc -o h2.c` rep… | escape-1 |

---

## 5. P3：卫生问题（42 条）

> 这些本身不产生错误，但每一条都在"掩盖下一个 bug"：死代码、写而不读的字段、
> 两份已经分叉的实现、注释与实现不符。它们让上面那些 P0/P1 更难被发现，也让修复更容易漏。

| 位置 | 类型 | 摘要 | 复现要点 | 来源 |
|---|---|---|---|---|
| `src/ast.c:160` | HYGIENE | `moduleInit` 漏初始化 `Module.aliases`：结构体里九个 Vec 只 init 了八个，唯一的写入点靠 `if (!out->aliases.arena)` 兜底 | 静态核对：`grep -n 'vecInit(&m->' src/ast.c` 得 8 处，`grep -n 'Vec' src/ast.h` 的 Module 得 9 个 Ve… | ast |
| `src/ast.h:358` | HYGIENE | EX_ASSOC→EX_IDENT 原地改写后 `u.ident.srcName` 别名到 `u.assoc.targs.arena`：该字段是“用户写下的原样名字”，实际却持有一个 arena 指针 | cd /tmp/rev2 cat > liba.extc: `type tone = \| low \| high`；usemod.extc: `use liba` + `var t… | ast |
| `src/base.h:12` | DOC | base.h 写下的不变量「没有可变全局状态」与 base.c 的 dbgOn 缓存相反 | 读代码即可核实（无运行期复现）：src/base.h:12 与 src/base.c:18-26 直接矛盾；`grep -rn 'static struct' src/base.… | misc |
| `src/check_internal.h:274` | DOC | `explicitTargs` 注释给出的理由（“union 对某些 kind 不清零 ⇒ 只由一条路径写的字段会读到垃圾”）与代码不符：`exprNew` 是唯一分配点且整节点清零；真正的风险是在原地改 kind 时… | cd /home/alphayang/extC_Compiler grep -rn 'sizeof(Expr)' src/ # 仅 src/ast.c:124 sed -n '1… | internal |
| `src/check_internal.h:415` | HYGIENE | `Checker.fxAltScan` 声明后从未被写、也从未被读；`[fx]` 转储把 `fxAliasWalks` 打印在 `altScan=` 标签下 —— 想看的那个“每处赋值扫描”计数永远是 0，看到的是另一… | cd /home/alphayang/extC_Compiler grep -rn 'fxAltScan' src/ # 仅 check_internal.h:415 一处（写 … | internal |
| `src/dataflow.c:316` | HYGIENE | dfStmt/dfBlock 的 depth 参数从不参与任何比较：只有形状像深度闸门，缺的正是那道闸门 | $ cd /tmp/dfa && python3 -c "n=12;print('fn f() -> i32 {');print('if 1 == 1 {\n'*n,end=''… | dataflow |
| `src/dataflow.c:494` | HYGIENE | dfValueDepth 没有任何调用者（头文件却称它用于修字段表），且 overflow 时返回 0——恰好是最不保守的答案 | $ cd /home/alphayang/extC_Compiler && grep -rn "dfValueDepth" src/ tools/ tests/ \| grep -… | dataflow |
| `src/domain.c:55` | HYGIENE | `extc_dom_free` 没有任何调用者：每个 `domain::single()` 泄漏堆对象（+ 任务表），而这段死代码正好把泄漏盖住 | cat > /tmp/rev/misc/domleak.extc <<'EOF' use std::sys::domain fn main() -> i32 { var k: i… | misc |
| `src/lexer.c:235` | MISCOMPILE | Float literals that overflow or underflow are accepted silently (`1e999` becomes infinity), while the integer… | printf 'fn main() -> i32 {\n var x: f64 = 1e999\n return 0\n}\n' > /tmp/t_f4.extc /home/a… | lexer |
| `src/lexer.c:427` | HYGIENE | lexChar moves `lx->pos` by hand and never advances line/col, so every diagnostic after a character literal on… | printf "fn main() -> i32 {\n var x = 'a' + )\n return 0\n}\n" > /tmp/t_col.extc /home/alp… | lexer |
| `src/main.c:355` | DOC | `--help` (documented as listing every accepted switch) omits `--explain-memory`, which the driver accepts | /home/alphayang/extC_Compiler/build/extc --help 2>&1 \| grep -c 'explain-memory' # -> 0 /h… | main |
| `src/main.c:576` | HYGIENE | `-o FILE` is silently ignored when `--run` is also given: the requested output file is never written and no e… | cd /tmp/extc-t && rm -f /tmp/should_be_written.c timeout 60 /home/alphayang/extC_Compiler… | main |
| `src/main.c:632` | HYGIENE | $CC is exec'd as one program path, so a conventional `CC="cc -m32"` (or `ccache cc`) cannot be used by --run … | cd /tmp/extc-t && CC="cc -m32" /home/alphayang/extC_Compiler/build/extc --run t_hyg.extc;… | main |
| `src/parser.c:48` | UB | `pk()` 在令牌向量为空时下标下溢（`p->toks->len - 1` 变成 SIZE_MAX） | 无（不可达）。静态依据：src/lexer.c 的 lexAll 末尾无条件 push TK_EOF。 | parser-1 |
| `src/parser.c:107` | BUG | shown() assumes TK_NEWLINE has empty text, but the lexer stores "\n": diagnostics print a raw newline inside … | printf 'struct s {\n x:\n}\nfn main() -> i32 { return 0 }\n' > /tmp/t_nl.extc ./build/ext… | parser-1 |
| `src/parser.c:396` | HYGIENE | `parseEffectsClause` 末尾有不可达的第二个 `return true;` | sed -n '392,397p' src/parser.c | parser-1 |
| `src/parser.c:515` | BUG | 重复注解的检查不一致：`@inline @inline` 与 `@noCopy @noCopy` 静默通过，`@frozen @frozen` 报错 | printf '@inline @inline\nfn f() -> i32 { return 1 }\nfn main() -> i32 { return f() }\n' >… | parser-1 |
| `src/parser.c:777` | HYGIENE | 顶层 `fn` 分支里的 `// at(&p, "@")` 与随后的 `parseFuncAnnotations` 是不可达死路径 | 读 parser.c:502-605（注解循环）与 777-800（fn 分支）即可判定；`@inline fn f(){}` 走的是 502 行那一遍。 | parser-1 |
| `src/parser.c:1058` | HYGIENE | trait 的类型参数不要求大写，与 struct 的检查不一致 | printf 'trait t<x> { fn f(self: ref x) -> i32 }\nfn main() -> i32 { return 0 }\n' > tt.ex… | parser-1 |
| `src/parser.c:1746` | BUG | Fixed-array length has no upper bound: `[9223372036854775807]i32` type-checks and the generated C is rejected… | printf 'fn main() -> i32 {\n var a: [9223372036854775807]i32\n return 0\n}\n' > /tmp/t_bi… | parser-2 |
| `src/parser.c:2327` | FALSE-REJECT | C-style `for (init; cond; step)` cannot omit the init or the step, although the parser documents the C shape | printf 'fn main() -> i32 {\n var i = 0\n for (; i < 3; i += 1) { }\n return i\n}\n' > /tm… | parser-2 |
| `src/codegen.c:1345` | HYGIENE | The operator-as-method-call path never passes `@overwrite` cells, unlike the three other call emitters (curre… | Not reproducible. Tried: (a) `fn +(self: ref box, v: i64) -> box` with `@overwrite var x … | codegen-1 |
| `src/codegen.c:2167` | HYGIENE | A lambda with no captures emits an empty initializer `(T){}`, which ISO C11 forbids and which leaves the synt… | `./build/extc tests/lambda/no_capture.extc -o /tmp/rev/t_nocap.c` emits `$lam$main$0 f = … | codegen-2 |
| `src/codegen.c:2327` | HYGIENE | The EX_CALL handle-protocol block (next/value/send on a coroutine receiver) is unreachable, and its `send` ar… | No program shape reproduces it. Evidence: `printf 'set pagination off\nbreak codegen.c:23… | codegen-2 |
| `src/codegen.c:6527` | HYGIENE | markUncalledFunctions lacks the dynTable exemption its sibling pruner has, so a method the dyn table calls is… | cat > /tmp/t11.extc <<'EOF' use std::io trait Tag { fn tag(self: ref Self) -> i64 } struc… | codegen-4 |
| `src/coroutine.c:76` | BUG | extc_task_zone 只查上界不查 liveness ⇒ 对从未用过的 id 返回未初始化内存（ASan 下 0xBEBE...），对死任务返回旧 zone | /tmp/extcw/taskzone.extc：一个装箱协程跑完后 `io::cout << syscoro::extc_task_zone(i64(5)) << " " <<… | coro |
| `src/modules.c:12` | DOC | 文档与代码不一致：头部说 use 解析「next to the importing file」，实际只用入口文件目录；findModFile 的注释还写了一个不存在的参数 | 读 src/modules.c:11-13 与 396-401；实测：cd /tmp/rev/mine/m9 之类布局，子目录模块里裸写兄弟模块名会报 cannot find m… | modules-1 |
| `src/modules.c:236` | BUG | suggestImportPath 不查 L->rootDir，而 findModFile 恰恰先查 rootDir：建议的 `use` 要么给不出，要么指向另一个文件 | A（给不出建议）：/tmp/rev/modrev/u1_suggest（greet.extc 与 main.extc 同目录，main 里写 `greet::thing`）→ `… | modules-1 |
| `src/modules.c:960` | HYGIENE | 构造器改写里的 isFn 探测是死代码：拿已 mangle 的声明名去比源名，永远不相等 | 读代码即可核实（mangle 见 1359-1373 行；构造器分支 947-969 行）。实测对照：/tmp/rev/mine/m21 等模块里 `Type(...)` 全部走… | modules-1 |
| `src/modules.c:980` | BUG | `!saveCall` 提前 return：非调用形式的限定名（`greet::K`）丢掉「模块存在但没 import」的诊断，只剩误导性的 unknown type | /tmp/rev/modrev/u1_noncall（greet.extc: `let K: i32 = 9`；main.extc: `fn main() -> i32 { re… | modules-1 |
| `src/check_top.c:267` | UNSOUND | Trait/impl parameter comparison starts at index 1 unconditionally: for a receiver-less method the first real … | cat > /tmp/x/tr6.extc <<'EOF' trait Conv { fn conv(x: i64) -> i64 } struct box { v: i64 }… | checktop-1 |
| `src/check_top.c:2768` | HYGIENE | Checker.escapeesFor is written but never read: the documented cross-call memo does not exist and the key is a… | grep -rn 'escapeesFor' src/ -> only src/check_internal.h:432 (declaration) and src/check_… | checktop-2 |
| `src/check_top.c:3544` | PERF | Level pass cost is quadratic in the number of store records of one function | Files /tmp/rev/findings/big1000.extc, big2000.extc, big4000.extc (generator: `var vI: ?re… | checktop-2 |
| `src/check_top.c:3942` | HYGIENE | HYGIENE: two copies of the coroutine handle-protocol synthesis disagree on the receiver mutability of `send` … | Static: compare check_top.c:3942 with check_top.c:5207 (and check_top.c:3931 vs 5195-5199… | checktop-3 |
| `src/check_top.c:5118` | HYGIENE | HYGIENE: allFunctions lists every `impl` method twice (already attached to StructDef.methods), so all four al… | struct counter { i: i64 n: i64 } + 'impl counter { fn zzNext(self: mut ref counter) -> bo… | checktop-3 |
| `src/check_top.c:6048` | PERF | 电平事实重放的收敛判据用错了返回值（promoteInto 返回“成功”而非“有变化”）⇒ 两处不动点循环恒定跑满 32 轮 | EXTC_DUMP_LVL=1 ./build/extc examples/escape-promotion.extc -o /dev/null 2>&1 \| grep 'con… | checktop-4 |
| `src/types.c:264` | BUG | A type parameter written with type arguments (`T<i32>`) silently resolves to `T`: the arguments are dropped i… | cd /tmp/tt # t_targ2.extc: fn f<T>(x: T<i32>) -> i32 { return 0 } + f<i32>(y) /home/alpha… | types |
| `src/types.c:301` | DOC | The unknown-type diagnostic advertises `str` as a built-in type, but no such type exists | cd /tmp/tt # t_str.extc: fn main() -> i32 { var s: str = 0 return 0 } /home/alphayang/ext… | types |
| `src/check.c:466` | HYGIENE | checkAssignable 不判 want/got 为空就解引用（cppcheck check.c:466 的 want-may-be-NULL 判定：静态成立、当前调用方不可达，但缺护栏） | 无可复现输入（这也是判定为 P3 而不是 CRASH 的原因）。静态验证： grep -n 'ttIsError(Type' -A2 src/types.c # NULL -> … | check |
| `src/check_expr.c:2013` | HYGIENE | EX_ASSOC's `parallel::run` branch reads the wrong union member (`e->u.call.args` on an EX_ASSOC node); it is … | Reachability was measured, not assumed: a temporary `fprintf` at the head of `case EX_ASS… | checkexpr-2 |
| `src/check_expr.c:3527` | HYGIENE | `parallel::run` 的整段内建处理被复制两份（EX_CALL 与 EX_METHOD），修一处不改另一处 | grep -n 'isParRunDecl' src/check_expr.c # 3020 / 3520 两处 sed -n '3020,3111p' src/check_ex… | checkexpr-3 |
| `src/check_lookup.c:550` | HYGIENE | 死代码：`typeContainsProto` 已无活调用方（三处 `0 &&` 关闭）、`checkFnDisplay` 无任何调用方 | grep -rn "typeContainsProto" src/ → 只有 check_lookup.c 的定义 + check_expr.c 三处 `0 &&`； grep … | lookup |

---

## 6. 性能与可扩展性（编译器自身）

这一节单独成章，因为这些问题有一个共同形状：**递归 walker 没有预算，输入稍大就从"慢"变成"挂住"或"栈溢出"**。
它们不是"优化空间"，而是可用性缺陷（一个源文件就能让编译器不返回）。

| # | 位置 | 现象（实测） | 机制 |
|---|---|---|---|
| 6.1 | `src/codegen.c:3628-3635` | 嵌套块 n=20 → 0.10 s；n=26 → 0.41 s；n=30 → **5.5 s**；n=40 → **>60 s 不返回**（n=32 时 36.9 s，n=80 永不结束） | `blkMaxOfBlock` 对每个子语句调用 `blkMaxLevel` **两次**（`:3632` 与 `:3633`），深度 d 的嵌套块成本 ~2^d。callgrind（n=24）：`blkMaxOfBlock` 53.4% + `blkMaxLevel` 36.7% 指令 |
| 6.2 | `src/dataflow.c:316-322` | 约 **200 层嵌套块** → SIGSEGV（栈溢出）；`ulimit -s 512` 时 12 层、`-s 1024` 时 24 层即崩 | `dfStmt`/`dfBlock` 互递归且每帧持有数份 `Facts`；`-fstack-usage` 实测 `dfStmt` 帧 **41152 B**（`DFA_MAX_VARS=256` × 80 B × 多个副本），`depth` 参数一路传递却从不用于限深 |
| 6.3 | parser 递归下降 | `(`×8000 正常；`(`×**16000** → SIGSEGV；`[`×20000 同 | `parseUnary/parseFactor/parseTerm/parseExpr` 无深度上限（gdb 栈可证） |
| 6.4 | `src/check_top.c:901` | `funcReachesItself` 无 memo：`@inline` 函数在扇出 DAG 上 24 层 0.1 s、30 层 2.2 s、34 层 **36.3 s**、36 层 >60 s | 每条调用路径都走一遍（深度上限 64 但没有 visited 集），成本 ~2^层数 |
| 6.5 | 类型解析（嵌套数组） | `[1][1]…i32`（n 维）n=1000 → 2.0 s，n=2000 → **19.8 s**（平方级），最终报错退出 | 类型构造/字符串化随嵌套深度平方增长（在错误路径上，体验仍是"卡住"） |
| 6.6 | `src/check_top.c:3544` | 单函数 1000/2000/4000 条 store 记录 → 0.08/0.28/1.03 s（每翻倍 ~3.7×） | level pass 在 64 轮不动点里对每条记录重扫整个 store 切片 |
| 6.7 | `src/check_top.c:5596` 一线 | 未量化 | 延迟调用/实例批处理是单趟（见 P0-5），修成不动点时要注意加轮数上限 |
| 6.8 | `src/modules.c:2011`、`:1359-1373` | 未做耗时归因（线索） | aliases 去重 O(n²)、mangle 第二遍 O(ren × 声明数) |
| 6.9 | `stdlib/stl/hashSet.extc`（**只有 52 行**）—— **根因已定案，见 §2 P0-22** | 编译器单文件编译烧掉 **>15 分钟 CPU**（三次独立运行：`--check-c` clang 路径 >120 s 超时、生成物告警扫描 >90 s 超时、我自己的 400 s 运行被手动终止）。同样规模的 stdlib 文件是毫秒级 | 已定位到 extc 自身（不是 clang），并进一步定位到 `codegen.c:5798` 的 `continue` 死循环（§2 P0-22）。仍属 §6 的“扫描循环没有推进保证”一族 |

**建议**：
1. 给所有递归 walker 一个**统一的深度/步数预算**（parser、dataflow、`blkMax*`、`funcReachesItself`、level pass），
   超限报"程序嵌套过深/编译资源耗尽"的**诊断**，而不是崩溃或静默挂住。
2. `blkMax*` 去掉重复调用并加记忆化（每个块算一次，O(n)）。
3. `dataflow` 改成显式栈或把 `Facts` 移到堆上；`Facts` 的 256×80 B 结构按值复制是栈溢出的直接原因。
4. 把 6.1/6.2/6.3 的最小输入写进回归（当前仓里没有任何"规模/深度"用例）。

---

## 7. 为什么 311 个用例全绿还会漏掉这些

把这次发现的缺陷映射回测试集，盲区有四类：

1. **组合面没覆盖。** 每条机制单独有测试，但"机制 × 新语法"没有：
   `?` 与逃逸集合（P0-1）、泛型与协程（P1-13）、变体枚举与泛型（P1-19）、
   `for` 与 `continue`（P0-15）、`?` 与枚举载荷（P0-19）、字面量与浮点（P0-12）。
   仓库里唯一用 `continue` 的用例恰好是 `while` 循环——`for`+`continue` 这个最常见的组合一次都没跑过。
2. **只有"正常路径"的断言。** 断言写的是"能编译/能跑出期望输出"，很少断言
   **"生成的 C 必须能编译"** 与 **"生成物在 sanitizer 下干净"**：
   - `--run` 默认 `-O2`，会把 NULL `memmove`（P0-8）这类 UB 直接"优化掉"，测试反而变绿；
   - 全语料 `--check-c` 不在 `check.sh` 里，于是 13 条"报成功但产物编不过"的问题无人发现；
   - `tests/asan/` 只有 9 个形状，覆盖不到逃逸/协程/域这些新机制。
3. **没有差分与随机化。** 本次一次 900 例的 extC-vs-C 差分 fuzz 就抓到 P0-12（浮点字面量）
   与 P0-18（窄类型算术）；变异 fuzz 1858 例则证明"普通畸形输入不崩"，
   真正崩溃的是**结构性**输入（深嵌套），需要生成式而非变异式。
4. **自检开关只检查作者已知的不变量。** `EXTC_SELFCHECK`/`EXTC_DBG_ARENA` 在 tests/examples 上全绿，
   但它们检查的是"checker 与 codegen 对 arena 层级的看法一致"，对"两边对**类型名/帧名**的看法一致"
   （P0-9、P1-8、P1-11）没有任何覆盖。

**建议新增的四道闸门**（都能直接放进 `check.sh`）：
- ① 全语料 `--check-c`（gcc 与 clang 各一次）：任何"extc 报成功、C 编不过"直接红；
- ② 正例语料生成物用 `-O0 -fsanitize=address,undefined` 跑一遍：把 UB 从"被优化掉"变成"必须红"；
- ③ 常驻小规模差分 fuzz（extC vs C，随机算术/转换/切片程序各 N 例）；
- ④ 规模闸门：嵌套深度/语句数/泛型实例数各设一个上限用例，断言"要么快，要么给出诊断"。

---

## 8. 修复顺序建议

按"风险 × 影响面"排序，建议分六批（每批都能独立验证）：

| 批次 | 内容 | 理由 |
|---|---|---|
| **第 1 批：静默错值（最危险）** | P0-12（浮点字面量）、P0-15（`for`+`continue`）、P0-18（窄算术）、P0-19（`?` 载荷）、P0-7（`==` 临时量）、P0-9（重载后缀） | 都产生**错误结果且无任何诊断**；修法局部、可加针对性用例 |
| **第 2 批：编译器不能崩** | P0-4（NULL 实例）、P0-14（零实参）、P0-10（深度预算：parser/dataflow/`blkMax`）、P0-13（可空下标）、P0-17（非空证明失效） | 崩溃与误判；深度预算是 6.1/6.2/6.3 的统一修法 |
| **第 3 批：健全性（逃逸/生命周期）** | P0-1（`EX_TRY`）、P0-2（深度哨兵）、P0-16（出参 publication）、P0-6 四条（载体穿透/`placeDepth`/`needsHome`/名字匹配）、P1-4（32 位掩码）、P1-5（`fresh` 失效） | 这一批是"语言核心承诺"的实现；建议同时引入 §0.2-1 的**不动点**与**符号化来源链**，避免继续逐点打补丁 |
| **第 4 批：生成物必须能编译** | P0-11（域层级）、P0-8（NULL 保护）、P1-6..P1-18（trait/协程/`@overwrite`/字符串转义/前缀冲刷） | 每一条都对应"extc 退出码 0、C 编不过"；配合闸门 ① 可防回归 |
| **第 5 批：一致性守卫** | 把 checker/codegen 的重复表合并（`blkMaxLevel` vs `genBlockBody`、`countOwSites` vs `collectOwSites`、帧名、句柄协议两份、`parallel::run` 两份、`unifyTParams`）；每个"两边必须一致"处加断言或共享 walker | 这一批不修具体 bug，但把"下一个 bug"的概率降下来 |
| **第 6 批：性能与卫生** | §6 的九项 + P2/P3 清单 | 影响开发体验与长期可维护性 |

> 回归资产：本次每条的复现程序都应转成 `tests/` 用例（P0 全部、P1 至少一半）。
> 建议目录：`tests/audit-2026-09/`（正例）+ `tests/errors/`（应拒绝的形状），
> 并在 `check.sh` 挂上四道闸门。

---

## 9. 覆盖清单（审阅了什么）

审阅方式：`src/` 每个文件切成若干区间，每区间一个独立审阅单元**逐行**读完（不是抽样），
再由我复核 P0 与高影响 P1。下表是单元与产出的对应关系。

| 文件 | 区间 | 单元 | 条数 | 最高级别 |
|---|---|---|---|---|
| `src/codegen.c` | 1-2150 | codegen-1 | 7 | P0 |
| `src/codegen.c` | 2151-4300 | codegen-2 | 8 | P0 |
| `src/codegen.c` | 4301-6450 | codegen-3 | 8 | P1 |
| `src/codegen.c` | 6451-8578 | codegen-4 | 8 | P1 |
| `src/check_top.c` | 1-1720 | checktop-1 | 6 | P0 |
| `src/check_top.c` | 1721-3440 | checktop-2 | 6 | P0 |
| `src/check_top.c` | 3441-5160 | checktop-3 | 6 | P0 |
| `src/check_top.c` | 5161-6885 | checktop-4 | 3 | P1 |
| `src/check_expr.c` | 1-1420 | checkexpr-1 | 8 | P1 |
| `src/check_expr.c` | 1421-2840 | checkexpr-2 | 5 | P0 |
| `src/check_expr.c` | 2841-4268 | checkexpr-3 | 5 | P0 |
| `src/check_escape.c` | 1-1290 | escape-1 | 6 | P0 |
| `src/check_escape.c` | 1291-2571 | escape-2 | 3 | P0 |
| `src/parser.c` | 1-1220 | parser-1（Lead 亲读 + 独立单元复核） | 10 | P1 |
| `src/codegen.c` 剪枝阶段（5798 附近） | Lead 亲测 | hang-hashset（Lead 定案） | 1 | P0 |
| `src/parser.c` | 1221-2440 | parser-2 | 7 | P0 |
| `src/parser.c` | 2441-3649 | parser-3 | 2 | P0 |
| `src/check_stmt.c` | 全文 | stmt | 1 | P1 |
| `src/check_lookup.c` | 全文 | lookup | 8 | P1 |
| `src/check.c` + `check.h` | 全文 | check | 2 | P2 |
| `src/types.c` + `types.h` | 全文 | types | 6 | P1 |
| `src/modules.c` | 1-1050 | modules-1 | 6 | P1 |
| `src/modules.c` | 1051-2093 | modules-2 | 6 | P1 |
| `src/dataflow.c` + `.h` | 全文 | dataflow | 5 | P2 |
| `src/coroutine.c` + `.h` | 全文 | coro | 9 | P0 |
| `src/lexer.c` + `lexer.h` | 全文 | lexer | 2 | P3 |
| `src/main.c` | 全文 | main | 4 | P2 |
| `src/ast.c` + `ast.h` | 全文 | ast | 5 | P1 |
| `src/check_internal.h` | 全文 | internal | 3 | P1 |
| `src/base.c` + `base.h` | 全文 | Lead 亲读 | 2（记在 §10） | P3 |
| `pools/plate/domain/time/memfind/prelude` | 全文 | misc | 7 | P1 |
| 生成物告警扫描（gcc/clang 全语料，594 份生成物×4 路） | — | genwarn | 5 | P1 |
| 差分语义（extC vs C，900 例 fuzz + 定向） | — | differential | 5 | P0 |

**各单元在 `coverage_notes` 里明确记为"已核实正确"的点**（节选，完整见各 JSON）：
`extc_checkedIndex`/`extc_checkedRange` 的边界与切片一致、除零/移位/越界 trap 的正确性、
i32/i64/u32/u64 的回绕、负数除模、浮点打印/NaN、结构体值语义与大结构体拷贝、
切片别名与多维边界、`@frozen` 的 `_Static_assert`、dyn 派发表与 thunk、`??` 的求值次数、
`gcc -fanalyzer` 全 `src/` 0 告警、仓库自带 `EXTC_SELFCHECK`/`EXTC_DBG_ARENA` 在 tests/examples 上 0 报警。

---

## 10. 未决项与不确定项

1. **覆盖完整性**：`src/` 全部文件、全部区间都有单元逐行读过（§9），包括我亲读的 `src/base.c` 与 `src/parser.c` 1-1220（后者又由一个独立单元复核并合并，共 10 条）。没有未覆盖的文件。
2. **我未逐条复核全部 173 条**：全部 22 条 P0 与约 25 条 P1 由我在本机独立重跑；
   其余条目是报告者复现（JSON 里有命令与输出），级别为报告者自评。引用时请以"报告者复现"看待。
3. **base.c 亲读记录**（低危）：
   - `bufReserve`（`base.c:224-235`）的 `while (ncap < b->len + extra) ncap *= 2;` 在 `extra` 为
     `SIZE_MAX` 级（`bufPrintf` 收到 `vsnprintf` 返回负值时 `(size_t)n` 变成巨大数）会**回绕成 0 并死循环**；
     正常语料不可达，但属于"没有上界的循环"。
   - `dbgOn`（`base.c:18-26`）用可变的 static 缓存，与 `base.h:12` 自称的"无可变全局状态"相抵触（无功能危害）。
4. **报告者的"无法确认"项**（已随各 JSON 的 `coverage_notes` 保留，摘要）：
   - `computeEffectsTransitive` 的位位置合并（`check_top.c:1162-1167`）在实参顺序不同时可能错，未构造出复现；
   - `checkCallRefArgs` 的 `h = scopes.len` 兜底（`check_top.c:1273`）过于宽松，未构造出复现；
   - `unifyTParams` 对 `mut ref T` 形参直接失败导致一族自由泛型函数不可调用（`check_expr.c:3130-3155`）——
     已确认"误拒"，但它是"缺失功能"还是"缺陷"需要语言侧定案；
   - `coroPushLoop/coroPopLoop` 在 >8 层嵌套时把链条折回 8（`check_top.c:6662-6668`），可能漏判，未构造；
   - `ttCrossesC` 在含类型参数的签名上跳过、实例化后不复检（`types.c:417`，注释自认的已知缺口）；
   - `check_expr.c:1940` 一线在审阅期间出现过无条件的 `DBG_ASSOC_ENTRY` stderr 打印
     （差分审阅者遇到时已从源码移除；当前工作树与 HEAD 一致，未计入）。
5. **`stdlib/stl/hashSet.extc`（52 行）让编译器 >15 分钟不返回**：已定案为 P0-22（`codegen.c:5798` 的 `continue` 不推进循环位置，100% CPU 死循环），一行可修；同族扫描循环应一并审计。

---

## 附录 A：审计材料与复现方式

- 各单元完整 finding JSON（含逐字引用、触发条件、复现命令）：**全部在 `.audit/findings/`**（29 个单元；
  Lead 调度的单元原本写在 `/tmp/rev/findings/`，已一并归档）。机器可读索引：`.audit/findings-index.tsv`。
- 最小复现程序：
  - 逃逸/健全性：`/tmp/rev/repro/`、`/tmp/rev2/`、`/tmp/rev/lookup/`、`/tmp/rev/v3/`、`/tmp/rev/v5/`
  - 浮点/差分：`/tmp/rev/diff/`
  - 深嵌套/性能：`/tmp/rev/atk/`
  - 池/模块/类型：`/tmp/rev/vfy/`、`/tmp/rev/modrev/`、`/tmp/tt/`
- 工具脚本：`/tmp/rev/fuzz.py`（变异 fuzz）、`/tmp/rev/san_sweep.py`（生成物 ASan/UBSan 全量）、
  `/tmp/rev/checkc_sweep.py`（`--check-c` 全语料，gcc/clang）、`/tmp/rev/merge.py`（汇总）。
- 复现环境：`gcc 15.2.0`、`clang 21.1.8`、`valgrind 3.26`、`cppcheck`、Ubuntu 25.10、x86-64。
- 重新生成附录 B 的索引：`python3 /tmp/rev/merge.py --tsv`。

## 附录 B：全部 finding 索引

> 级别为"报告者自评 + Lead 复核"（P0 全部复核；P1 部分复核）。
> 复现列为要点摘录，完整命令见对应单元的 JSON。

| # | 级别 | 位置 | 类型 | 摘要 | 复现 |
|---|---|---|---|---|---|
| 1 | P0 | `src/parser.c:2394` | HANG | `continue` inside `for i in lo..hi` (and C-style `for`) restarts the loop without running the step: infinite loop | cat > /tmp/t_cont1.extc <<'EOF' fn main() -> i32 { var sum = 0 for i in 0..5 { if i == 2 { continue } sum += i } return sum } EOF timeout 5 /home/alp… |
| 2 | P0 | `src/parser.c:2457` | HANG | `continue` inside `for x in xs` skips the index step: the loop never advances (same defect in the C-style form) | cat > /tmp/t_cont2.extc <<'EOF' fn main() -> i32 { var xs = [1, 2, 3, 4] var sum = 0 for x in xs { if x == 2 { continue } sum += x } return sum } EOF… |
| 3 | P0 | `src/codegen.c:630` | MISCOMPILE | opOverloadSuffix derives the overload suffix from the *caller's* substitution context, so a generic method's definition… | file /tmp/rev/work/b_ovl2.extc: struct w<T> { v: T fn ==(self: ref w<T>, o: T) -> bool { return true } fn ==(self: ref w<T>, o: u8) -> bool { return … |
| 4 | P0 | `src/codegen.c:768` | CRASH | genViewCopier (`copyInto`'s primitive) dereferences a NULL storage pointer: it lacks the `!v.data` guard that the index… | file /tmp/rev/work/e_copy3.extc (view_no_storage.extc with `s[0] = i32(42)` replaced by a copyInto): use std::io use std::sys::pool as syspool fn mai… |
| 5 | P0 | `src/codegen.c:1194` | MISCOMPILE | Temporary for an array `==` operand is hoisted out of a `while` condition: the condition's side effect runs once, givin… | file /tmp/rev/work/i_while2.extc: use std::io var g: i32 = 0 fn mk() -> [2]i32 { g = g + 1 return [g, 0] } fn main() -> i32 { var a: [2]i32 = [1, 0] … |
| 6 | P0 | `src/codegen.c:2203` | MISCOMPILE | f64 literals with an integral value are emitted as C integer constants: 1.0/2.0 evaluates to 0, 1e9*1e9 overflows in in… | $ cd /tmp/rev/diff/findings $ cat F2_floatlit.extc fn main() -> i32 { println("1.0/2.0 = ", 1.0 / 2.0) println("1e9*1e9 = ", 1000000000.0 * 100000000… |
| 7 | P0 | `src/codegen.c:3642` | CRASH/UNSOUND | A `d.run { ... }` block is not counted by blkMaxLevel, so the block arena array is one level too short: out-of-bounds `… | Program /tmp/rev/t_dom.extc: `use std::sys::domain` / `use std::io` / `fn main() -> i32 { let d = domain::single() var tot: i64 = 0 d.run { var buf: … |
| 8 | P0 | `src/codegen.c:5798` | HANG | 编译 stdlib/stl/hashSet.extc 永不返回：dropRuntimeDefs 的 `continue` 跳过了循环推进语句（100% CPU 死循环） | cd /home/alphayang/extC_Compiler # 1) 现象：硬超时（20s 足够，实际上 >15 分钟不返回；三次独立运行均如此） timeout -s KILL 20 ./build/extc -w stdlib/stl/hashSet.extc -o /dev/null … |
| 9 | P0 | `src/coroutine.c:84` | UNSOUND | 任务结束时按 LIFO 弹“任务的地方”：把仍然存活的任务 zone 一起弹掉 ⇒ 生成物 heap-use-after-free（ASan） | /tmp/coro/z2.extc：./build/extc -w z2.extc -o z2.c && gcc -std=c11 -O1 -g -fsanitize=address -fwrapv z2.c -o z2 && ./z2 → 'AddressSanitizer: heap-use-… |
| 10 | P0 | `src/check_top.c:2022` | UNSOUND | `call(...)?` (EX_TRY) is invisible to the escape set, so a local published through a call goes into a block arena: gene… | cp /tmp/rev/findings/tryb3.extc /tmp/tryb3.extc; cd /home/alphayang/extC_Compiler; ./build/extc -w /tmp/tryb3.extc -o /tmp/tryb3.c # accepted, exit 0… |
| 11 | P0 | `src/check_top.c:4528` | CRASH | Compiler SIGSEGV: funcInstance returns NULL after the type-depth diagnostic, callers dereference it (deep explicit type… | python3 -c "d='i64'\nfor _ in range(65): d='box<%s>'%d\nopen('/tmp/d65.extc','w').write('struct box<T> { v: T }\nfn z<T>() -> i64 { return i64(0) }\n… |
| 12 | P0 | `src/check_top.c:4865` | UNSOUND | UNSOUND: the deferred zero-value check is never recorded for free generic functions -> silently zero-initializes a `fn`… | (a) /tmp/f3.extc: 'struct cellfn { f: fn(i32) -> i32 }\nfn mk<T>() -> T { var b: T return b }\nfn main() -> i32 { let c: cellfn = mk<cellfn>() return… |
| 13 | P0 | `src/check_top.c:5596` | MISCOMPILE | MISCOMPILE: deferred generic call sites are resolved in a single pass, so a 3-deep generic chain calls the never-emitte… | /tmp/g3.extc: 'fn f<T>(x: T) -> T { return x }\nfn g<T>(x: T) -> T { return f(x) }\nfn m<T>(x: T) -> T { return g(x) }\nfn main() -> i32 { let r: i64… |
| 14 | P0 | `src/check_top.c:6024` | UNSOUND | Arena/zone depth for a call site is computed with 0 as "unset", so a later deeper `mut ref` argument raises it: caller … | cat > /tmp/x/ea2.extc <<'EOF' use std::io struct node { value: i64 next: ?ref node } struct slot_t { v: ?ref node } fn stash(dest: mut ref slot_t, ot… |
| 15 | P0 | `src/check_expr.c:1684` | UNSOUND | EX_INDEX / EX_SLICE never reject a nullable reference: `p[i]` on `?ref [N]T` dereferences null with no diagnostic | cat > /tmp/ce2/nullcrash.extc <<'EOF' fn main() -> i32 { var a: [4]i32 = [1, 2, 3, 4] var p: ?ref [4]i32 = null return p[1] } EOF ./build/extc /tmp/c… |
| 16 | P0 | `src/check_expr.c:3027` | CRASH | `parallel::run()` 一个实参都不给 ⇒ 编译器读空 Vec 段错误（exit 139） | cat > /tmp/p0.extc <<'EOF' use std::parallel fn main() -> i32 { var r: i32 = parallel::run() return 0 } EOF /path/build/extc -w /tmp/p0.extc -o /dev/… |
| 17 | P0 | `src/check_escape.c:649` | UNSOUND | exprBorrowed does not walk several carrier shapes, so rule B is bypassed: a borrow laundered through a local copy / str… | Files in /tmp/rev/repro. (a) b3.extc: `var G: slice<u8> = "hi"` (global); `fn stash(p: slice<u8>) { var q: slice<u8> = p G = q }`; main fills a block… |
| 18 | P0 | `src/check_escape.c:1447` | UNSOUND | needsHome 早退把“有 home arena 的函数”往全局存借用值放行 ⇒ 生成物 heap-use-after-free | cat > /tmp/n6.extc <<'EOF' struct node { v: i32 } var g: slice<u8> = "hi" fn stash(s: slice<u8>) -> ?ref node { var n: ?ref node = new node g = s ret… |
| 19 | P1 | `src/ast.c:267` | BUG | `astWalkStmtChildren` 的 ST_ASSIGN 不访问 `u.assign.opExpr`：复合赋值里的用户运算符调用对**所有**基于 walk 的谓词不可见，产物编不过 | cd /tmp/rev2 /home/alphayang/extC_Compiler/build/extc -w op2.extc -o op2.c # rc=0，无诊断 # op2.extc: struct box { p: mut ref node fn +(self: ref box, o:… |
| 20 | P1 | `src/ast.h:192` | BUG | `Expr.func` 的跨 pass 契约只写了 EX_CALL/EX_METHOD：运算符节点（EX_BIN）也带 `func`，而每个基于 walk 的谓词只认三种 call kind ⇒ `var c = a + b` 的产物编不过 | cd /tmp/rev2 /home/alphayang/extC_Compiler/build/extc -w op1.extc -o op1.c # rc=0，无诊断 # 与上一条同一组类型/全局，只是把 `a += b` 写成 `var c: box = a + b` gcc -fsynta… |
| 21 | P1 | `src/ast.h:824` | UNSOUND | `FuncDef.extThreadMask` 的文档缺省值(1=碰共享状态)没有代码实现：无 `effects` 子句的 extern 被当成 Thread=0，并行 worker 里可静默调用它（TSan 实测 data race） | cd /tmp/rev2（文件已留在此处） # A. 未签字的 extern 被接受 /home/alphayang/extC_Compiler/build/extc -w par1.extc -o par1.c # par1 = use std::parallel + extern!("libc… |
| 22 | P1 | `src/check_internal.h:497` | UNSOUND | 被调者通过 `mut ref`/`ref` 出参写入的存储不产生任何 publication：目的绑定的 `refDepth`/字段深度停在旧值，晋升走不到分配站点 ⇒ 编译器接受、产物 heap-use-after-free | cd /tmp/rev2（文件已留在此处；build/extc md5=017fc7ec39e77b76e989460d76a04fe7） # A. 堆版本：真实 heap-use-after-free /home/alphayang/extC_Compiler/build/extc -w add… |
| 23 | P1 | `src/pools.c:46` | BUG | 池槽位回收时不清 `extc_poolKind`：容器池继承「对象表」模式 ⇒ 合法程序运行期假 trap（abort） | cat > /tmp/rev/misc/kind6.extc <<'EOF' use std::io use stl::vector trait Tag { fn tag(self: ref Self) -> i64 } struct box { v: i64 } impl Tag for box… |
| 24 | P1 | `src/pools.c:51` | BUG | `extc_pool_take/resize` 把非正字节数静默钳成 1：`n * sizeof(T)` 溢出时给出 1 字节板块而切片 `.len` 仍是 n ⇒ 堆越界写 | cat > /tmp/rev/misc/huge1.extc <<'EOF' use std::io use stl::vector fn main() -> i32 { var v = vector<i64>::withCap(i64(1152921504606846976)) v.push(i… |
| 25 | P1 | `src/domain.c:49` | UB | 域运行期保存块内局部帧的裸指针：`d.run { }` 在块结束之后才驱动帧 ⇒ 生成物 stack-use-after-scope | cd /tmp/rev/misc && /home/alphayang/extC_Compiler/build/extc -w /home/alphayang/extC_Compiler/tests/ext/domain_runs.extc -o dom.c # 先绕开已归档的 P0-11（__e… |
| 26 | P1 | `src/parser.c:392` | FALSE-REJECT | `effects` clause rejects `Ret` when it is not the first key (`effects Addr=0 Ret=0`), although `effects Ret=0 Addr=0` i… | printf 'extern!("libc") fn f1(p: ref u8) -> i32 effects Addr=0 Ret=0\nfn main() -> i32 { return 0 }\n' > /tmp/t_A.extc printf 'extern!("libc") fn f2(… |
| 27 | P1 | `src/parser.c:1088` | UNSOUND | A trait method's `effects Ret=0` is believed at `dyn` call sites although no impl is ever checked against it: a view in… | cat > /tmp/t_traitret.extc <<'EOF' trait C { fn view(self: ref Self) -> slice<u8> effects Ret=0 } struct s { buf: slice<u8> } impl C for s { fn view(… |
| 28 | P1 | `src/parser.c:2457` | NONTERMINATION | `continue` inside any `for` loop skips the loop step, so every `for` with a continue hangs forever | $ cd /tmp/rev/diff/findings $ cat F5_continue.extc fn main() -> i32 { var n: i64 = 0 for i in i64(0)..i64(4) { if i == 1 { continue } n += 1 } printl… |
| 29 | P1 | `src/codegen.c:1334` | MISCOMPILE | genBin wraps a `ref` right operand in `&(...)` without checking whether the operand is already a reference: `a == ref b… | file /tmp/rev/work/d_opref.extc: struct s { v: i32 fn ==(self: ref s, o: ref s) -> bool { return self.v == o.v } } fn main() -> i32 { var a: s = { v:… |
| 30 | P1 | `src/codegen.c:1379` | MISCOMPILE | Integer +,-,*,<<,>>,~,- on i8/i16/u8/u16 are computed at C `int` width and never truncated: the value escapes its decla… | $ cd /tmp/rev/diff/findings $ cat F1_narrow.extc fn main() -> i32 { var a: i8 = 100 var x: i8 = a + a // the language types `a + a` as i8 -> x holds … |
| 31 | P1 | `src/codegen.c:1574` | BUG | Zero value of a generic instance whose type argument is `coroutine<T>` is emitted as `(extc_coro){ .unit = false }` - a… | cd /tmp/rev/genwarn && /home/alphayang/extC_Compiler/build/extc -w g2_coro_zero.extc -o g2_coro_zero.c # rc=0, accepted $ gcc -std=c11 -O2 -Wall -Wex… |
| 32 | P1 | `src/codegen.c:2206` | MISCOMPILE | String literals with `\xNN` followed by a hex-digit character are re-decoded by the C compiler under different rules: s… | `printf 'use std::io\nfn main() -> i32 {\n io::cout << "\\x41B" << "\|" << "\\x41" << "B" << "\\n"\n return 0 }\n' > /tmp/rev/t_hex.extc`; `./build/ex… |
| 33 | P1 | `src/codegen.c:2649` | MISSING_TRAP | Sign-changing 64-bit conversions never trap: u64(i64(-1)) silently yields 18446744073709551615 and i64(u64max) yields -1 | $ cd /tmp/rev/diff/findings $ cat F3_u64conv.extc fn main() -> i32 { var a: i64 = i64(-1) var b: u64 = u64(a) println("u64(-1 as i64) = ", b) var c: … |
| 34 | P1 | `src/codegen.c:3665` | UB | -Warray-bounds on generated `__extc_a[3]` with an `extc_arena[2]` declaration: a domain block nested inside an `if` wal… | cd /tmp/rev/genwarn && /home/alphayang/extC_Compiler/build/extc -w g4_arena_bounds.extc -o g4_arena_bounds.c # rc=0, accepted $ gcc -std=c11 -O2 -Wal… |
| 35 | P1 | `src/codegen.c:3756` | MISCOMPILE/FALSE-REJECT | cgLine evaluates an expression after flushPrefix (or without one), so the temporary it queues in the statement prefix i… | (a) /tmp/rev/t_trap_new.extc: `fn n() -> i64 { return 3 }` / `fn main() -> i32 { trap(new u8[n()]) return 0 }`. `./build/extc /tmp/rev/t_trap_new.ext… |
| 36 | P1 | `src/codegen.c:3841` | MISCOMPILE | Pool-owning coroutine driven as a stack frame leaks its whole place: the spawn restores extc_zoneTop before the task en… | cat > /tmp/zl.extc <<'EOF' use stl::pool fn g(n: i64) -> coroutine<i64> { var p = pool<i64>::withCap(n) var i: i64 = 0 while i < n { p.insert(i) i = … |
| 37 | P1 | `src/codegen.c:4521` | MISCOMPILE | A boxed instance of a generic coroutine never ends its task: the frame's task arena and zone are leaked and the dead-ha… | cat > /tmp/leak_g.extc <<'EOF' fn gen<T>(x: T) -> coroutine<T> { yield x } fn main() -> i32 { var k: i64 = 0 var s: i64 = 0 while k < 2000 { var h: c… |
| 38 | P1 | `src/codegen.c:4538` | FALSE-REJECT | Coroutine handle helpers embed the yield type's C spelling in an identifier: a `ref` yield type emits `extc_coro_value_… | cat > /tmp/t2.extc <<'EOF' fn g(x: ref i64) -> coroutine<ref i64> { yield x } fn main() -> i32 { var v: i64 = 7 var h: coroutine<ref i64> = g(ref v) … |
| 39 | P1 | `src/codegen.c:4608` | BUG | An unstarted coroutine that touches the domain layer keeps its `$next` wrapper but loses the `$step` body: `-Wundefined… | cd /tmp/rev/genwarn && /home/alphayang/extC_Compiler/build/extc -w g5_coro_domain_step.extc -o g5_coro_domain_step.c # rc=0, accepted $ gcc -std=c11 … |
| 40 | P1 | `src/codegen.c:4680` | FALSE-REJECT | Coroutine declared as a method references `struct <owner>_<name>$frame` while the frame struct is named `<name>$frame` … | cat > /tmp/cm.extc <<'EOF' struct tick { n: i64 } impl tick { fn count(self: ref tick) -> coroutine<i64> { var i: i64 = 0 while i < self.n { yield i … |
| 41 | P1 | `src/codegen.c:4852` | FALSE-REJECT | A coroutine body that needs hidden frame state emits `__extc_a[1]` / `__extc_home`, neither of which exists in a step f… | A: printf 'fn g() -> coroutine<i64> {\n @overwrite var p = new i64\n *p = 3\n yield *p\n}\nfn main() -> i32 { var c = g() var s: i64 = 0 while c.next… |
| 42 | P1 | `src/codegen.c:4901` | MISCOMPILE | Recursive coroutine increments the recursion guard but never decrements it: sequential spawns eventually die with a bog… | /tmp/rc2.extc: fn drive(n: i64) -> i64 { var c = rec(n) while c.next() { } return 0 } fn rec(n: i64) -> coroutine<i64> { yield n if n > 1 { drive(n -… |
| 43 | P1 | `src/codegen.c:4953` | FALSE-REJECT | Recursive non-void function that can fall off its end emits `return __extc_ret_v;` for an undeclared variable (generate… | cat > /tmp/t3.extc <<'EOF' fn f(n: i64) -> i64 { if n > 0 { return f(n - 1) } } fn main() -> i32 { return i32(f(i64(3))) } EOF ./build/extc /tmp/t3.e… |
| 44 | P1 | `src/codegen.c:6779` | BUG | A `coroutine<T>` named only in a data position (struct field / array element / generic type argument) emits `extc_coro`… | cd /tmp/rev/genwarn && /home/alphayang/extC_Compiler/build/extc -w g1_coro_field.extc -o g1_coro_field.c # rc=0, accepted $ gcc -std=c11 -O2 -Wall -W… |
| 45 | P1 | `src/codegen.c:6785` | BUG | event 层按需发射的触发名单漏了 `extc_unix_` ⇒ 只用 extc_unix_listen/connect 的程序链接失败（--check-c 却报通过） | cat > /tmp/unixonly.extc <<'EOF' use std::sys::net fn main() -> i32 { var path: mut slice<u8> = new u8[32] path[0] = u8(0) var nm: slice<u8> = "extc-… |
| 46 | P1 | `src/codegen.c:7065` | BUG | A `dyn Trait` used only in a data position (struct field / array element / generic type argument) emits `ExtcDynHandle`… | cd /tmp/rev/genwarn && /home/alphayang/extC_Compiler/build/extc -w g3_dyn_typearg.extc -o g3_dyn_typearg.c # rc=0, accepted $ gcc -std=c11 -O2 -Wall … |
| 47 | P1 | `src/codegen.c:7278` | UNSOUND | f64 -> i64 conversion at exactly 2^63 is not trapped and is UB in the generated C ((double)INT64_MAX rounds up to 2^63) | cat > /tmp/t13.extc <<'EOF' use std::io fn main() -> i32 { let f: f64 = 9223372036854775807.0 let x: i64 = i64(f) if x < i64(0) { io::cout << "no tra… |
| 48 | P1 | `src/codegen.c:7326` | UNSOUND | extc_arena_alloc's size rounding overflows for n > INT64_MAX-7, returning a huge slice backed by a tiny block (wild wri… | cat > /tmp/t5.extc <<'EOF' use std::io fn main() -> i32 { let n: i64 = 9223372036854775807 var p: mut slice<u8> = new u8[n] var i: i64 = 1000000 let … |
| 49 | P1 | `src/codegen.c:7696` | MISCOMPILE | A payload-free generic enum instance is emitted as a tag+union struct while its values are bare enumerators, so the gen… | cat > /tmp/t6.extc <<'EOF' type flag<T> = \| off \| on fn main() -> i32 { let f: flag<i64> = flag<i64>::on if f == flag<i64>::on { return 0 } return 1 … |
| 50 | P1 | `src/codegen.c:7708` | MISCOMPILE | A coroutine declared as a method never gets its frame struct defined, so the generated C does not compile (extc exits 0) | cat > /tmp/t8.extc <<'EOF' use std::io struct box { v: i64 } impl box { fn run(self: ref box, n: i64) -> coroutine<i64> { var i: i64 = 0 while i < n … |
| 51 | P1 | `src/codegen.c:8402` | CRASH | parallel::run with threads == 0 divides by zero in the generated C (SIGFPE), although the runtime documents threads <= … | cat > /tmp/t1.extc <<'EOF' use std::parallel fn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 { var i: i64 = lo while i < hi { out[i - lo]… |
| 52 | P1 | `src/coroutine.c:84` | BUG | boxed spawn 不还原 extc_zoneTop，extc_task_end 随后弹掉调用方仍在使用的地方（合法程序 trap/UAF） | cat > /tmp/dybox.extc <<'EOF' use std::io trait Tag { fn tag(self: ref Self) -> i64 } struct box2 { v: i64 } impl Tag for box2 { fn tag(self: ref box… |
| 53 | P1 | `src/coroutine.c:230` | BUG | event 块用 inet_pton 却只有 DNS 块 include <arpa/inet.h> ⇒ 只声明 extc_tcp_connect_to 的程序生成 C 编不过 | cat > /tmp/t_conn.extc <<'EOF' extern!("extc-runtime") fn extc_tcp_connect_to(ip: ref u8, iplen: i64, port: i64) -> i64 effects Addr=0 Cont=0 fn main… |
| 54 | P1 | `src/modules.c:759` | FALSE-REJECT | rwType 只下探 TY_REF 与 targs：数组 `[N]m::T` 和函数类型 `fn(m::T)->R` 里的模块限定类型名永远不被改写（误拒合法程序） | mkdir -p /tmp/x && cd /tmp/x && printf 'struct pair { a: i32 }\n' > lib.extc && printf 'use lib\nfn f(a: [4]lib::pair) -> i32 { return a[0].a }\nfn m… |
| 55 | P1 | `src/modules.c:1209` | FALSE-REJECT | EX_GENCALL 的 callee 名从不改写：模块里的泛型函数一旦写显式类型实参就再也调不到（误拒 + 误导诊断） | A：/tmp/rev/modrev/u1_gencall → `cd /tmp/rev/modrev/u1_gencall && /home/alphayang/extC_Compiler/build/extc -w main.extc -o /dev/null` 观察 `./lib.extc:4… |
| 56 | P1 | `src/modules.c:2005` | BUG | @private 类型不设防：alias 表收录了私有声明的返回票据，裸名（含传递 import）就能使用别人的私有 struct/enum | /tmp/rev/modrev/u2_priv：lib.extc=`@private struct secret { x: i32 }` - `cd /tmp/rev/modrev/u2_priv && /home/alphayang/extC_Compiler/build/extc -w bar… |
| 57 | P1 | `src/check_top.c:230` | UNSOUND | Trait/impl conformance is skipped entirely for targets without a StructDef (builtins and views): `impl Trait for i64` /… | cat > /tmp/x/tr3.extc <<'EOF' use std::io trait Codec { fn enc(self: ref Self) -> i64 } impl Codec for slice<u8> { fn enc(self: ref slice<u8>) { io::… |
| 58 | P1 | `src/check_top.c:289` | MISCOMPILE | Trait/impl conformance compares return types only when BOTH are non-void, so a trait method returning a value may be im… | cat > /tmp/x/tr2.extc <<'EOF' use std::io trait Codec { fn enc(self: ref Self) -> i64 } struct box { v: i64 } impl Codec for box { fn enc(self: ref b… |
| 59 | P1 | `src/check_top.c:857` | MISCOMPILE | `countOwSites` does not descend into `ST_DOMAIN`, while codegen's `collectOwSites` does: an `@overwrite` inside `d.run … | cat > /tmp/x/dom_ow.extc <<'EOF' use std::sys::domain use std::io fn main() -> i32 { let d = domain::single() d.run { @overwrite var buf = new [8]u8 … |
| 60 | P1 | `src/check_top.c:2476` | UNSOUND | Effect-summary masks are 32 bits but a function may have more than 32 parameters: `1u << j` is UB and every argument at… | cp /tmp/rev/findings/wide2.extc /tmp/wide2.extc; cd /home/alphayang/extC_Compiler; ./build/extc -w /tmp/wide2.extc -o /tmp/wide2.c # accepted, exit 0… |
| 61 | P1 | `src/check_top.c:2497` | UNSOUND | `fresh` local set is built once from declarations and never invalidated by retargeting: the callee's effect summary com… | cp /tmp/rev/findings/fresh2.extc /tmp/fresh2.extc; cd /home/alphayang/extC_Compiler; ./build/extc -w /tmp/fresh2.extc -o /tmp/fresh2.c # accepted, ex… |
| 62 | P1 | `src/check_top.c:6750` | MISCOMPILE | MISCOMPILE: a coroutine whose body has no `yield` skips coroFrameLay's field layout, so the generated frame struct has … | /tmp/c1.extc: 'fn c(n: i64) -> coroutine<i64> { var i: i64 = 0 }\nfn main() -> i32 { var h = c(i64(3)) var b: bool = h.next() return 0 }'; ./build/ex… |
| 63 | P1 | `src/check_top.c:6850` | BUG | 泛型协程实例的帧漏掉 `var x = yield e` 引入的绑定（coroBinds 按模板函数指针匹配）⇒ 生成物编不过 | cat > /tmp/c2.extc <<'EOF' fn worker<T>(seed: T) -> coroutine<T> { var got: T = yield seed yield got } fn main() -> i32 { var c: coroutine<i64> = wor… |
| 64 | P1 | `src/types.c:483` | MISCOMPILE | The generated C name of an array type (`array_<len>_<elem>`) and of a generic-enum instance (`<enum>_<arg>`) is not res… | cd /tmp/tt # t_arr.extc: struct array_3_i32 { x: i32 } + var a: [3]i32 = [1, 2, 3] /home/alphayang/extC_Compiler/build/extc -w t_arr.extc -o t_arr.c … |
| 65 | P1 | `src/types.c:961` | FALSE-REJECT | `ttIs(t, "void")` can never be true (`void` is TY_VOID, not TY_BUILTIN): a bare `return` inside any `-> void` function … | cd /tmp/tt # t_vret.extc: fn g() -> void { return } /home/alphayang/extC_Compiler/build/extc -w t_vret.extc -o /dev/null -> t_vret.extc:1:1: error: `… |
| 66 | P1 | `src/check.c:176` | MISCOMPILE | `cNameFor` 生成的 `名字__N` 会与用户自己写的 `名字__N` 撞名：产物重定义或静默错值 | 同块（产物编不过）： printf 'fn main() -> i32 {\n var a__2: i32 = 10\n let a: i32 = 1\n let a: i32 = 2\n return a__2 + a\n}\n' > /tmp/lk2.extc ./build/extc -w … |
| 67 | P1 | `src/check_expr.c:108` | UNSOUND | worker 白名单把"没有 effects 子句的 extern!"当成 Thread=0（extThreadMask 缺省为 0）⇒ worker 可任意调用共享状态的 C 函数 | printf '%s\n' 'use std::parallel' 'use std::sys::pool::*' 'fn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {' ' let rid: i64 = extc_pool_… |
| 68 | P1 | `src/check_expr.c:149` | MISCOMPILE | worker 链上的函数被普通调用时去解引用 NULL 的 extc_tls_arena（生成物必然空指针崩溃） | printf '%s\n' 'use std::parallel' 'fn helper(n: i64) -> i64 {' ' var t: mut slice<i64> = new i64[4]' ' t[0] = n' ' return t[0]' '}' 'fn w(id: i64, lo… |
| 69 | P1 | `src/check_expr.c:359` | FALSE-REJECT | 字面量适配先于隐式拓宽、且失败即报错 ⇒ 同一表达式因字面量左右位置不同而一收一拒 | for e in '5000000000 + 1' '1 + 5000000000' '5000000000 * 2' '2 * 5000000000'; do printf 'fn main() -> i32 {\n let x = %s\n return 0\n}\n' "$e" > /tmp… |
| 70 | P1 | `src/check_expr.c:971` | BUG | lambda 在泛型函数里捕获 T 时，环境结构体字段保留未替换的类型参数（codegen 退化成 int） | printf '%s\n' 'struct point { x: i64 y: i64 }' 'fn gen<T>(x: T) -> i64 {' ' let f = fn() -> i64 { let y = x return i64(1) }' ' return f()' '}' 'fn ma… |
| 71 | P1 | `src/check_expr.c:1376` | MISCOMPILE | `<`/`<=`/`>`/`>=` 的左操作数是 ref 时不报错，静默生成指针比较 | printf 'fn main() -> i32 {\n var n: i32 = 5\n var p: ref i32 = ref n\n if p < 3 { return 1 }\n return 0\n}\n' > /tmp/rev-ce/rcx.extc /home/alphayang/… |
| 72 | P1 | `src/check_expr.c:3150` | FALSE-REJECT | unifyTParams cannot unify an enum instance: a generic function with an `option<T>` / `result<T,E>` parameter can never … | cat > /tmp/ce2/f3.extc <<'EOF' fn f3<T>(o: option<T>) -> T { return o! } fn main() -> i32 { let o: option<i64> = option<i64>::some(i64(3)) return i32… |
| 73 | P1 | `src/check_expr.c:3457` | FALSE-REJECT | `dyn` 派发的实参检查不做上下文定型 ⇒ `d.area(3)` 误拒（静态调用同一行合法） | cat > /tmp/dl.extc <<'EOF' trait Area { fn area(self: ref Self, s: i64) -> i64 } struct sq { v: i64 } impl Area for sq { fn area(self: ref sq, s: i64… |
| 74 | P1 | `src/check_expr.c:3459` | UNSOUND | `dyn` 派发的实参检查剥掉 `ref` 再比 ⇒ 接受 `ref i64` 形参收到裸值，生成的 C 编不过 | cat > /tmp/dr.extc <<'EOF' trait Bump { fn add(self: ref Self, d: ref i64) -> i64 } struct box { v: i64 } impl Bump for box { fn add(self: ref box, d… |
| 75 | P1 | `src/check_expr.c:3753` | BUG | 方法的类型参数在签名里没被实例化 ⇒ 生成的 C 引用未定义的 `slice_U`（编不过），标量时把 `u8` 和 `i64` 当成同一个 U | cat > /tmp/sl.extc <<'EOF' struct box { v: i64 } impl box { fn len<U>(self: ref box, a: slice<U>) -> i64 { return a.len } } fn main() -> i32 { var b:… |
| 76 | P1 | `src/check_lookup.c:105` | UNSOUND | 字段路径的非空证明在 `mut ref self` 方法调用后不失效 ⇒ 生成物解引用 NULL | cat > /tmp/lk1.extc <<'EOF' struct node { value: i64 next: ?ref node } struct stack { top: ?ref node fn pop(self: mut ref stack) -> i64 { var t: ?ref… |
| 77 | P1 | `src/check_lookup.c:671` | UNSOUND | 聚合里的 `coroutine<T>` / `dyn` 句柄未被当作『没有零值』⇒ 零句柄驱动 NULL 帧（SEGV） | cat > /tmp/lk3.extc <<'EOF' struct wrapper { h: coroutine<i64> } fn worker(n: i64) -> coroutine<i64> { yield n } fn main() -> i32 { var c: coroutine<… |
| 78 | P1 | `src/check_stmt.c:1085` | MISCOMPILE | `return e?` 的载荷类型从不与函数返回类型的载荷比较（wb->kind == TY_GENERIC 对 option/result 恒假）⇒ 静默截断 | cat > /tmp/p3.extc <<'EOF' type myerr = \| bad fn f(o: option<i64>) -> option<u8> { return o? } fn main() -> i32 { let x: option<u8> = f(some(1000)) m… |
| 79 | P1 | `src/codegen.c:5109` | BUG | worker 需要 zone 形参时 trampoline 不补传 __extc_home_zone ⇒ 产物编不过（too few arguments） | printf '%s\n' 'use std::parallel' 'trait Tag { fn tag(self: ref Self) -> i64 }' 'struct box { v: i64 }' 'impl Tag for box { fn tag(self: ref box) -> … |
| 80 | P1 | `src/check_escape.c:147` | UNSOUND | placeDepth answers 0 ('lives forever') for a place whose root is not a binding, so a local view laundered through an in… | n1.extc vs n2.extc in /tmp/rev/repro (both store into the global `var G: slice<u8> = "hi"`): n2 is `G = v` where `v: slice<u8> = a[..]` and `a` is a … |
| 81 | P1 | `src/check_escape.c:726` | UNSOUND | valTracesToParam / destIsParam compare parameter *names*, so a local that shadows a parameter makes the store publish i… | m3.extc in /tmp/rev/repro; m2.extc is the identical program without the shadowing local. m2 is rejected: 'argument 3 of `stash` carries a reference i… |
| 82 | P1 | `src/check_escape.c:1447` | UNSOUND | checkStoreEscape's `needsHome` early return lets any function that has a home arena store a borrowed reference into a g… | c4.extc in /tmp/rev/repro: `var G: slice<u8> = "hi"`; `struct hbox { p: mut slice<u8> }`; `fn stash(p: slice<u8>) -> hbox { G = p var b: mut slice<u8… |
| 83 | P1 | `src/check_escape.c:2381` | BUG | main 里 `?` 的操作数只查 targs.len>0，非 option/result 的泛型类型被当成 option 解包 ⇒ 生成物编不过 | cat > /tmp/t4.extc <<'EOF' struct box<T> { v: T } fn main() -> i32 { var b: box<i64> let x = b? return 0 } EOF ./build/extc /tmp/t4.extc -o /tmp/t4.c… |
| 84 | P2 | `src/dataflow.c:104` | UNSOUND | 第 5 个字段（哪怕深度为 0）就把整个函数标成 overflow：字段表满不该升级为整表作废 | $ cd /tmp/dfa && cat fields5.extc # struct big { a..e: ?ref i32 }，f() 里 var h: big = { a: null, b: null, c: null, d: null, e: null } $ EXTC_DBG_DFA=1… |
| 85 | P2 | `src/dataflow.c:403` | UNSOUND | DFA_ROUNDS=64 不是保险丝：70 步直线循环体就打满，打满即整函数结果作废 | $ cd /tmp/dfa # chain70.extc：while n>0 { v0=v1; v1=v2; ...; v68=v69; v69 = new i32; n = n-1 } $ EXTC_DBG_TIME=1 EXTC_DBG_DFA=1 /home/alphayang/extC_C… |
| 86 | P2 | `src/plate.c:39` | BUG | `extc_memCopy` 用 `memcpy` 做板的整块拷贝：`copyIn/copyOut` 允许源与目标都是板内视图 ⇒ 重叠时 -O2 静默拷错 | cat > /tmp/rev/misc/ovl.extc <<'EOF' use std::io use std::heap fn main() -> i32 { var pl: heap::plate = heap::plate::open(heap::DEFAULT_RESERVE)! var… |
| 87 | P2 | `src/pools.c:118` | BUG | `extc_dynPoolOf` 只记 pid、用 `generation == 0` 判陈旧 ⇒ 一个 place 会把 dyn 值生进**别的 place 还活着的对象表**里（地方寿命失效） | 见 /tmp/rev/misc/alias2.extc（f 帧里先建对象表再建容器 → f 退出；main 建自己的 dyn 表；再调 g）： cd /tmp/rev/misc && /home/alphayang/extC_Compiler/build/extc -w alias2.extc -… |
| 88 | P2 | `src/main.c:132` | HYGIENE | A compiled program killed by a signal makes `--run` exit 255 with no message at all | cat > /tmp/t_crash.extc <<'EOF' fn main() -> i32 { var p: ?ref i32 = null return *p! } EOF /home/alphayang/extC_Compiler/build/extc /tmp/t_crash.extc… |
| 89 | P2 | `src/parser.c:606` | BUG | Top-level annotations that do not apply to the declaration that follows are silently dropped (except on `let`/`var`, al… | cat > /tmp/t_ann.extc <<'EOF' @inline struct box { x: i32 } @inline type e = \| a \| b @builtin struct bi { y: i32 } @builtin impl bi { } @builtin trai… |
| 90 | P2 | `src/parser.c:708` | FALSE-REJECT | 全局声明（let/var）上的 `@noCopy` / `@poolObject` / `@sharesStorage` / `@inline` 被静默接受且完全不做任何事 | printf '@noCopy\nvar g: i32 = 0\nfn main() -> i32 { return 0 }\n' > g.extc; ./build/extc -w g.extc -o /dev/null # 退出码 0，无任何诊断；换成 @poolObject/@sharesS… |
| 91 | P2 | `src/parser.c:1096` | BUG | parseTrait throws away the method's own type-parameter list (`fn f<T>` in a trait), so the signature later fails with a… | cat > /tmp/t_traitgen.extc <<'EOF' trait C { fn f<T>(self: ref Self, v: T) -> i64; } struct s { x: i64 } impl C for s { fn f<T>(self: ref s, v: T) ->… |
| 92 | P2 | `src/parser.c:1475` | FALSE-REJECT | `p->sawBuiltin` is parser-global, not per-declaration: a second `@builtin` in a struct/impl body is falsely rejected, a… | cat > /tmp/t_bi3.extc <<'EOF' struct s { x: i32 @builtin fn m1(self: ref s) -> i32 { return 1 } @builtin fn m2(self: ref s) -> i32 { return 2 } } fn … |
| 93 | P2 | `src/parser.c:1575` | BUG | Function type parameters are not checked to start uppercase, so `fn f<point>(p: point)` silently shadows the struct `po… | cat > /tmp/t_shadow.extc <<'EOF' struct point { x: i32 } fn f<point>(p: point) -> i32 { return 0 } fn main() -> i32 { return f(3) } EOF /home/alphaya… |
| 94 | P2 | `src/parser.c:1996` | FALSE-REJECT | `at()` only refuses string-vs-punctuation matches, so a statement-position string literal that spells a keyword is take… | printf 'fn main() -> i32 {\n "abc"\n return 0\n}\n' > /tmp/t_ok.extc # exit 0 printf 'fn main() -> i32 {\n "yield"\n return 0\n}\n' > /tmp/t_kw.extc … |
| 95 | P2 | `src/parser.c:3143` | MISCOMPILE | A raw NUL byte inside a string literal is silently dropped: the parser keeps only `t->text` and codegen re-derives the … | printf 'fn main() -> i32 {\n println("a\000b")\n return 0\n}\n' > /tmp/t_nul.extc /home/alphayang/extC_Compiler/build/extc /tmp/t_nul.extc \| grep -n … |
| 96 | P2 | `src/codegen.c:487` | MISCOMPILE | C keywords are renamed only for functions/methods: a struct/enum type name or a global variable named after a keyword i… | files /tmp/rev/work/m_kw1.extc (`struct double { x: i32 }` + `var d: double = { x: 1 }`), m_kw3.extc (`var int: i32 = 3`), m_kw4.extc (`type int = \| … |
| 97 | P2 | `src/codegen.c:2077` | MISCOMPILE | genGlobalInit makes only the outermost struct literal a brace initializer: a nested literal stays a compound literal, w… | file /tmp/rev/work/t1.extc: struct point { x: i32 y: i32 } struct holder { p: point n: i32 } var g: holder = holder { p: point { x: 1, y: 2 }, n: 3 }… |
| 98 | P2 | `src/codegen.c:2203` | MISCOMPILE | Negative-zero float literals are emitted as the integer `-0`, losing the sign: `1.0 / -0.0` prints `inf` instead of `-i… | /tmp/rev/t_negzero.extc: `use std::io` / `fn main() -> i32 { var z: f64 = -0.0 var w: f64 = 1.0 io::cout << (w / z) << "\n" return 0 }`. Emitted C: `… |
| 99 | P2 | `src/codegen.c:2605` | MISSING_TRAP | f64 -> i64 conversion accepts exactly 2^63 (`v <= (double)INT64_MAX`) and silently returns INT64_MIN | $ cd /tmp/rev/diff/findings $ cat F4_f2i64.extc fn main() -> i32 { var v: f64 = 9223372036854775808.0 println("i64(2^63) = ", i64(v)) return 0 } $ /h… |
| 100 | P2 | `src/codegen.c:3632` | HANG | blkMaxOfBlock evaluates blkMaxLevel twice per statement, making codegen exponential (2^nesting-depth) in the number of … | Generate nested blocks: `python3 -c "n=32; inner='\n'.join(['{']*n + [' x = x + 1.0'] + ['}']*n); open('/tmp/rev/deep.extc','w').write('fn main() -> … |
| 101 | P2 | `src/codegen.c:3632` | HANG | Exponential compile time in block-depth computation: blkMaxOfBlock evaluates blkMaxLevel twice per statement, so nestin… | for n in 24 26 28 30 32: generate `fn f() -> i64 { var x: i64 = 0` + n times `{ x = x + 1` + n closing braces + `return x }` + main, then time ./buil… |
| 102 | P2 | `src/codegen.c:3778` | MISCOMPILE | The receiver of a domain block is generated once per `ext` task and again for the run, so an impure receiver is evaluat… | /tmp/rev/t_domrecv3.extc: a global `calls: i64`, `struct holder { d: domain }` with `fn get(self: ref holder) -> domain { calls = calls + 1 return se… |
| 103 | P2 | `src/codegen.c:6698` | MISCOMPILE | The "the compiler needs this name" guard skips array and enum instances, so a user declaration with that name produces … | cat > /tmp/t9.extc <<'EOF' struct array_2_i64 { x: i64 } fn main() -> i32 { var a: [2]i64 = [1, 2] var s: array_2_i64 = array_2_i64 { x: 7 } return i… |
| 104 | P2 | `src/codegen.c:8342` | UNSOUND | The parallel region is sized and initialized with the uncapped thread count; counts >= 2^60 wrap the size computation i… | cat > /tmp/t12.extc <<'EOF' use std::parallel fn w(id: i64, lo: i64, hi: i64, out: mut slice<slice<i64>>) -> i32 { var mine: mut slice<i64> = new i64… |
| 105 | P2 | `src/coroutine.c:83` | BUG | extc_task_end 用 extc_arena_release 回收任务 arena，而任务槽永不复用 ⇒ 每个跑完的装箱协程永久泄漏一块（实测 30 万任务 +32MB） | /tmp/extcw/leakbox.extc：`var h: coroutine<i64> = w(k)` + `while h.next() {}` 循环 30 万次。 gcc -std=c11 -fwrapv -O2 -o leakbox leakbox.c && /usr/bin/time… |
| 106 | P2 | `src/coroutine.c:137` | BUG | extc_epoll_wait 的待处理队列是函数级 static，不按 epoll 实例区分 ⇒ 跨实例投递别人的 tag | /tmp/extcw/epcross.extc：三对 socketpair 全注册进 epoll A（tag 101/102/103）并各写 1 字节，epoll B 什么都不注册； 先 `net::extc_epoll_wait(epa, 0)`（返回一个、其余入队），再 `net::extc_… |
| 107 | P2 | `src/coroutine.c:203` | BUG | extc_unix_connect / extc_unix_listen / extc_tcp_listen(_shared) 的失败路径不关 fd ⇒ 重试循环每次漏一个 fd | /tmp/extcw/connfail2.extc：同一抽象名字连 50 次（全失败），末尾再调一次 extc_sock_read 把事件层拉进产物。 strace -f -e trace=socket,close -o tr2.txt ./connfail2 # 实测：socket()=50 c… |
| 108 | P2 | `src/coroutine.c:403` | BUG | extc_http_date 用固定的 64 字节上限写调用方的缓冲区，而 extC 声明的 `out: ref u8` 不带长度 ⇒ 越界写 | /tmp/extcw/httpdate3.extc：`var buf: mut slice<u8> = new u8[64]`，取 `var small: mut slice<u8> = buf[0..8]`，把 small.data 传给 file::extc_http_date，再把 buf[… |
| 109 | P2 | `src/modules.c:820` | BUG | 限定类型名的诊断用 ctxError(ctx, 0, ...) 报，渲染成 `file: error:`：整条类型改写路径的错误都没有行号 | /tmp/rev/modrev/u1_line0：lib.extc = `@private struct priv { x: i32 }`，main.extc 第 3 行 `var b: lib::priv`。 cd /tmp/rev/modrev/u1_line0 && /home/alphay… |
| 110 | P2 | `src/modules.c:1557` | FALSE-REJECT | openLookup 遇到「某个被打开模块里是 @private 的同名声明」就报错返回，压掉了另一个模块公开导出的同名符号（误拒合法程序） | /tmp/rev/modrev/u2_open：a.extc=`@private fn helper() -> i32 { return 1 }`，b.extc=`fn helper() -> i32 { return 10 }` - `both.extc`（两个都打开，调 `helper()`）… |
| 111 | P2 | `src/modules.c:1681` | FALSE-REJECT | impl 块只改写目标类型名、不改写 trait 名：`impl lib::Codec for mine` 报 unknown trait | /tmp/rev/modrev/u2_trait：lib.extc=`trait Codec { fn enc(self: ref Self) -> i32 }`，main.extc 第 3 行 `impl lib::Codec for mine { ... }` cd /tmp/rev/modr… |
| 112 | P2 | `src/modules.c:1769` | UNSOUND | isStdlibFile 用裸前缀比较：路径字符串以 stdDir 开头的任意目录都被当成标准库，@builtin / 特权模块闸门被绕过 | mkdir -p /home/alphayang/extC_Compiler/build/../stdlib-x && printf '@builtin fn extc_viewOf(p: ref u8, n: i64) -> mut slice<u8>\n' > /home/alphayang/… |
| 113 | P2 | `src/modules.c:1865` | BUG | 短名冲突检查只看单个 unit 的 use 列表：跨文件的 `x::util` / `y::util` 漏检，两个模块共用 `util$` 前缀后互相绑错类型，报错却指向无辜代码和根文件行 | /tmp/rev/modrev/u2_samename（x/util.extc 与 y/util.extc 各声明一个同名 struct `pair`，字段不同；alpha/beta 各导入一个；main 导入 alpha+beta） cd /tmp/rev/modrev/u2_samename … |
| 114 | P2 | `src/check_top.c:901` | HANG | `funcReachesItself` has no memo and explores every call path up to depth 64: exponential compile time for an `@inline` … | python3 - <<'EOF' N=36; src="" for i in range(1,N+1): for tag in ("l","r"): pre="@inline " if (i==1 and tag=="l") else "" body=f" l{i+1}()\n r{i+1}()… |
| 115 | P2 | `src/check_top.c:3091` | HANG | The level solver's step budget only prints a message: it never stops the walk, and the message claims it was stopped | Read src/check_top.c:3082-3106 and grep for `loud`/`steps`: `grep -rn 'loud\\|ls->steps' src/` returns only 3091-3098 and 3467. No program-driven repr… |
| 116 | P2 | `src/check_top.c:4279` | UNSOUND | overflow 后调用方并不退回保守答案：整个函数的不动点被丢弃，回落到同一文件自称 unsound 的破坏性遍历 | 实测（EXTC_DBG_DFA=1 会打印命中）： $ cd /tmp/dfa && cat fields5.extc # struct big{a..e: ?ref i32}，f() 里 var h: big = {a:null,...} $ EXTC_DBG_DFA=1 /home/alpha… |
| 117 | P2 | `src/check_top.c:6769` | UNSOUND | 跨 yield 的局部量规则（rule 2）对泛型实例失效：typeContainsRef 问的是未替换的 T | cat > /tmp/c3.extc # 具体类型：被拒 type 略 —— 见下： fn w(v: slice<u8>) -> coroutine<slice<u8>> { var x: slice<u8> = v yield x yield x } ./build/extc /tmp/c3.e… |
| 118 | P2 | `src/types.c:275` | FALSE-REJECT | A module's own declaration is unreachable by its bare name when the prelude declares the same name: the plain-name look… | cd /tmp/tt # lib4.extc: struct unit { n: i32 } fn mk() -> unit { return unit { } } # m4.extc: use lib4 fn main() -> i32 { var u: lib4::unit = lib4::m… |
| 119 | P2 | `src/types.c:379` | BUG | `mut` is silently discarded for every non-generic name (`mut i32`, `mut point`) and for generic enum instances (`mut op… | cd /tmp/tt # t_mut1.extc: fn f(x: mut i32) -> i32 /home/alphayang/extC_Compiler/build/extc -w t_mut1.extc -o /dev/null # rc=0, no diagnostic at all #… |
| 120 | P2 | `src/check.c:162` | BUG | cNameFor 不登记已发出的 C 名 ⇒ 形如 `a__2` 的源码名与第二个 `a` 撞成同一个 C 标识符，生成物重定义编不过 | cat > /tmp/n1.extc <<'EOF' fn main() -> i32 { let a = 1 let a = 2 let a__2 = 3 return a + a__2 } EOF ./build/extc /tmp/n1.extc -o /tmp/n1.c ; echo ex… |
| 121 | P2 | `src/check_expr.c:100` | UNSOUND | worker 白名单不识别 EX_GENCALL ⇒ 池原语（poolSlice/poolResize/poolGive）在 worker 里不经任何检查 | printf '%s\n' 'use std::parallel' 'use std::sys::pool::*' 'fn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {' ' var p: mut slice<i64> = p… |
| 122 | P2 | `src/check_expr.c:586` | UNSOUND | poolResize/poolGive 不要求视图是 mut ⇒ 只读 `slice<T>` 被洗成可写 `mut slice<T>` | printf '%s\n' 'use std::sys::pool::*' 'fn main() -> i32 {' ' let rid: i64 = extc_pool_new(i64(-1))' ' var m: mut slice<i64> = poolSlice<i64>(rid, i64… |
| 123 | P2 | `src/check_expr.c:1975` | UNSOUND | `@private` is enforced for fields, methods and operators but not for associated functions: `mod::Type::secret()` is cal… | mkdir -p /tmp/ce2/priv && cat > /tmp/ce2/priv/lib.extc <<'EOF' struct widget { n: i64 @private fn secret() -> i64 { return 42 } } EOF cat > /tmp/ce2/… |
| 124 | P2 | `src/check_expr.c:2989` | BUG | `domain::single(...)`: a @builtin with no body skips argument checking, so extra arguments are silently discarded (side… | cat > /tmp/ce2/domside.extc <<'EOF' use std::sys::domain fn touch() -> i64 { return 1 } fn main() -> i32 { let d = domain::single(touch(), touch(), t… |
| 125 | P2 | `src/check_lookup.c:63` | FALSE-REJECT | `unNarrow` 直接截断整个证明栈 ⇒ 与该绑定无关的非空证明被一起丢掉（误拒） | cat > /tmp/lk4.extc <<'EOF' struct node { v: i32 next: ?ref node } fn main() -> i32 { var a: node var b: node var p: ?ref node = ref a var q: ?ref no… |
| 126 | P2 | `src/check_lookup.c:457` | FALSE-REJECT | `findOperator` 不查 `Type.mholder` ⇒ 泛型实例/内建标量上定义运算符被判非法（并把内部名写进诊断） | cat > /tmp/lk6.extc <<'EOF' impl slice<u8> { fn ==(self: ref slice<u8>, o: ref slice<u8>) -> bool { return false } } fn main() -> i32 { var a: [4]u8 … |
| 127 | P2 | `src/check_lookup.c:791` | FALSE-REJECT | `isPlace` 不认 `*p` ⇒ `(*p)[0..2]` 被当成『临时值』拒绝，诊断理由错误 | cat > /tmp/lk7.extc <<'EOF' fn first(p: ref [4]u8) -> u8 { let s = (*p)[0..2] return s[0] } fn main() -> i32 { var a: [4]u8 return i32(first(ref a)) … |
| 128 | P2 | `src/check_stmt.c:606` | FALSE-REJECT | 被收窄的**字段**作为赋值目标时沿用收窄后的非空类型 ⇒ 合法的可空写入被拒 | cat > /tmp/lk5.extc <<'EOF' struct node { v: i32 next: ?ref node } struct holder { next: ?ref node } fn main() -> i32 { var h: holder var n: ?ref nod… |
| 129 | P2 | `src/check_escape.c:144` | FALSE-REJECT | A store into a field/element of a by-value aggregate parameter is judged against depth 0 instead of the frame, so a saf… | a2b.extc in /tmp/rev/repro holds both functions: `f` (bare `slice<u8>` parameter, store to the binding) compiles, `g` (store to `b.s`) is rejected at… |
| 130 | P2 | `src/check_escape.c:1497` | BUG | 延迟泛型检查的记录端以 owner==NULL 提前返回 ⇒ 自由泛型函数的零值检查永不执行（生成 __extc_reference_has_no_zero_value__） | cat > /tmp/z4.extc <<'EOF' fn mk<T>() -> T { var local: T return local } fn main() { var x: slice<u8> = mk<slice<u8>>() println("x = ", x) } EOF ./bu… |
| 131 | P2 | `src/check_top.c:4419` | FALSE-REJECT | A global whose annotated type is a nullable reference cannot be initialized with `null`: the annotation is not used as … | h2.extc in /tmp/rev/repro: `var B: ?ref i32 = null` -> `./build/extc h2.extc -o h2.c` reports 'h2.extc:1:1: error: `null` here does not know which `?… |
| 132 | P3 | `src/ast.c:160` | HYGIENE | `moduleInit` 漏初始化 `Module.aliases`：结构体里九个 Vec 只 init 了八个，唯一的写入点靠 `if (!out->aliases.arena)` 兜底 | 静态核对：`grep -n 'vecInit(&m->' src/ast.c` 得 8 处，`grep -n 'Vec' src/ast.h` 的 Module 得 9 个 Vec；实际写入点 src/modules.c:2000 带 `!out->aliases.arena` 判空。未构造运行时… |
| 133 | P3 | `src/ast.h:358` | HYGIENE | EX_ASSOC→EX_IDENT 原地改写后 `u.ident.srcName` 别名到 `u.assoc.targs.arena`：该字段是“用户写下的原样名字”，实际却持有一个 arena 指针 | cd /tmp/rev2 cat > liba.extc: `type tone = \| low \| high`；usemod.extc: `use liba` + `var t: liba::tone = liba::tone.low` /home/alphayang/extC_Compiler… |
| 134 | P3 | `src/base.h:12` | DOC | base.h 写下的不变量「没有可变全局状态」与 base.c 的 dbgOn 缓存相反 | 读代码即可核实（无运行期复现）：src/base.h:12 与 src/base.c:18-26 直接矛盾；`grep -rn 'static struct' src/base.c` 可看到唯一一处进程级可变状态。 |
| 135 | P3 | `src/check_internal.h:274` | DOC | `explicitTargs` 注释给出的理由（“union 对某些 kind 不清零 ⇒ 只由一条路径写的字段会读到垃圾”）与代码不符：`exprNew` 是唯一分配点且整节点清零；真正的风险是在原地改 kind 时新成员复用旧成员的字… | cd /home/alphayang/extC_Compiler grep -rn 'sizeof(Expr)' src/ # 仅 src/ast.c:124 sed -n '124p' src/ast.c # arenaAllocZero(a, sizeof(Expr)) grep -rn 'k… |
| 136 | P3 | `src/check_internal.h:415` | HYGIENE | `Checker.fxAltScan` 声明后从未被写、也从未被读；`[fx]` 转储把 `fxAliasWalks` 打印在 `altScan=` 标签下 —— 想看的那个“每处赋值扫描”计数永远是 0，看到的是另一个量 | cd /home/alphayang/extC_Compiler grep -rn 'fxAltScan' src/ # 仅 check_internal.h:415 一处（写 0 次、读 0 次） sed -n '6472,6476p' src/check_top.c # altScan= 位置… |
| 137 | P3 | `src/dataflow.c:316` | HYGIENE | dfStmt/dfBlock 的 depth 参数从不参与任何比较：只有形状像深度闸门，缺的正是那道闸门 | $ cd /tmp/dfa && python3 -c "n=12;print('fn f() -> i32 {');print('if 1 == 1 {\n'*n,end='');print('var x: i32 = 1');print('}\n'*n,end='');print('retur… |
| 138 | P3 | `src/dataflow.c:494` | HYGIENE | dfValueDepth 没有任何调用者（头文件却称它用于修字段表），且 overflow 时返回 0——恰好是最不保守的答案 | $ cd /home/alphayang/extC_Compiler && grep -rn "dfValueDepth" src/ tools/ tests/ \| grep -v dataflow （无输出） $ git show 6f7a131 --stat \| head -5 # 该提交删除… |
| 139 | P3 | `src/domain.c:55` | HYGIENE | `extc_dom_free` 没有任何调用者：每个 `domain::single()` 泄漏堆对象（+ 任务表），而这段死代码正好把泄漏盖住 | cat > /tmp/rev/misc/domleak.extc <<'EOF' use std::sys::domain fn main() -> i32 { var k: i64 = 0 while k < i64(300000) { let d = domain::single() d.ru… |
| 140 | P3 | `src/lexer.c:235` | MISCOMPILE | Float literals that overflow or underflow are accepted silently (`1e999` becomes infinity), while the integer path diag… | printf 'fn main() -> i32 {\n var x: f64 = 1e999\n return 0\n}\n' > /tmp/t_f4.extc /home/alphayang/extC_Compiler/build/extc /tmp/t_f4.extc --check-c; … |
| 141 | P3 | `src/lexer.c:427` | HYGIENE | lexChar moves `lx->pos` by hand and never advances line/col, so every diagnostic after a character literal on the same … | printf "fn main() -> i32 {\n var x = 'a' + )\n return 0\n}\n" > /tmp/t_col.extc /home/alphayang/extC_Compiler/build/extc /tmp/t_col.extc # -> t_col.e… |
| 142 | P3 | `src/main.c:355` | DOC | `--help` (documented as listing every accepted switch) omits `--explain-memory`, which the driver accepts | /home/alphayang/extC_Compiler/build/extc --help 2>&1 \| grep -c 'explain-memory' # -> 0 /home/alphayang/extC_Compiler/build/extc --explain-memory /tmp… |
| 143 | P3 | `src/main.c:576` | HYGIENE | `-o FILE` is silently ignored when `--run` is also given: the requested output file is never written and no error is re… | cd /tmp/extc-t && rm -f /tmp/should_be_written.c timeout 60 /home/alphayang/extC_Compiler/build/extc --run -o /tmp/should_be_written.c t_hyg.extc; ec… |
| 144 | P3 | `src/main.c:632` | HYGIENE | $CC is exec'd as one program path, so a conventional `CC="cc -m32"` (or `ccache cc`) cannot be used by --run or --check… | cd /tmp/extc-t && CC="cc -m32" /home/alphayang/extC_Compiler/build/extc --run t_hyg.extc; echo $? # -> extc: cannot exec `cc -m32`: No such file or d… |
| 145 | P3 | `src/parser.c:48` | UB | `pk()` 在令牌向量为空时下标下溢（`p->toks->len - 1` 变成 SIZE_MAX） | 无（不可达）。静态依据：src/lexer.c 的 lexAll 末尾无条件 push TK_EOF。 |
| 146 | P3 | `src/parser.c:107` | BUG | shown() assumes TK_NEWLINE has empty text, but the lexer stores "\n": diagnostics print a raw newline inside the backti… | printf 'struct s {\n x:\n}\nfn main() -> i32 { return 0 }\n' > /tmp/t_nl.extc ./build/extc /tmp/t_nl.extc -o /dev/null; echo exit=$? # observed: # t_… |
| 147 | P3 | `src/parser.c:396` | HYGIENE | `parseEffectsClause` 末尾有不可达的第二个 `return true;` | sed -n '392,397p' src/parser.c |
| 148 | P3 | `src/parser.c:515` | BUG | 重复注解的检查不一致：`@inline @inline` 与 `@noCopy @noCopy` 静默通过，`@frozen @frozen` 报错 | printf '@inline @inline\nfn f() -> i32 { return 1 }\nfn main() -> i32 { return f() }\n' > d.extc; ./build/extc -w d.extc -o /dev/null # rc=0，无诊断；@noC… |
| 149 | P3 | `src/parser.c:777` | HYGIENE | 顶层 `fn` 分支里的 `// at(&p, "@")` 与随后的 `parseFuncAnnotations` 是不可达死路径 | 读 parser.c:502-605（注解循环）与 777-800（fn 分支）即可判定；`@inline fn f(){}` 走的是 502 行那一遍。 |
| 150 | P3 | `src/parser.c:1058` | HYGIENE | trait 的类型参数不要求大写，与 struct 的检查不一致 | printf 'trait t<x> { fn f(self: ref x) -> i32 }\nfn main() -> i32 { return 0 }\n' > tt.extc; ./build/extc -w tt.extc -o /dev/null # rc=0 |
| 151 | P3 | `src/parser.c:1746` | BUG | Fixed-array length has no upper bound: `[9223372036854775807]i32` type-checks and the generated C is rejected by cc | printf 'fn main() -> i32 {\n var a: [9223372036854775807]i32\n return 0\n}\n' > /tmp/t_big.extc /home/alphayang/extC_Compiler/build/extc /tmp/t_big.e… |
| 152 | P3 | `src/parser.c:2327` | FALSE-REJECT | C-style `for (init; cond; step)` cannot omit the init or the step, although the parser documents the C shape | printf 'fn main() -> i32 {\n var i = 0\n for (; i < 3; i += 1) { }\n return i\n}\n' > /tmp/t_for3.extc /home/alphayang/extC_Compiler/build/extc /tmp/… |
| 153 | P3 | `src/codegen.c:1345` | HYGIENE | The operator-as-method-call path never passes `@overwrite` cells, unlike the three other call emitters (currently laten… | Not reproducible. Tried: (a) `fn +(self: ref box, v: i64) -> box` with `@overwrite var x = new node` called as `b + i64(5)` (/tmp/rev/work/c_owop.ext… |
| 154 | P3 | `src/codegen.c:2167` | HYGIENE | A lambda with no captures emits an empty initializer `(T){}`, which ISO C11 forbids and which leaves the synthesized `_… | `./build/extc tests/lambda/no_capture.extc -o /tmp/rev/t_nocap.c` emits `$lam$main$0 f = ($lam$main$0){};` with `struct $lam$main$0 { char __extc_emp… |
| 155 | P3 | `src/codegen.c:2327` | HYGIENE | The EX_CALL handle-protocol block (next/value/send on a coroutine receiver) is unreachable, and its `send` argument ind… | No program shape reproduces it. Evidence: `printf 'set pagination off\nbreak codegen.c:2327 if e->u.call.callee->kind == EX_FIELD\ncommands\nsilent\n… |
| 156 | P3 | `src/codegen.c:6527` | HYGIENE | markUncalledFunctions lacks the dynTable exemption its sibling pruner has, so a method the dyn table calls is marked EX… | cat > /tmp/t11.extc <<'EOF' use std::io trait Tag { fn tag(self: ref Self) -> i64 } struct pair<T> { a: T } impl<T> Tag for pair<T> { fn tag(self: re… |
| 157 | P3 | `src/coroutine.c:76` | BUG | extc_task_zone 只查上界不查 liveness ⇒ 对从未用过的 id 返回未初始化内存（ASan 下 0xBEBE...），对死任务返回旧 zone | /tmp/extcw/taskzone.extc：一个装箱协程跑完后 `io::cout << syscoro::extc_task_zone(i64(5)) << " " << syscoro::extc_task_zone(i64(-1)) << " " << syscoro::extc_ta… |
| 158 | P3 | `src/modules.c:12` | DOC | 文档与代码不一致：头部说 use 解析「next to the importing file」，实际只用入口文件目录；findModFile 的注释还写了一个不存在的参数 | 读 src/modules.c:11-13 与 396-401；实测：cd /tmp/rev/mine/m9 之类布局，子目录模块里裸写兄弟模块名会报 cannot find module 并列出 `<rootDir>/b.extc`（错误信息本身列的是 rootDir，与头部注释矛盾）。 |
| 159 | P3 | `src/modules.c:236` | BUG | suggestImportPath 不查 L->rootDir，而 findModFile 恰恰先查 rootDir：建议的 `use` 要么给不出，要么指向另一个文件 | A（给不出建议）：/tmp/rev/modrev/u1_suggest（greet.extc 与 main.extc 同目录，main 里写 `greet::thing`）→ `main.extc: error: `greet` is not imported here -- add the `u… |
| 160 | P3 | `src/modules.c:960` | HYGIENE | 构造器改写里的 isFn 探测是死代码：拿已 mangle 的声明名去比源名，永远不相等 | 读代码即可核实（mangle 见 1359-1373 行；构造器分支 947-969 行）。实测对照：/tmp/rev/mine/m21 等模块里 `Type(...)` 全部走 new 路径成功；找不到任何能命中 isFn=true 的输入。 |
| 161 | P3 | `src/modules.c:980` | BUG | `!saveCall` 提前 return：非调用形式的限定名（`greet::K`）丢掉「模块存在但没 import」的诊断，只剩误导性的 unknown type | /tmp/rev/modrev/u1_noncall（greet.extc: `let K: i32 = 9`；main.extc: `fn main() -> i32 { return greet::K }`） cd /tmp/rev/modrev/u1_noncall && /home/alp… |
| 162 | P3 | `src/check_top.c:267` | UNSOUND | Trait/impl parameter comparison starts at index 1 unconditionally: for a receiver-less method the first real parameter … | cat > /tmp/x/tr6.extc <<'EOF' trait Conv { fn conv(x: i64) -> i64 } struct box { v: i64 } impl Conv for box { fn conv(x: bool) -> i64 { return i64(1)… |
| 163 | P3 | `src/check_top.c:2768` | HYGIENE | Checker.escapeesFor is written but never read: the documented cross-call memo does not exist and the key is a truncated… | grep -rn 'escapeesFor' src/ -> only src/check_internal.h:432 (declaration) and src/check_top.c:2768 (write). |
| 164 | P3 | `src/check_top.c:3544` | PERF | Level pass cost is quadratic in the number of store records of one function | Files /tmp/rev/findings/big1000.extc, big2000.extc, big4000.extc (generator: `var vI: ?ref i32 = null` xN then `vI = p` xN then `return v0`); run `/u… |
| 165 | P3 | `src/check_top.c:3942` | HYGIENE | HYGIENE: two copies of the coroutine handle-protocol synthesis disagree on the receiver mutability of `send` (and on mo… | Static: compare check_top.c:3942 with check_top.c:5207 (and check_top.c:3931 vs 5195-5199 for modName). I could not construct an input where the coro… |
| 166 | P3 | `src/check_top.c:5118` | HYGIENE | HYGIENE: allFunctions lists every `impl` method twice (already attached to StructDef.methods), so all four all-passes d… | struct counter { i: i64 n: i64 } + 'impl counter { fn zzNext(self: mut ref counter) -> bool { ... } }' (/tmp/impl2.extc): EXTC_DBG_HOME=1 ./build/ext… |
| 167 | P3 | `src/check_top.c:6048` | PERF | 电平事实重放的收敛判据用错了返回值（promoteInto 返回“成功”而非“有变化”）⇒ 两处不动点循环恒定跑满 32 轮 | EXTC_DUMP_LVL=1 ./build/extc examples/escape-promotion.extc -o /dev/null 2>&1 \| grep 'converged' # => [lvl] N level facts, converged after 32 round(s… |
| 168 | P3 | `src/types.c:264` | BUG | A type parameter written with type arguments (`T<i32>`) silently resolves to `T`: the arguments are dropped instead of … | cd /tmp/tt # t_targ2.extc: fn f<T>(x: T<i32>) -> i32 { return 0 } + f<i32>(y) /home/alphayang/extC_Compiler/build/extc -w t_targ2.extc -o t_targ2.c ;… |
| 169 | P3 | `src/types.c:301` | DOC | The unknown-type diagnostic advertises `str` as a built-in type, but no such type exists | cd /tmp/tt # t_str.extc: fn main() -> i32 { var s: str = 0 return 0 } /home/alphayang/extC_Compiler/build/extc -w t_str.extc -o /dev/null -> error: u… |
| 170 | P3 | `src/check.c:466` | HYGIENE | checkAssignable 不判 want/got 为空就解引用（cppcheck check.c:466 的 want-may-be-NULL 判定：静态成立、当前调用方不可达，但缺护栏） | 无可复现输入（这也是判定为 P3 而不是 CRASH 的原因）。静态验证： grep -n 'ttIsError(Type' -A2 src/types.c # NULL -> false nm -C build/extc >/dev/null; grep -rn checkAssignable … |
| 171 | P3 | `src/check_expr.c:2013` | HYGIENE | EX_ASSOC's `parallel::run` branch reads the wrong union member (`e->u.call.args` on an EX_ASSOC node); it is unreachabl… | Reachability was measured, not assumed: a temporary `fprintf` at the head of `case EX_ASSOC:` (line 1935) printed only `typeName=pcg32 name=withStrea… |
| 172 | P3 | `src/check_expr.c:3527` | HYGIENE | `parallel::run` 的整段内建处理被复制两份（EX_CALL 与 EX_METHOD），修一处不改另一处 | grep -n 'isParRunDecl' src/check_expr.c # 3020 / 3520 两处 sed -n '3020,3111p' src/check_expr.c > /tmp/a; sed -n '3520,3611p' src/check_expr.c > /tmp/b… |
| 173 | P3 | `src/check_lookup.c:550` | HYGIENE | 死代码：`typeContainsProto` 已无活调用方（三处 `0 &&` 关闭）、`checkFnDisplay` 无任何调用方 | grep -rn "typeContainsProto" src/ → 只有 check_lookup.c 的定义 + check_expr.c 三处 `0 &&`； grep -rn "checkFnDisplay" src/ → 只有 check_lookup.c:295 定义与 check_… |

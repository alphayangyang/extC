# extC 编译器审计 — 进行中的实测发现（Lead 亲自核实）

状态标记：[已复现] = 我在本机跑过；[待核] = 候选，需再验。

## F-01 [已复现] 64 位同宽换符号转换不做检查（静默回绕）
- 位置：`src/codegen.c:2615-2654`（`EX_CONV` 检查分支）；关键行 `2623`（i64 目标）与 `2650`（u64 目标）
- 代码：
  ```c
  else
      return arenaPrintf(g->arena,
          "((%s)extc_narrowI((int64_t)(%s), INT64_MIN, INT64_MAX, \"%s\", %d))", ...);
  ...
  else
      return arenaPrintf(g->arena,
          "((%s)extc_narrowU((uint64_t)(%s), UINT64_MAX, \"%s\", %d))", ...);
  ```
- 问题：目标 64 位时把源值**先转成辅助函数的 64 位域**再做范围检查 ⇒ 检查恒真。
  - `var u: u64 = 0  u = u - 1  var y: i64 = i64(u)` → 输出 `y = -1`，**不 trap**（文档：换符号装不下 ⇒ trap）
  - `var s: i64 = 0 - 1  var y: u64 = u64(s)` → 输出 `y = 18446744073709551615`，**不 trap**
  - 对照：`u32→i32`、`i64→u32`、`u8→i8`、`i16(40000)` 都正确 trap。
- 生成物：`extc_narrowI((int64_t)(u), INT64_MIN, INT64_MAX, ...)` / `extc_narrowU((uint64_t)(s), UINT64_MAX, ...)`
- 复现：`cd /tmp/rev/atk && /home/alphayang/extC_Compiler/build/extc --run c_i64_u64.extc`（及 `c_u64_i64.extc`）
- 影响：文档承诺"静默截断被干掉"，唯独 64 位↔64 位换符号静默回绕；调用方拿到错值。
- 严重度：P1（静默错值，非内存不安全）

## F-02 [已复现] `-w` 不作用于 `use` 进来的模块文件
- 位置：`src/main.c:409`（只给根 ctx 设 `noWarn`）、`src/modules.c:1821`（模块 ctx 用 arenaAllocZero + ctxInit，inherit 不到）
- 复现：
  ```
  cd /tmp/rev/modwarn && ./build/extc -w main.extc -o /dev/null   # main.extc: use lib
  → 仍打印 ./lib.extc:2:1: warning: `println` is deprecated
  ```
- 另外：`ctxInit`（src/base.c:416-426）**不初始化** `warnCount` 与 `noWarn`；根 ctx 的 `warnCount` 从未初始化就被 `++`（base.c:481），属未初始化读。
- 严重度：P2（开关失效 + 未初始化字段）

## F-03 [已复现] codegen 对嵌套块的耗时指数爆炸（HANG）
- 位置：`src/codegen.c:3628-3635` `blkMaxOfBlock`
  ```c
  for (size_t i = 0; i < block->u.block.stmts.len; i++)
      if (blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i)) > m)
          m = blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i));   /* 同一个调用做两遍 */
  ```
  每层嵌套把子树的 `blkMaxLevel` 算两遍 ⇒ 深度 n 的嵌套块是 O(2^n)。
- 实测（`fn main` 里 n 层 `{ }`）：n=20 → 0.10s；n=24 → 0.10s；n=26 → 0.41s；n=30 → 5.5s；n=40 → >60s 未返回。
- callgrind（n=24）：`blkMaxOfBlock` 53.4% + `blkMaxLevel` 36.7% 指令数，共 2.0e9 条 ⇒ 指数已证实。
- 调用点：`src/codegen.c:4715`（每个函数体算一次 maxLv）、`3403`（协程）。
- 复现：`/tmp/rev/atk/t_30.extc`，`EXTC_DBG_TIME=1 ./build/extc t_30.extc -o /dev/null` 显示 `[time] cg-emit 5.354 s`。
- 严重度：P1（可构造输入让编译器不返回；n=30 只是 30 层块）

## F-04 [已复现] 深嵌套输入让编译器段错误（栈溢出，两处）
- 复现 A（~1000 层块）：`/tmp/rev/atk/db_1000.extc` → SIGSEGV(139)。
  gdb：`#0 dfStmt (src/dataflow.c:322)` ↔ `#1 dfBlock (src/dataflow.c:319)` 互递归，崩溃时 `depth=204`
  ⇒ 每层帧很大（`dataflow.c` 的 `Facts` 结构按值复制，`DFA_MAX_VARS=256`），8MB 栈只够约 200 层。
  `depth` 参数被传递但没有任何上限判断。
- 复现 B（16000 层括号）：`/tmp/rev/atk/deep_paren.extc` → SIGSEGV；
  gdb：`parseUnary (parser.c:2732)` → `parseFactor` → `parseTerm` …（parser.c 递归下降无深度上限）。
  8000 层正常返回，16000 层崩溃。
- 另有性能面：`[1][1]...i32` 嵌套数组类型 n=1000 → 2.0s，n=2000 → 19.8s（平方级），最终报错退出。
- 严重度：P1（编译器对输入崩溃 = 拒绝服务；合法程序也能写出深嵌套）

## F-05 [已复现] `-w` 不作用于模块（已在上文 F-02）
（见 F-02）

## F-06 [已复现·P0] `call(...)?`（EX_TRY）让逃逸集合失效 ⇒ 生成物 heap-use-after-free
- 来源：checktop-2 报告（`src/check_top.c:2022` `markNamesInStmt` 只认 EX_CALL/EX_METHOD/EX_ASSOC，
  EX_TRY 落进 `default: return false`），Lead 独立复现。
- 复现：`/tmp/rev/findings/tryb3.extc`（= tests/arena-promoted/B3 的表达式语句版，两处调用写成 `...?`）
  ```
  ./build/extc -w tryb3.extc -o /tmp/rev/tryb3.c           # rc=0，接受
  gcc -fsanitize=address,undefined -fwrapv /tmp/rev/tryb3.c -o /tmp/rev/tryb3 && /tmp/rev/tryb3
  → AddressSanitizer: heap-use-after-free ... tryb3.extc:40 in main
  ```
  对照 `tests/arena-promoted/B3_exprstmt_method.extc`（不带 `?`）ASan 干净。
- 严重度：P0（语言核心承诺"引用不悬垂"被绕过；生成物真实 UAF）

## F-07 [已复现·P1] `fresh` 名字集合从不失效 ⇒ 调用点生命周期检查整段失效（stack-use-after-scope）
- 来源：checktop-2（`src/check_top.c:2497`）；Lead 复现。
- 复现：`/tmp/rev/findings/fresh2.extc`
  ```
  ./build/extc -w fresh2.extc -o /tmp/rev/fresh2.c && gcc -fsanitize=address ... && ./fresh2
  → AddressSanitizer: stack-use-after-scope ... fresh2.extc:17
  ```
  （直接 `n = target` 的写法会被正确拒绝：`/tmp/rev/findings/fresh1.extc`）
- 严重度：P1（健全性漏洞）

## F-08 [已复现·P1] 效果掩码只有 32 位、参数 ≥33 个之后不再检查（stack-use-after-scope）
- 来源：checktop-2（`src/check_top.c:2476` `1u << j`，j≥32 是 UB；消费循环停在 `j < 32`）；Lead 复现。
- 复现：`/tmp/rev/findings/wide2.extc`（33 个参数）被接受 → ASan stack-use-after-scope wide2.extc:13；
  4 参数的对照 `/tmp/rev/findings/small2.extc` 被正确拒绝。
- 严重度：P1

## F-09 [已复现·P0] 泛型类型参数深度超限后 `funcInstance` 返回 NULL 被解引用 ⇒ 编译器 SIGSEGV
- 来源：checktop-3（`src/check_top.c:4528`，调用方 `src/check_expr.c:2405`/`:3180` 直接 `inst->used = true`）。
- 复现：`/tmp/d65.extc`（65 层 `box<...>` 类型实参）→ SIGSEGV；64 层正常报错退出。
  另一路径：4300 个不同泛型实例 ⇒ 同样 SIGSEGV。
- 严重度：P0（编译器崩溃）

## F-10 [已复现·P0] 自由泛型函数返回 `T` 时不做零值检查 ⇒ 生成 `(cellfn){0}`，调用空函数指针
- 来源：checktop-3（`src/check_top.c:4865`，记录点在 `src/check_escape.c:1474/1496/1529` 对 `owner == NULL` 提前返回）。
- 复现：`/tmp/f3.extc`
  ```
  struct cellfn { f: fn(i32) -> i32 }
  fn mk<T>() -> T { var b: T  return b }
  fn main() -> i32 { let c: cellfn = mk<cellfn>()  return c.f(3) }
  ./build/extc -w f3.extc -o /tmp/rev/f3.c && gcc -O0 -fwrapv ... && ./f3 → Segmentation fault (rc=139)
  生成物第 569 行：cellfn b = (cellfn){0};
  ```
- 严重度：P0（健全性：语言声称 fn/ref 字段无零值，此处静默给零值并调用）

## F-11 [已复现·P1] 泛型调用点只做一遍解析 ⇒ 生成物引用从未生成的 `f_T`（声明顺序相关）
- 来源：checktop-3（`src/check_top.c:5596`）。
- 复现：`/tmp/g3.extc`（三链 f←g←m，模板声明顺序内层在前）
  ```
  ./build/extc -w g3.extc -o /tmp/rev/g3.c && gcc -fsyntax-only /tmp/rev/g3.c
  → error: implicit declaration of function 'f_T'
  ```
  把声明顺序反过来（`/tmp/g3r.extc`）则编译通过 ⇒ 顺序相关。
- 严重度：P1（编译"成功"但生成物编不过）

## F-12 [已复现·P1] 无 `yield` 的协程不生成帧布局 ⇒ 生成物编不过
- 来源：checktop-3（`src/check_top.c:6750`，`coroFrameLay` 提前返回）。
- 复现：`/tmp/c1.extc`
  ```
  fn c(n: i64) -> coroutine<i64> { var i: i64 = 0 }
  fn main() -> i32 { var h = c(i64(3))  var b: bool = h.next()  return 0 }
  ./build/extc -w c1.extc -o /tmp/rev/c1.c   # rc=0
  gcc -fsyntax-only /tmp/rev/c1.c → error: 'struct c$frame' has no member named 'pc' / 'arena1'
  ```
- 严重度：P1（编译器报成功、产物编不过）

## 待归档（来自子代理/队友，需 Lead 复核）
- /tmp/rev/findings/checktop-2.json（6 条）
- /tmp/rev/findings/checktop-3.json（6 条）
- .audit/findings/*.json（队友产出，进行中）

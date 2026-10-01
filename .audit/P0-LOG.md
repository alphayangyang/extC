# P0 批次进展（2026-09-30）

目标：① 先立四道能判红的闸门；② 修 6 条一行/数行级 P0（+2 条被闸门点名的崩溃）；
③ 每条带回归用例；④ `tests/run.sh` 与 `check.sh` 全绿。

## 一、四道闸门（新增，已接进 `check.sh`）

| 闸门 | 脚本 | 挡什么 | 基线 | 接在哪 |
|---|---|---|---|---|
| ① 全语料 `--check-c` | `tools/gate_checkc.py` | "extc 报成功、生成物编不过 / 有可疑告警"（gcc+clang，`-c -O2`，按告警类别判红） | `tools/gate-known-bad.txt`（当前**空**） | `check.sh` 完整模式 |
| ② 正例生成物 ASan+UBSan | `tools/gate_asan_corpus.py` | "生成物有 UB 却看起来正常"（`-O2` 会把 UB 优化掉） | `tools/gate-asan-known-bad.txt`（当前**空**） | `check.sh` 完整模式 |
| ③ 常驻差分 fuzz | `tools/gate_difffuzz.py` | "静默算错"：extC 与等价 C 的结论必须一致（17 个固定用例，含浮点/窄类型/转换/求值次数/for+continue） | `tools/gate-diff-known-bad.txt`（2 条） | `check.sh` quick |
| ④ 规模/嵌套 | `tools/gate_scale.py` | "编译器自己崩或挂住"（12 个用例，各带时间预算 + 内存上限） | `tools/gate-scale-known-bad.txt`（3 条） | `check.sh` quick |

公共实现 `tools/gatecommon.py`：语料定义、"生成 C"、"编译生成物"、告警类别表、**棘轮式基线**
（每个失败都必须在基线里；基线里已不再失败的条目会让闸门变红 ⇒ 只许缩小）。

## 二、本批修好的条目（每条都实测过）

| 审计条目 | 位置 | 改动 | 证据 |
|---|---|---|---|
| **P0-22** dropRuntimeDefs 死循环 | `codegen.c:5798` | `continue` 前先推进游标（复制循环底部的 `ln = eol + 1`；`!eol` 时 `break`） | `hashSet.extc` 作根文件：**>15 分钟不返回 → 0.11 s**；scale 闸门 `hashset_root` 用例转绿并从基线删除 |
| **P0-11** `d.run{}` 少算一层 + blkMax 指数 | `codegen.c` `blkMaxLevel`/`blkMaxOfBlock` | 补 `ST_DOMAIN` 分支；每个子语句只调一次（原来两次 ⇒ 2^n） | `dom.extc` 生成 `__extc_a[3]`（原 `[2]`，越界）；30 层嵌套块 **5.4 s → 0.1 s**；ASan 闸门（`tests/ext`）全绿 |
| **P0-13** `?ref` 下标/切片无非空检查 | `check_expr.c` `EX_INDEX`/`EX_SLICE` | 补 `rejectNullableDeref`（与字段/`*`/方法接收者一致） | 两条新反例 `tests/errors/nullable_ref_{subscript,slice}.extc` 被正确拒绝 |
| **P0-14** `parallel::run()` 零实参崩 | `check_expr.c` `EX_CALL` 内建分派 | arity 检查前先判空并给诊断 | 新反例 `tests/errors/parallel_run_noargs.extc`；原来是 rc=139 零诊断 |
| **P0-8** `copyInto` 空视图 memmove | `codegen.c` `genViewCopier` | 补 `if (n > 0 && (!d.data \|\| !s.data)) trap`（姊妹函数早有） | 新 trap 用例 `tests/traps/copy_into_dead_view.extc`（trap 带位置）；ASan 闸门纳入 tests/traps |
| **P0-12** f64 整数值字面量发成整型 | `codegen.c` 字面量发射 | 文本里没有 `.`/`e`/`E` 就补 `.0`（`-0.0` 的符号随之保住） | 新正例 `examples/float-literal-forms.extc`（`0.5\|3.5\|1e+18\|-inf`）；差分闸门 3 条转绿并删基线 |
| **P0-4** `funcInstance` NULL 被解引用 | `check_expr.c` 两个调用点 | `if (!inst) return ttError(tt);` | 65 层嵌套类型：SIGSEGV → 带位置的 `internal:` 诊断；64 层照旧；scale 用例转绿并删基线 |
| （闸门①首捕）协程 typedef 门控 | `codegen.c` `cType` | 打印 `extc_coro` 时**登记需求**（`needCoroHandle = true`）——"需求闭包"的最小形态 | `stdlib/std/coro/scheduler.extc` 产物从编不过 → gcc 零告警；checkc 闸门 135 文件 0 失败 |

## 三、验收

| 项 | 结果 |
|---|---|
| `./tests/run.sh` | **316 通过 / 0 失败**（311 + 5 条新回归用例） |
| `./check.sh quick` | **53 节全绿 / 0 失败**（原 51 + 2 道新闸门） |
| 闸门① 全语料 `--check-c` | **966 文件（gcc+clang）0 失败**；quick 135 文件 0 失败 |
| 闸门② ASan+UBSan | **209 正例 1 条基线**（`domain_runs.extc`，闸门新抓到的真 UAF）；quick 102 正例 0 失败 |
| 闸门③ 差分 | 17 用例，2 条已知坏（P0-15/P0-18） |
| 闸门④ 规模 | 12 用例，3 条已知坏（P0-10：深嵌套栈溢出） |

基线里剩下的 6 条，正好是**下一批（P1）的施工单**：
- `for_continue` → P0-15（脱糖契约 R9）
- `i8_wrap` → P0-18（算术语义 R5）
- `blocks200` / `blocks1000` / `parens16000` → P0-10（编译器资源上限 R8）
- `tests/ext/domain_runs.extc` → 帧合成单点（R12/R4），闸门新抓到的那条

## 三·五、闸门自身的质量修正（第一版会误报，逐条修掉）

闸门的第一版跑完整语料时出现 9 条"新失败"，逐条归因后发现**7 条是闸门自己的假设错**，
2 条是真问题。修掉的是：

| 误报 | 原因 | 修法 |
|---|---|---|
| `tests/arena-promoted/R2a/R2b` 判失败 | 拿**退出码**当判据，而正例程序常把计算结果当退出码（R2a 返回 `s.len` = 4） | 判据改成"sanitizer 报告 / 非预期 trap"，退出码不参与 |
| `tests/impl/file_string.extc` 等 rc=1 | 运行时 cwd 是 `build/gate`，而套件约定在**仓库根**跑 | 运行 cwd 改成仓库根 |
| `tests/impl/orphanmod.extc` 链接失败 | 它是**给别的模块用的 helper**（没有 `main`），不是程序 | 链接错误里含 `undefined reference to main` ⇒ 跳过 |
| `tests/traps/arena_oom.extc` ASan 报错 | 该用例**故意**要 1PB 让 extC 自己报 `out of arena memory`；ASan 先崩 | `ASAN_OPTIONS=allocator_may_return_null=1` |
| `tests/ext/use_advice_works.extc` 判泄漏 | extC 的内存模型是 arena、进程退出才归还；"退出时仍持有"是设计 | `ASAN_OPTIONS=detect_leaks=0` |
| `arena_oom` 的 `WARNING: ... failed to allocate` 被当报告 | 把 ASan 的 WARNING 也匹配了 | 只认 `ERROR: AddressSanitizer` / `runtime error:` / `LeakSanitizer` |
| `--scope quick` 说全量基线"过期" | 棘轮把"不在本次扫描范围内"的条目也算了 | 棘轮只看**本次范围内**的条目（`eligible`） |

**闸门新抓到的真问题（1 条）**：`tests/ext/domain_runs.extc` 的 `stack-use-after-scope`
（`task$step`）。它是审计**已记**的一条（`.audit/findings/misc.json`：`src/domain.c:49` 存帧裸指针），
但一直被 `__extc_a[2]` 越界掩盖 —— **P0-11 修好后它才露出来**。属"帧/句柄合成只有一处"那批（R12），
进 ASan 基线，列为下一批第一条。

## 四、怎么跑

```sh
python3 tools/gate_checkc.py --scope quick   # 或 full（gcc+clang）
python3 tools/gate_asan_corpus.py --scope quick
python3 tools/gate_difffuzz.py
python3 tools/gate_scale.py
# 基线收敛（只在确认"新增失败确实是已知问题"时用；会把当前失败写成基线）
python3 tools/gate_scale.py --update-allowlist
```

`./check.sh quick` 会跑闸门③④；`./check.sh`（完整模式）额外跑闸门①②。

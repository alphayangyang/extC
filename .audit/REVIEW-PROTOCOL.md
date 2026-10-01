# extC 编译器审计 — 审阅协议（所有审阅智能体共用）

仓库：`/home/alphayang/extC_Compiler`（C11 写的编译器，约 3.9 万行 `src/`，把 extC 编译成 C11）。
编译器二进制：`./build/extc`（用法 `./build/extc FILE.extc [-o out.c] [--run] [--check-c]`，`--help` 看全部开关）。
现状：`./tests/run.sh` 311 例全绿 ⇒ 要找的是**它们没覆盖**的真实缺陷。

## 目标
逐行审阅分配给你的**文件区间**（不是抽样；用 read 工具按 ~500–700 行分段读完），找出真实缺陷。
判断只以**代码实际行为**为准。

## 铁律
1. **注释与文档不可信**。注释声称的不变量不是证据；要找到真正落实它的代码（或证明它没被落实）。
2. 报之前先**自我反驳**：查调用方/被调方/别的文件/数据结构不变量。不变量在别处已保证的不要报。
3. 每条 finding 必须包含：
   - `file` + `line`（真实行号，必须核对过）
   - `code_quote`：2–10 行**逐字**代码
   - `why`：为什么错（点明被违反的不变量）
   - `trigger`：什么输入/程序形状会走到
   - `repro`：尽量给出可复现命令。写 `/tmp` 下的 `.extc`，用 `./build/extc` 实测；codegen/健全性问题把生成的 C 用
     `gcc -fsanitize=address,undefined -fwrapv -O1` 编出来跑，贴出观察到的输出。做不到就写清试过什么。
   - `confidence`：high / medium / low（诚实）
4. kind ∈ {BUG, UB, CRASH, HANG, MISCOMPILE, UNSOUND, FALSE-REJECT, PERF, HYGIENE, DOC}
   severity ∈ {P0, P1, P2, P3}
   - P0：编译器崩溃/内存破坏；或对**常见**程序静默生成错码/不安全的 C
   - P1：健全性漏洞（放行了会 UB 的程序）、误拒合法程序、窄路径错码、产物编不过
   - P2：健壮性/边界/诊断质量/性能陷阱
   - P3：有具体代价的卫生问题（死代码掩盖 bug、未检查的分配、已分叉的重复逻辑、丢数据的窄化）
5. **宁缺毋滥**：每个单元最多 8 条，按严重度排序。风格/命名/纯格式不报。空列表 + 扎实的 `coverage_notes` 是合格答案。
6. 环境可能中断你：**每完成一个单元立刻写文件**，别把结果只留在回复里。

## 输出
每个单元写一个 JSON 文件（路径见分配）：
```json
{"findings":[{"title":"...","file":"src/x.c","line":123,"kind":"BUG","severity":"P1",
  "code_quote":"...","why":"...","trigger":"...","repro":"...","confidence":"high"}],
 "coverage_notes":"读过的函数清单 + 已核实为正确的点 + 无法确认的点"}
```

## 已知（Lead 已实测，不要重复报，可作为上下文）
- 64 位同宽换符号转换 u64↔i64 不 trap（`src/codegen.c:2623` / `:2650` 先把源值 cast 再查范围）。
- `-w` 不作用于 `use` 进来的模块（`src/main.c:409` 只设根 ctx；`src/modules.c:1821` 模块 ctx 未继承；`ctxInit` 不初始化 `noWarn`/`warnCount`）。
- 嵌套块 30 层让 codegen 指数变慢（`src/codegen.c:3628-3635` `blkMaxOfBlock` 每次把子节点算两遍）；~200 层栈溢出段错误（`src/dataflow.c:316-322` 互递归 + `Facts` 按值复制）；16000 层括号段错误（parser 递归下降无深度上限）。
- 已确认的健全性漏洞（别人报的，已由 Lead 复现）：`call(...)?`（EX_TRY）不进逃逸集合（check_top.c:2022）；`fresh` 名字集合从不失效（check_top.c:2497）；效果掩码 32 位、第 33 个参数起不检查（check_top.c:2476）；自由泛型 `mk<T>` 零值检查缺失（check_top.c:4865）；泛型深度超限后 `funcInstance` NULL 被解引用（check_top.c:4528）；调用点 arena 深度用 0 当"未设置"（check_top.c:6024/6330）。

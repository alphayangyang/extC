# 扰动普查（`MALLOC_PERTURB_`）

## 为什么有这一套

`MALLOC_PERTURB_` 把未初始化的堆内存填成一个**确定值**，于是"偶发 1–4%"的未初始化读变成必现。
ASan/UBSan **照不到**这一类（那是 MSan 的领域），而它的危害不止崩溃 —— 编译器读了垃圾值可能得出
**错误的分析结论 ⇒ 误编译**。task-10 修掉的那条（`symFieldsGrow` 少置零 ⇒ `promoteFieldsAt` 把
垃圾当 `Expr *`）就是这一类；本目录负责把"还有多少这种 bug"从猜测变成一个**数**。

## 跑什么

```bash
python3 tests/perturb/census.py                     # 全语料 × 三档（重活：先确认机器空）
python3 tests/perturb/census.py --limit 3 --filter stl/clone   # 自检（轻）
python3 tests/perturb/census.py --check             # 门禁口径：与 known-bad.txt 比
python3 tests/perturb/census.py --extc /tmp/extc.pinR2 --arms 165 --limit 1 --filter stl/clone
                                                    # 用固定副本跑（副本在 /tmp 时脚本自动带 EXTC_STD）
```

- 语料：`tests/**/*.extc` + `bench/**/*.extc` + `examples/**/*.extc`（不含本目录）。
- 档位：`plain`（不设）、`42`、`165`（glibc 的 `MALLOC_PERTURB_`）、`afill`（`EXTC_ARENA_FILL=165`，
  我们自己的 arena 分配器填图案 ⇒ **这一类不依赖 glibc 的偶然行为**）；每个源每档各跑一次
  `build/extc --run <源>`。门禁跑 `plain,42,165,afill` 四档。
- **每次运行一个私有的 `EXTC_RUN_DIR`**（`/tmp/extc-perturb-runs/<档>/<源>`）：`--run` 默认写
  `build/<源基名>`，语料里同名源很多（`tests/ops/generic.extc` 与 `tests/ctor/generic.extc`），
  不隔离就会互相截断/覆盖，普查结果就全是假红。

## 分类（"崩在编译器还是被编译的程序"）

| 分类 | 判据 | 含义 |
|---|---|---|
| `crash` | `rc < 0`（被信号打死） | **编译器自己崩了** ⇒ 未初始化读的强信号 |
| `reject` | 非零 + `...: error:` | 语料本来就是反例（三档一致算正常） |
| `driver` | 非零 + `extc:` | 驱动器自己报错（建目录/写文件/`.link` 指令非法…）⇒ **要人工判读一次**：可能是普查自己出问题，也可能是语料在测驱动器的报错路径 |
| `program` | 其余非零 | 编译器没说不行，被编译的程序非零退出（需要 argv/stdin 的多在这里） |
| `ok` | `rc == 0` | 正常 |

另外记一条**比崩溃更值得看**的：`discordant` = 同一个源在 `plain` 与扰动档之间**退出码不同**
（例如 plain 0 / 165 1）⇒ 疑似**误编译**，不是崩溃也会被列出来。

## 归属（既存 / 本次引入）

每个 `crash`：

1. 同档重跑 2 次（记录 `2/2` 是否可复现）；
2. `gdb -batch -ex run -ex "bt 6"` 取前几帧，**按栈顶 `src/` 帧去重**成"不同根因"；
3. 与固定副本逐一对照同一源同一档（`/tmp/extc.pin3`、`pin-prehoist`、`pinR1`、`pinR2`；
   四个都崩 ⇒ **既存**）。副本缺失就跳过并在输出里标注 `(无 pin 可比)`。

## 已知坏清单与门禁

- `known-bad.txt`：每行 `源相对路径<TAB>原因<TAB>跟踪`，`#` 开头是注释。**棘轮：只许缩小**。
- 门禁 `gate-perturb-corpus`（`tools/check.py`，**bench 档**）跑 `census.py --check`：
  崩的源集与清单**逐条相同**才绿；多一条=`NEW`（新增坏源）、少一条=`STALE`（修好了要删清单）。

## 覆盖边界（别把它读成"这一类已清空"）

- 只照得到**扰动触发得了**的那条路径：读未初始化内存后**没有影响控制流/不崩**的，本方法看不见。
- `program` / `discordant` 里混着"需要 argv/stdin 的源"，它们不是编译器缺陷（报告里单列）；
  `driver` 一桶里既有普查自身问题、也有语料故意测驱动器报错的（例如 `tests/linkflags/badlink/app.extc`
  的 `.link` 里写了非法指令）⇒ 报告里单列，人工判读。
- 多线程/协程、以及需要特殊环境变量的源，`--run` 裸跑不等于套件里的跑法 ⇒ 报告里单独记"未覆盖"。
- 数字不写进本文件：总体数、去重根因数、每根因的背栈都在普查输出里
  （`--report` 落 `report.md`、机器可读的 `summary.json`）。

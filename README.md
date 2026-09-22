# extC

一门编译到 C 的语言。用编译期检查替代运行时机制。

> **一句话**：凡是编译期能证明的，运行时不留痕迹。

## 文档

> ⚠️ **2026-09-23 补齐**：这张表原来只列 11 份，而仓库里有 **21 份** ⇒ 9 份专题文档
> **一份都没进索引**（`ARENA*` · `ARRAYS` · `REFS` · `BOOTSTRAP` · `IO` · `PLAN-REGION` ·
> `VISION` · `extC-overview`）✗ —— 读的人只能靠 `ls` 猜哪份是权威 ✓
> ⇒ 现在**全部列出**，并标明每份的**性质**（现行 / 历史 / 已取代）✓

| 文档 | 给谁看 | 内容 |
|---|---|---|
| [`LANGUAGE.md`](LANGUAGE.md) | **想知道「extC 是什么」** | ⭐ **正式设计文档**：定位、原则、语言表面、内存模型、代价与损失（⚠️ 它的**状态标记**以 `PLAN.md` §0.4 + 实测为准）|
| [`MANUAL.md`](MANUAL.md) | **要写 extC 程序的人** | 语言手册。**只描述已实现的东西** |
| [`DESIGN.md`](DESIGN.md) | 想知道「为什么这么设计」 | 一条原则 + 全部推论 + 判出局清单 |
| [`DECISIONS.md`](DECISIONS.md) | 想知道「哪条定了、哪条还欠着」 | 语法决策的「已定案 / 还欠着」两栏（**最全的历史决策库**）|
| [`PLAN.md`](PLAN.md) | **想知道「现在什么坏了 / 接下来做什么」** | ⭐ **进度与缺陷的唯一权威**（§0.4 缺陷清单 + §0.6 修法记录）|
| [`DEVLOG.md`](DEVLOG.md) | 想知道「发生过什么」 | 开发记录：决策与发现的来龙去脉（**倒序，最全**）|
| [`ARENA.md`](ARENA.md) | 想知道「内存模型怎么落地」 | arena = 词法作用域（主人的模型）+ 逐条落地 |
| [`ARENA-FORMAL.md`](ARENA-FORMAL.md) | 想知道「为什么这样是 sound」 | ⭐ arena 规则的**严格推导**（约束系统 / 定理 / 误拒分析 / 调用点求解）|
| [`PLAN-REGION.md`](PLAN-REGION.md) | 想知道「形式化怎么落进编译器」 | `ARENA-FORMAL` 的执行计划（**条目已全部 ✅**，留作记录）|
| [`REFS.md`](REFS.md) | 想知道 `ref` 的语义 | 引用现在在哪、该往哪走 |
| [`ARRAYS.md`](ARRAYS.md) | 想知道数组/切片 | 旧稿 × 当前标准的合并版 |
| [`IO.md`](IO.md) | 想知道输入输出 | 设计 + **三段顺序（IO-0/1/2）**；第一块已落地 |
| [`LIBS.md`](LIBS.md) | 想知道「库怎么做」| **编译到 C 意味着什么**（C 当 ABI / 两种消费者 / 跨边界签字）+ 四种分发模型 |
| [`MODULES.md`](MODULES.md) | 想知道「模块系统怎么做」| 调研 + 候选方案（12 门语言怎么做）；**模块 v1 已落地**（定案 70）|
| [`CONCURRENCY.md`](CONCURRENCY.md) | 想知道**协程 / 线程**该怎么做 | ⚠️ **设想 + 风险清单**（**一个字都没实现**）：为什么必须无栈 · 帧与 arena 怎么结合 · 已知代价 · 落地顺序 |
| [`BOOTSTRAP.md`](BOOTSTRAP.md) | 想知道依赖顺序与「鸡生蛋」| 四条破环规则 + ISO C 边界 + 沙箱 + 六步计划 |
| [`SYNTAX.md`](SYNTAX.md) | 想知道「名字该怎么起」 | 命名规范 + 样板 |
| [`REVIEW-gomoku-sample.md`](REVIEW-gomoku-sample.md) | 想知道「真实程序需要什么」 | 五子棋示例代码审读 |
| [`MIGRATION.md`](MIGRATION.md) | 想知道「编译器自己哪些代码该用 extC 写」 | 搬迁审计 + 两阶段构建机制 |
| [`extC-overview.md`](extC-overview.md) | **想一次看完「这语言是什么」** | 259 行总览（2026-09-21 写，给外人看的；**计数已核对** ✓）|
| [`VISION.md`](VISION.md) | — | ⚠️ **历史快照，已被 `LANGUAGE.md` 取代**（冲突时以 `LANGUAGE.md` 为准）|
| `bench/oi/*/REPORT.md` · `FINDINGS.md` | 想看性能实测 | 四语言横评（OI 三维偏序 / 主席树 P3834）的报告与发现 |

### 口径：谁的"状态"说了算（**冲突时按这个顺序**）

> ⚠️ 这一节是 2026-09-23 加的，原因是**同一个事实在 6 份文档里各写一遍 6 个版本** ✗
> 全量对账时实测出**十几处**「实现早就做完了，文档那句还写着没做」
> （典型：`BOOTSTRAP` 的「三处算术 UB 没堵」其实**三处全堵了** ·
> `ARRAYS`「还没有 arena」其实**早就按块细化了**）✗

| 想知道的 | **只看这一份** | 别的文档里的同一件事 |
|---|---|---|
| **现在什么坏了 / 还剩什么** | ⭐ [`PLAN.md`](PLAN.md) §0.4 | 一律作废，包括本 README 的「状态」节 |
| **某特性到底实现了没** | ⭐ 实测（`examples/` 里跑一遍）或 `MANUAL.md` | `LANGUAGE.md` 的 ✅/🔸/⬜ 标记**只是设计口径**，不是实现状态 |
| **名字怎么起 / 这语法怎么写** | [`MANUAL.md`](MANUAL.md) | — |
| **为什么这么设计** | [`LANGUAGE.md`](LANGUAGE.md)（设计口径）· [`DESIGN.md`](DESIGN.md) | — |
| **哪条拍板了** | [`DECISIONS.md`](DECISIONS.md)（编号最全） | 各文档里的"定案 NN"都指它 |
| **发生过什么** | [`DEVLOG.md`](DEVLOG.md) | 它里面的**旧计数是历史台阶，故意不改** ✓ |
| **某专题的设计全貌** | 该专题文档（`ARENA*`/`REFS`/`ARRAYS`/`IO`/`MODULES`/`LIBS`）| 专题文档的"实施状态"表**可能滞后** ⇒ 以 §0.4 为准 |

**两条与"文档可信度"有关的硬规矩**：

1. **专题文档里的"状态/进度"表最容易过时** —— 所以它们**必须指向 §0.4**
   （已经这么改了：`ARRAYS`/`IO`/`BOOTSTRAP`/`DESIGN`/`REFS` ✓）。
2. **`DEVLOG` 与历史段落里的旧数字不要"顺手改新"** —— 那是当时的真实台阶；
   改掉它，"数字为什么对不上"这件事就永远学不到了 ✗
   （但**当前状态**那类句子必须改 ⇒ 判断标准：**这句话是在描述"当时"还是"现在"** ✓）

### 维护规矩

1. **`MANUAL.md` 只说已实现的** —— 已定案但没实现的统一放第 10 节。
   *一份说谎的手册比没有手册更坏。*
2. **`DECISIONS.md` 分「已定案 / 还欠着」两栏**，定了就搬过去，不留在原地。
3. **`DEVLOG.md` 每完成一块追加一条** —— 记「做了什么、为什么、发现了什么」，
   不是 diff（那个 git 里有）。
4. **`PLAN.md` 随阶段重写**，不留历史（历史在 DEVLOG）。
5. **每次实现完，立刻给出一段能跑的示例代码** —— 说明「现在新支持了什么写法」，
   并把它加进 `examples/`。
   *语言的能力由能跑的代码证明，不由文档里的形容词证明。*
6. **能在 extC 里写的东西，就在 extC 里写**（见 `MIGRATION.md`）。
   验收方式很机械：
   `grep -inE 'struct slice|slice_[a-z]' src/*.c src/*.h` 应该**一无所获** ——
   容器的结构和方法只能活在 `stdlib/prelude.extc` 里。*不靠自觉，靠 grep。*
7. **编译器的输出一律英文**（`error:` / `note:` / `--help` / 生成代码的头注释）。
   文档和代码注释可以是中文，但**用户看到的东西**统一英文 ——
   一个程序两种语言很怪。


## 状态

**week-1 的地基已经铺完，并在往"能跟人下棋"推**。**arena + 逃逸检查都在了** ——
按块细化的 arena（每个 `{}` 一只）、四条逃逸检查、逃逸提升、块级提升都**已落地**，
常设验收是 `tests/arena/`（150MB 上限下循环 300×1MB 不涨）和 `tests/asan/`（8 个形状干净）✓

最近四块（都在 `DEVLOG.md` 里）：**模块系统 v1**（定案 70）· **泛型自由函数**（定案 71）·
**`extern!` + 信任声明**（定案 72）· **IO 第一块：能从 stdin 读了**（定案 73：`std::sys` 原语 + `std::io` 库）✓

`./check.sh` **14 节全绿**：**251** 测试 · ASan 8 · arena 5 · 攻击库基线 · 模块 8 ·
泛型 4 · extern 4 · IO 3 · 漂移哨兵 92 语料零漂移 · golden 逐字节不变 ✓

⚠️ **还没到"能跟人下五子棋"**：差 **IO-1**（`open` + 帧拥有文件 + `main(args)`）·
`allocSlice<T>(n)` · `nextInt` 一族 + `reader`。缺口表见 [`PLAN.md`](PLAN.md) §0.4 / §1 ✓
（这一节以前写着「arena / 逃逸检查还没接上」—— 那是 2026-09-18 的话，之后落后了两轮 ✗）

## 构建

```sh
make              # 产出 build/extc
make clean
```

只用 C11 标准库，无第三方依赖。要求 `-Wall -Wextra -Wpedantic` 零告警。

## 用法

```sh
./build/extc --run examples/hello.extc     # 生成 C、编译、直接运行（用到 ./build/）
./build/extc examples/hello.extc           # 把生成的 C 打到 stdout
./build/extc -o /tmp/hello.c examples/hello.extc
./build/extc --check-c examples/hello.extc # 生成 C 后再过一遍 `cc -fsyntax-only`
./build/extc -O3 -march=native --run examples/hello.extc
./build/extc --dump-tokens examples/hello.extc
./build/extc --no-line-map examples/hello.extc
./build/extc --help                        # 全部开关
```

**装到 PATH 里**（可选，`build/extc` 是自带的：prelude 已经嵌进二进制，拷到哪都能跑）✓

```sh
ln -sfn "$PWD/build/extc" ~/.local/bin/extc     # 之后任何目录直接 `extc foo.extc`
```

⚠️ 两个要记住的点：

- **`--run` / `--check-c` 用 `$CC`**（默认 `cc`，即系统 gcc）编生成出来的 C；
  `--run` 会在**当前目录**建 `build/<名字>.c` 和 `build/<名字>` ✓
- `-march=native` 能白拿 2~4×（矩阵乘那种），代价是**牺牲可移植性** ⇒ 默认不开 ✓

## 测试

```sh
./tests/run.sh     # 正例跑通 + 反例必须被编译期挡掉
./check.sh         # 全套：上面这些 + ASan + 攻击库 + 基准（一条命令跑完）
```

正例跑通 + 反例必须被编译期挡掉（当前 **251 通过 / 0 失败**）✓

## 支持的语法

```extc
type status = | ok | warn | error        // 枚举（无载荷）；变体用 status.ok 访问

struct point {
    x: i32
    y: i32

    fn moveBy(self: ref point, dx: i32, dy: i32) {   // 方法写在 struct 体内
        self.x = self.x + dx
    }

    fn magnitudeSquared(self: ref point) -> i32 {
        return self.x * self.x + self.y * self.y
    }
}

fn origin() -> point {
    return { x: 0, y: 0 }                // 裸 {} 从返回类型推导
}

fn addTo(p: ref point, dx: i32) {
    p.moveBy(dx, 0)                      // 方法接收者自动取地址
}

fn main() -> i32 {
    var p: point = { x: 1, y: 2 }
    addTo(ref p, 3)                      // 自由函数的引用实参要写 ref
    let q: point = {}                    // 零初始化
    var s: status                        // 零初始化：第一个变体
    println(p.magnitudeSquared())
    println(s)                           // ok —— 枚举自动有名字文本
    return 0
}
```

`i8..i64 / u8..u64 / f32 / f64 / bool`、`let`/`var`（含零初始化）、
**位运算 `& | ^ ~ << >>`（优先级跟 C 一致）**、
`if`/`else if`/`else`、`while`、`return`/`break`/`continue`、
`struct` + 方法（写在体内）、`type` 枚举、**泛型 `struct Name<T>`（单态化）**、**`==` 由用户显式定义 `fn ==`**、**索引 `a[i]`**、
**固定数组 `[N]T`（多维、字面量、`...` 补零、越界 trap、值语义）**、
**切片视图 `a[lo..hi]`（四种写法、编译期能证明的零检查、元素可写）**、
**`option<T>` / `result<T,E>`（prelude 里用 extC 写的）+ `?`**、
**`::` 关联函数（`option<i64>::some(3)`）**、
`ref` 表达式、`print`/`println`。

**类型系统**：只自动做**无损失**的拓宽；收窄一律禁止；字面量按值适配；
`==` 需要类型自己**显式定义 `fn ==`**（不引入 trait，也不用约定名）；
数组的 `==` 和 `println` 由**编译器递归生成**。

**还没有**：`for` · lambda · 格式串 ·
**输入的原语/库还没铺完**（`nextInt` 一族 · `reader` · `readAll` · `open`/帧拥有文件 · `main(args)`）·
`region` · `@recursive` · 协议补全（`fn <` / hash）。

> ⚠️ 这一行以前写作「…`match`、…输入/argv、模块系统、全局变量…**逃逸检查**」——
> 那里面 **`match` / 模块系统 / 全局变量 / 逃逸检查** 四样**早就实现并已验收**了
> （逃逸检查更是 `LANGUAGE.md` §4.3 明写的四条规则），只有输入那一族是真的还欠着 ✗
> 老读者按旧口径会以为这门语言还没有 arena —— 那正是它最核心的东西 ✓

## 目录

```
src/          C 实现的编译器（正史）—— 已按 pass 拆分（2026-09-21 拆分，为 arena 的"丙"腾地方）
  base.[ch]        arena / Buf / Vec / Ctx —— 写它的规矩就是 extC 要强制的规矩
  lexer.[ch]       词法
  ast.[ch]         AST
  parser.[ch]      递归下降
  types.[ch]       类型层（ttArray / ttViewMut / ttSubstitute / ttRender …）
  check*.c         检查器，按职责分：check_top / check_stmt / check_expr / check_lookup
                   / check_escape（逃逸与深度）/ check_internal.h
  codegen.[ch]     C 代码生成（带 #line 映射）
  modules.[ch]     模块系统（`use std::io`）
  prelude.[ch]     prelude 注入
  main.c           驱动
stdlib/       prelude.extc（内嵌进二进制）+ std/sys.extc（特权层：唯一写 extern! 的地方）
              + std/io.extc（普通库，用 extC 写）
examples/     样例（按特性分类；每个特性都要有一个能跑的）
tests/        回归：正例（含 // expect: 断言）/ 反例 / traps / arena / asan / warnings
              / modules / generics / extern / io / attacks（攻击库 + BASELINE）
tools/        embed.c（把 stdlib 嵌成 C 字节数组）· golden.sh（生成 C 的逐字节金标准）
              · print-desc.c / print-formats.txt
prototype-python/   作废的 Python 草稿，只作语法参考
```

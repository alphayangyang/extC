# extC

一门编译到 C 的语言。用编译期检查替代运行时机制。

> **一句话**：凡是编译期能证明的，运行时不留痕迹。

## 文档

| 文档 | 给谁看 | 内容 |
|---|---|---|
| [`LANGUAGE.md`](LANGUAGE.md) | **想知道「extC 是什么」** | ⭐ **正式设计文档**：定位、原则、语言表面、内存模型、代价与损失、当前状态 |
| [`MANUAL.md`](MANUAL.md) | **要写 extC 程序的人** | 语言手册。**只描述已实现的东西** |
| [`DESIGN.md`](DESIGN.md) | 想知道「为什么这么设计」 | 一条原则 + 全部推论 + 判出局清单 |
| [`DECISIONS.md`](DECISIONS.md) | 想知道「哪条定了、哪条还欠着」 | 语法决策的「已定案 / 还欠着」两栏 |
| [`LIBS.md`](LIBS.md) | 想知道「库怎么做」| **编译到 C 意味着什么**（C 当 ABI / 两种消费者 / 跨边界签字）+ 四种分发模型 |
| [`MODULES.md`](MODULES.md) | 想知道「模块系统怎么做」| **调研 + 候选方案**（12 门语言怎么做、extC 的六个待拍板问题）|
| [`PLAN.md`](PLAN.md) | 想知道「接下来做什么」 | 当前阶段的任务与验收标准 |
| [`DEVLOG.md`](DEVLOG.md) | 想知道「发生过什么」 | 开发记录：决策与发现的来龙去脉 |
| [`SYNTAX.md`](SYNTAX.md) | 想知道「名字该怎么起」 | 命名规范 + 样板 |
| [`REVIEW-gomoku-sample.md`](REVIEW-gomoku-sample.md) | 想知道「真实程序需要什么」 | 五子棋示例代码审读 |
| [`MIGRATION.md`](MIGRATION.md) | 想知道「编译器自己哪些代码该用 extC 写」 | 搬迁审计 + 两阶段构建机制 |

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

**week-1 进行中**：T1（类型检查独立成 pass）、T2（真类型检查）、T3（`ref` 表达式）
已完成，外加零初始化 / 方法进 struct / `type` 枚举。

⚠️ 但 **arena / 逃逸检查还没接上** —— 现在它是一门「语法像 Go 的普通语言」，还不是 extC。
逃逸检查才是 extC 的命。缺口表见 [`DECISIONS.md`](DECISIONS.md) 和 [`PLAN.md`](PLAN.md)。

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

正例跑通 + 反例必须被编译期挡掉（当前 **244 通过 / 0 失败**）✓

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

**还没有**：动态数组 `array<T>`（等 arena）、`match`、`for`、格式串、输入/argv、
模块系统、全局变量、`region`、`@recursive`、**逃逸检查**。

## 目录

```
src/          C 实现的编译器（正史）
  base.[ch]     arena / Buf / Vec / Ctx —— 写它的规矩就是 extC 要强制的规矩
  lexer.[ch]    词法
  ast.[ch]      AST
  parser.[ch]   递归下降
  codegen.[ch]  C 代码生成（带 #line 映射）
  main.c        驱动
examples/     样例（tour 是语言巡礼；其余按特性分类）
stdlib/       prelude.extc —— 用 extC 写的预lude（T4b 会把 slice<T> 放这里）
tools/        embed.c —— 把 stdlib/*.extc 嵌成 C 字节数组（C 写的，无解释器依赖）
tests/        回归测试（正例 + 反例）
prototype-python/   作废的 Python 草稿，只作语法参考
```

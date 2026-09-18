# extC

一门编译到 C 的语言。用编译期检查替代运行时机制。

> **一句话**：凡是编译期能证明的，运行时不留痕迹。

## 文档

| 文档 | 给谁看 | 内容 |
|---|---|---|
| [`MANUAL.md`](MANUAL.md) | **要写 extC 程序的人** | 语言手册。**只描述已实现的东西** |
| [`DESIGN.md`](DESIGN.md) | 想知道「为什么这么设计」 | 一条原则 + 全部推论 + 判出局清单 |
| [`DECISIONS.md`](DECISIONS.md) | 想知道「哪条定了、哪条还欠着」 | 语法决策的「已定案 / 还欠着」两栏 |
| [`PLAN.md`](PLAN.md) | 想知道「接下来做什么」 | 当前阶段的任务与验收标准 |
| [`DEVLOG.md`](DEVLOG.md) | 想知道「发生过什么」 | 开发记录：决策与发现的来龙去脉 |
| [`SYNTAX.md`](SYNTAX.md) | 想知道「名字该怎么起」 | 命名规范 + 样板 |
| [`REVIEW-gomoku-sample.md`](REVIEW-gomoku-sample.md) | 想知道「真实程序需要什么」 | 五子棋示例代码审读 |

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


## 状态

**week-0**：链路通了（extC → C → gcc → 可执行），但 **arena / 逃逸检查还没接上** ——
现在它是一门「语法像 Go 的普通语言」，不是 extC。见 `DECISIONS.md` 末尾的缺口表。

## 构建

```sh
make              # 产出 build/extc
make clean
```

只用 C11 标准库，无第三方依赖。要求 `-Wall -Wextra -Wpedantic` 零告警。

## 用法

```sh
./build/extc --run examples/hello.extc     # 生成 C、编译、直接运行
./build/extc examples/hello.extc           # 把生成的 C 打到 stdout
./build/extc -o /tmp/hello.c examples/hello.extc
./build/extc --dump-tokens examples/hello.extc
./build/extc --no-line-map examples/hello.extc
```

## 测试

```sh
./tests/run.sh
```

正例跑通 + 反例必须被编译期挡掉。目前 10/10。

## 支持的语法（week-0）

```extc
struct Point {
    x: i32
    y: i32
}

fn moveBy(self: ref Point, dx: i32, dy: i32) {
    self.x = self.x + dx
}

fn origin() -> Point {
    return { x: 0, y: 0 }
}

fn main() -> i32 {
    var p: Point = { x: 1, y: 2 }
    p.moveBy(3, 4)              // a.f(x) 就是 f(a, x) 的糖
    let q: Point = {}           // 裸 {} 从声明类型推导
    println(p.x)
    return 0
}
```

`i8..i64 / u8..u64 / f32 / f64 / bool / str`、`let`/`var`、`if`/`else if`/`else`、
`while`、`return`/`break`/`continue`、`struct` + 方法、函数调用、`print`/`println`。

**还没有**：数组、`Option`/`Result`/`?`、`enum`/`match`、`for`、`region`、`@recursive`、
顶层全局变量、逃逸检查。

## 目录

```
src/          C 实现的编译器（正史）
  base.[ch]     arena / Buf / Vec / Ctx —— 写它的规矩就是 extC 要强制的规矩
  lexer.[ch]    词法
  ast.[ch]      AST
  parser.[ch]   递归下降
  codegen.[ch]  C 代码生成（带 #line 映射）
  main.c        驱动
examples/     样例
tests/        回归测试（正例 + 反例）
prototype-python/   作废的 Python 草稿，只作语法参考
```

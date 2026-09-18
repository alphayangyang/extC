# extC

一门编译到 C 的语言。用编译期检查替代运行时机制。

> **一句话**：凡是编译期能证明的，运行时不留痕迹。

设计见 [`DESIGN.md`](DESIGN.md)（一条原则 + 全部推论 + 判出局清单）。
命名规范见 [`SYNTAX.md`](SYNTAX.md)。
week-0 里奶昔自己拍的语法决策见 [`DECISIONS.md`](DECISIONS.md)。

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

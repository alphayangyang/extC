# 搬迁审计：编译器里哪些东西该用 extC 写

> **原则（主人 2026-09-18 定）：能在 extC 里写的东西，就在 extC 里写。**（除非很难 / 很影响效率）
>
> 本文回答：**现在编译器里，哪些代码适合搬到 extC？**

---

## 0. 机制（✅ 已落地，见 T4b）

> **现状**：`stdlib/prelude.extc` 里的 `slice<T>` 已经能用了。
> `grep -in slice src/*.c src/*.h` → **一无所获**。
> 编译器有两张表：一张是它自己的 C 代码，一张是它自带的 extC prelude。


**两阶段构建（部分自举）：**

```
① 用纯 C 源码构建 extc                 ← 现在的状态
② 用 ① 编出来的 extc 编译 stdlib/*.extc → build/stdlib.c
③ 把 build/stdlib.c 一起链进 extc        ← 最后这个 extc 的一部分是 extC 写的
```

于是最终编译器的一部分**已经是用 extC 写的**，而且这是自举的直通路（终局是整个编译器变成 extC 源码）。

**附带好处（可能是最大的）**：stdlib 每改一次，都被「**用 extC 编 extC**」这个循环检验一遍。
这比任何单元测试都值钱 —— extC 编译自己标准库时出的任何毛病，都是真问题。

**风险**：编译器依赖 stdlib，stdlib 有 bug 编译器就崩。
→ 保留一个 C fallback（编译期开关，stdlib 崩了就回退到 C 版实现）。

---

## 1. 审计结果

✅ 适合搬 ／ 🟡 部分搬 ／ ❌ 留

| 文件 | 内容 | 判定 | 卡在哪 |
|---|---|---|---|
| `base.c` | `Arena`（bump 分配器） | ❌ | 内存模型**本身** = 运行时 |
| `base.c` | `Buf`（可增长字节缓冲） | ✅ | 要 `array<u8>` / `string<T>` |
| `base.c` | `Vec`（定长元素动态数组） | ✅ | 要泛型 `array<T>` |
| `base.c` | `ctxInit` / `ctxError` | ❌ | 编译器状态的结构形状 |
| `base.c` | **`ctxRenderDiag`（诊断渲染，40 行）** | ✅ **首选** | 要 `slice<u8>` + 索引 + 拼接 |
| `lexer.c` | `isDigit/isHex/isAlpha/isAlnum/isUpperCase` | ✅ | 要字符索引 `s[i]` |
| `lexer.c` | 关键字表 / 内建类型表 / 标点表 | ✅ | 要数组/切片常量 |
| `lexer.c` | **整个词法器（状态机 + 四个 lex\*）** | ✅ **自举第一块砖** | 要字符串扫描 + 数组 |
| `lexer.c` | `tokenKindName` / `tokenDescribe` | ✅ | 要字符串拼接 |
| `types.c` | 整数表 + `ttIsInteger/Float/Numeric/IntBits/IntSigned` | ✅ | **要字符串相等**（现在没有！） |
| `types.c` | **`ttCanWiden`（拓宽规则）** | ✅ **最纯的一块** | 同上 |
| `types.c` | `ttEquals` / `ttBase` / `ttIsError` | ✅ | 要 tag 检查（`match` 更好） |
| `types.c` | `ttRender`（类型渲染） | ✅ | 要 `Buf` |
| `types.c` | `ttFromName` / `ttNew` | 🟡 | 要容器 + 分配 |
| `check.c` | `lookup` / `findFunc` / `findField` / `findMethod` / `findVariant` | ✅ | 要 `map<K,V>` 或数组（它们都是线性查找） |
| `check.c` | 作用域管理 / AST 遍历 / 各条检查 | ❌ | 编译器逻辑骨架（终局是自举，不是搬迁） |
| `codegen.c` | `C_TYPES` / `PRINT_FMT` 表 | ✅ | 要数组/切片常量 |
| `codegen.c` | `cType` / `cFuncName` / `zeroValue` / `genPrintValue` | ✅ | 纯字符串生成，要 `Buf` |
| `codegen.c` | `genExpr` / `genStmt` / `genFunc` 骨架 | 🟡 | 拼接部分可搬，遍历部分留 |
| `main.c` | `readFile` / `writeFile` / `runCmd` | ❌ | OS 交互（文件/进程）→ 逃生舱范畴 |
| `main.c` | `baseName` / `usage` | ✅ | 纯字符串处理 |

**约 15 处适合搬，3 处是首选目标。**

---

## 2. ⚠️ 硬门槛：现在几乎什么都搬不了

现在的 extC 有：`i32..u64 / f32 / f64 / bool / str / void`、`struct` + 方法、`type` 枚举、
`let`/`var`、`if`/`while`、`fn`、`ref`、零初始化、自动调试打印。

**没有：数组、索引、字符串操作、泛型、`option`、`match`。**

对着上面的表看：

| 想搬什么 | 缺什么 |
|---|---|
| `ttCanWiden` + 整数表 | **字符串相等**（要比较类型名） |
| 字符分类函数 | **`s[i]` 索引** |
| `ctxRenderDiag` | **字符串拼接 + 切片** |
| `Buf` / `Vec` | **泛型 + 分配** |
| 词法器 | 上面全部 |

> **结论：在补齐「数组 + 字符串操作 + 泛型」之前，编译器里几乎没有东西搬得过去。**
>
> 所以 T4 不只是为了写五子棋 —— **它同时是「把编译器自己搬进 extC」的开门钥匙。**

---

## 3. ⚠️ 审计中发现的 bug：`str == str` 是指针比较

```
if a == b { ... }        // a、b 都是 str
```
生成的 C 是：
```c
const char * a = "hi";
const char * b = "hi";
if ((a == b)) { ... }    // ← 这是**指针**比较
```

它之所以「看起来对」，只是因为 gcc 把相同内容的字面量折叠到了同一地址（默认行为）。
一旦字符串能从别的地方来（参数、拼接、文件），会立刻判不等 —— **而且错得无声无息**。

**处置（2026-09-18）**：现在 `str` 的 `==` / `!=` **直接报错**，而不是安静地给错答案。

> 与其让它假装能比较，不如让它诚实地不能。等 T4c 把字符串换成 `slice<u8>`
> 之后再给一个**按内容比较**的正确实现。
>
> 这也是一条通用原则：**语言层承诺的语义，生成的 C 必须真的实现 ——
> 让 C 的默认行为偷偷替代，就制造了一个未来的陷阱。**

---

## 4. 建议的搬迁顺序

| # | 搬什么 | 为什么先搬它 |
|---|---|---|
| 1 | **`ctxRenderDiag`（诊断渲染，40 行）** | 纯字符串处理、无副作用、无外部状态；输入输出明确好测；**收益立刻可见**（它就是当前显示错误的那条路径）；是 extC 字符串库的第一个真实用户 |
| 2 | **`ttCanWiden` + 整数表** | 最纯的一块规则，零 IO、零分配 |
| 3 | **词法器** | 自举的第一块砖 |
| 4 | `Buf` / `Vec` → `array<T>` | 库化 |
| 5 | `lookup` / `find*` → `map<K,V>` | 最后（最依赖容器） |

**第 1 件要等 T4c（`slice<u8>`），第 2 件要等字符串相等，第 3 件要等上面全部。**

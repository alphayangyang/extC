# `global`：函数内的持久存储（定案）

> **状态：已实现（2026-10-04）。** 2026-10-04 提出并定名，起因是写 `examples/wss-echo.extc` 时的实际痛点：
> 一个 60 行的示范被迫写四个**模块级**全局（`SESS`/`FD`/`RESULT`/缓冲），而它们本该是
> "这个函数自己的持久暂存"。实现要**成套**做（见下），半套会静默降级成普通局部 —— 比没有更坏。

## 一句话

**`global` = "存储是全局的，名字写在这里"。** 它与顶层 `var`/`let` 是**同一种东西**（深度 0、
活到进程结束），区别只是**声明的位置**：顶层那套已经有，`global` 让它在函数里也能写。

## 落地记录（2026-10-04）

| 层 | 改动 |
|---|---|
| parser | `global` 做**软关键字**（`accept(p,"global")` 后再看 `let`/`var`），**不动词法表** —— 把它变保留字会破坏现有代码里拿它当名字的地方 |
| AST | `ST_VAR.isGlobal`（**语法**事实，解析器写 ✓） |
| checker | 初始化式过 `isConstInit`；绑定置 `outOfFrame` / 深度 0（`pMarkGlobal`，**两条声明路径都调** —— 零初始化那条会提前返回，只改一条等于没改，实测撞过） |
| plan | `NodeResults.varInitOO` + `planSetVarInitOutOfFrame` / `planVarInitOutOfFrame`（**分析结论**的正规住处） |
| codegen | 声明处发 C 的**块作用域 `static`**；协程帧布局**跳过** `global`（`coroFrameLay`） |

**AST 冻结闸门抓过我一次，而且抓得对**：第一版把"初始化式是不是帧外存储"（**检查器的结论**）
写成了 `ST_VAR.initOutOfFrame` ✗ —— 闸门的规矩是"分析结果进 plan 侧表（`planSet*`），AST 只装语法"。
我一度想用 `--update-baseline` 让它过去，那是**绕开**；正确做法是把它放回 plan（上面的 `varInitOO`）。
另外两处新违规是文本扫描的误报（`d->isGlobal = s->u.var.isGlobal;` 看着像 AST 写）—— 修法不是放宽
扫描器（按名字排除会让它**真的**看不见 AST 写），而是 `CoroDecl` **只存语句指针**、用到时现读 ✓
更少的重复来源。**能遵守就遵守；规则不合逻辑才改规则。**

## 语法与语义

```extc
fn exchange(...) -> coroutine<i64> {
    global var rx: [4096]u8                              /* 零初始化；一处声明一个实例 */
    global let key: slice<u8> = "dGhlIHNhbXBsZSBub25jZQ=="  /* 常量初始化 ⇒ 字面量也持久 ✓ */
    ...
    yield FD        /* 视图指向 global ⇒ 跨 `yield` 合法 ✓（这是整个特性的目的） */
}
```

三条规则，都能检查：

| 规则 | 依据 |
|---|---|
| 初始化式必须是**常量** | C 的静态初始化限制；直接复用 `isConstInit`（`src/check/check_top.c:4366`）—— 它现在是 `static`，要导出 |
| 它是**深度 0**（本帧之外）⇒ 视图/引用可跨 `yield`、可存进别处 | 检查器已有这一格：`0 = a parameter or a global (outside this frame)`（`check_top.c:1541` 一带） |
| **一处声明 = 一个实例**，跨调用、跨协程共享 | C 的语义；关键字本身把这个危险点名了 |

**为什么叫 `global` 而不是 `static`**（讨论过，记下来免得反复）：

* 关键字应当命名**检查器真正在管的那条性质**。这里管的是**生命周期/深度**（深度 0），
  而 C 的 `static` 混了三种互不相干的意思（静态存储期 / 内部链接 / 静态函数）；
* extC 的既有词汇已经定死了这个意思：`check_top.c` 的原话就是 *"A global is depth 0"* ——
  `global` 与文档、诊断、`slotDepth` 的术语**同一套词**；
* `static` 在 C 用户心里会联想到"文件内可见"，而我们这里恰恰相反：**存储是全局的、名字是块作用域的**。

**顶层也允许写 `global`**（冗余但自文档化）：顶层 `var x: T` ≡ `global var x: T` ✓。

**与 Python 的 `global` 不是一回事**（词是借的，意思不是 —— 从 Python 过来的人会被带偏，所以写在这）：

| | Python `global` | 我们的 `global` |
|---|---|---|
| 管什么 | **名字解析**：让函数里这个名字指向**已经存在**的模块级绑定 | **存储寿命 + 名字作用域**：存储活到进程结束，**名字只在块里** |
| 能在函数里"造"出存储吗 | 不能（只是引用模块命名空间，名字随即模块可见） | **能**，就是在函数里造，且名字**不**外泄 |
| 语义上最接近的 | —— | **C 的块作用域 `static`**（Swift 的函数内 `static let` 同套） |

也就是说：Python 的 `global` 是"**指向外面**"，我们的 `global` 是"**在这里放一块活到最后的东西**"。

## 实现落点（四处，成套）

1. **`src/front/parser.c`** —— 声明语句接受 `global` 前缀（`parseStmt` 的 `var`/`let` 路径，
   `ST_VAR` 的构造在 2276 与 2459 一带），在 `ST_VAR` 上打一个 `isGlobal` 标记；
   顶层 `parseGlobalDecl`（2189）也接受（当作无操作）。
2. **`src/ast/ast.h`** —— `ST_VAR` 的载荷加 `bool isGlobal`（`u.var`，字段定义在 436 附近）。
3. **`src/check/check_stmt.c`** —— `ST_VAR` 分支：
   * `isGlobal` ⇒ 初始化式按 `isConstInit` 检查（要把该函数从 `check_top.c` 导出）；
   * 该名字的**深度报 0**（找到局部登记深度的那张表，按"像参数/全局那样"登记）。
4. **`src/back/codegen.c`** —— 声明处发 **C 的块作用域 `static`**（`static T name = init;`）：
   静态存储期 + 块内可见，**语义正好对上**；**唯一要额外做的是把它排除出协程帧**
   （帧的规则见 2343：*"A coroutine's parameters and its live-across-`yield` locals live in the
   frame"* —— `global` 不进帧，因为它是 C static）。

## 判据（先写判据再实现）

| 判据 | 说明 |
|---|---|
| 视图跨 `yield` ✓ | `global var buf: [N]u8` 的切片在协程体里跨 `yield` 使用，必须能编过（**整个特性的目的**） |
| 只初始化一次 | 一个带自增计数的初始化式（用 `global let` 不行，得用运行期可观察的手段）⇒ 第二次调用看得到上一次的状态 |
| **两个协程共享同一实例** | 把危险也钉成判据：两个协程跑同一个函数，看到的是同一块存储（这是 `global` 的语义，不是 bug） |
| 非常量初始化式 ✗ | `global var x: i64 = f()` 必须报错，且报错文案要指向"常量" |
| 普通函数里也合法 | 不只在协程里 |
| 顶层 `global` 冗余但合法 | 与不带关键字等价（生成物应一致 —— 可作为"同一件事"的判据） |

## 正交的一件（建议同时做）

**已完成（2026-10-04）**：字面量在静态存储里 ⇒ 本来就是深度 0。放宽做了两处，都是"把规则推广到
它本来的判据"：`exprOutOfFrame` 加 `EX_STR → true`；`coroCheckDeferred` 的放行条件从"池容器 /
本体内 `new`"推广成 **`exprOutOfFrame`**（全局 / `Ret=0` 返回 / 字面量一律放行）。
判据 `tests/coro/coro_literal.extc`（正例）+ run.sh 里的**反例**（局部数组的视图跨 `yield`
仍被拒）✓。**这一格与 `global` 共用同一套判定**（都问"存储活不活得过帧"），所以 `global`
的实现可以直接复用 `exprOutOfFrame` ✓。

## 与既有文档的冲突（要改）

`docs/manual/16-unimplemented.md` 里那句 **"`static` 关键字因此消失"** 说的是**模块级**；
函数级的持久存储当时没被考虑到。定案落地后这句要改成"顶层与 `global` 是同一种东西"。

## 扫描结果（2026-10-04，实现前的实测落点）

| 落点 | 位置 | 说明 |
|---|---|---|
| 语句级声明入口 | `src/front/parser.c:2082` | `if (at(p, "let") \|\| at(p, "var")) return parseVarDecl(p);` —— `global` 在这里做**软关键字**（`at(p,"global")` 再看下一个词），**不要动词法表**：把 `global` 变成保留字会破坏现有代码里用它当普通名字的地方 |
| 顶层声明入口 | `src/front/parser.c:762` | 同样的 `let`/`var` 判定；顶层 `global` 当作无操作（冗余但合法） |
| `ST_VAR` 的构造点 | `src/front/parser.c:2280` | `s->u.var.mut = (strcmp(kw->text, "var") == 0);` —— `isGlobal` 标记就加在这附近 |
| AST 载荷 | `src/ast/ast.h` 的 `u.var`（`mut` 字段附近，约 436 行是 `type`） | 加 `bool isGlobal` |
| 逃逸判定（**已验过的地基**） | `src/check/check_escape.c` 的 `exprOutOfFrame` | ①已经把 `EX_STR → true` 加进去；`global` 的"存储活过帧"直接复用它 ✓ |
| 检查器的绑定登记点 | `src/check/check_stmt.c:470-485` | 现有逻辑：`if (exprOutOfFrame(c, ini)) { sym->outOfFrame = true; anSetRefDepth(sym, 0); }` ⇒ `global` 要**无条件**走这一支（它的存储本来就是帧外），并且 `sy->depth = 0`（`slotDepth` 对非视图绑定读的是 `sy->depth`） |
| 常量初始化式 | `src/check/check_top.c:4366` `isConstInit` | 现为 `static` ⇒ 导出（`check_internal.h`）给 `ST_VAR` 分支用 |
| **协程帧的构建** | **还没找到** | `coroDeferred` 只用于**报错**（`check_top.c:6701`），**不是**帧的构建来源 ⇒ 下一轮第一件事：定位帧布局那一遍（`grep -n "frame" src/back/codegen.c`），确认"哪些局部进帧"，然后把 `isGlobal` 的排除掉 |

## 为什么必须一次做完

只加 parser 的话，`global var x: T` 会被**静默当成普通局部**：单任务里看起来"能用"（帧在任务存活期内一直有效），
但**跨调用/跨协程的持久性没了** —— 而这正是这个特性的全部意义。**静默错 > 没有**，所以宁可不做。

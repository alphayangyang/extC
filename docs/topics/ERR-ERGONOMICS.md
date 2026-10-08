# 失败处理：收缩 trap + 更方便的方案（2026-10-08）

> **状态：方案（未施工）。** 主线目标是"让语言用起来更顺手" ⇒ 本文针对最扎手的一格：
> **现在太容易 trap**，而 trap 是**进程级 exit(1)** ✗（不能当控制流、不能恢复）。
> 数据来源：目标场景产物 `qqbot-extc/examples/wss-echo.extc`（TLS+WS）✅实测。

## 一、先量：trap 都从哪来（TLS+WS 产物 21 处）

| 种类 | 条数 | 性质 |
|---|---|---|
| `"the view has no storage"`（空/无存储视图解引用）| 3 | **可恢复**（调用方本可判空）|
| `"value does not fit in the target type"`（窄化转换）| 2 | **可恢复**（值来自外部数据）|
| `"division by zero"` / `"integer overflow in division"` | 4 | **可恢复** |
| `"shift count out of range"` | 1 | **可恢复** |
| `"a non-void function reached its end without returning"` | 1 | **误用**（而且这是把 UB 变响 ✓ 该留）|
| 其余（下标越界等，未逐条归类）| ~10 | 多数**取决于下标从哪来** |

## 二、缺口（实测，不是印象）

| 能力 | 现状 |
|---|---|
| 容器取元素 | **有**可选式 ✓：`vector::get -> option<T>` · `hashMap::get -> ?V` · `pool::get -> ?T` · `linmap::get -> ?V` |
| **`slice`/数组取元素** | ✗ **只有会 trap 的 `xs[i]`**（stdlib 里找不到 `fn index`/`at`）|
| **窄化转换**（`i64(x)`/隐式收窄）| ✗ 没有 `asI64 -> option<T>` 一类 |
| **除法/取模/移位** | ✗ 没有 checked 形式 |
| **`?` 的错误类型** | ✗ **必须完全一致**（检查器：*"`?` forwards the failure as-is, so it cannot change the error type."*）⇒ 要么挤进一个巨型 enum ✗ 要么 `!` 炸掉 ✗ |
| `!`（解包或 trap）| ✓ 有，但**语义是"不可恢复"** ⇒ 被当成默认写法就变成"程序随时会死" ✗ |

## 三、方案：把失败**分成三类**，逐类处置

| 类 | 例子 | 处置 |
|---|---|---|
| **A 误用**（不可恢复）| null 解引用 · arena 误用 · `dyn` 陈旧 · 未返回就掉出函数体 · 用户显式 `trap()` | **保持 trap** ✓（响亮正确 ✓）|
| **B 输入相关**（可恢复）| 下标来自外部数据 · 窄化溢出 · 除零/移位 | **提供"不 trap 的形式"**，并让默认写法更容易拿到它 ✓ |
| **C 环境**（可恢复）| I/O · 网络 · 解析 | 已经是 `result`/`option` ✓（fs/io/http/dl ✓）—— **保持**，只补一致性 ✓ |

### S1（立刻可做，零语言改动）：补齐 B 类的 checked 形式

```extc
slice<T>::at(i) -> option<T>          /* 与 vector::get 命名/语义一致 ✓ */
array_at(...)                         /* 定长数组同理 */
asI64(v) / asU32(v) … -> option<T>    /* 窄化：不 trap，溢出给 none ✓ */
divChecked(a, b) / remChecked(a, b) -> option<i64>
shiftChecked(v, k) -> option<i64>
```
判据：每个 API 一条"**不 trap 且返回 none**"的判据 ✓（现在的对应操作会 exit(1) ✗）。

### S2（语言改动，痛感最大）：`?` 允许**错误类型转换**

现在 `?` 只能原样转发 ⇒ 各层错误被迫统一 ✗。改法（用现有 trait 机制 ✓）：

```extc
trait IntoErr<E> { fn intoErr(self: ref Self) -> E }
/* `?` 的规则：源错误 E1 可经 impl IntoErr<E2> 变成函数的错误类型 E2 ⇒ 允许 */
```
⇒ 每层可以有**自己的**错误类型 ✓（yaml 的、sqlite 的、协议的各一套 ✓），
只在 `?` 的边界做一次转换 ✓ —— 这就是"很多异常要处理"的正解 ✓。

### S3：**隐式传播的失败**（目标形态；不写进签名、不用 effects）

**设计前提（用户 2026-10-08 拍板）**：**不要滥用 effects** —— effects 写起来极贵，
它是给"**既证明不了、也处理不了**"的边界用的 ✗。而"越界/窄化/除零"这种**天天发生**的失败
不能要求每个函数都声明 ✗。

**失败的载体 = 一个 `dyn Err` 值**（复用刚验过的 dyn 机制 ✓ 值语义 ✓ 无常注解 ✓）：

```extc
trait Err { fn message(self: ref Self) -> slice<u8>  fn code(self: ref Self) -> i64 }

/* 产生：checked 原语返回 result<T, dyn Err>（S1 ✓） */
/* 传播：`?` 往上传 —— **签名不变** ✓ 由编译器内部给"可能失败"的函数加一条失败通道 ✓ */
/* 处理：catch { e => ... } / if let err(e) = r { ... }（后者正是我们讨论过的 `if let` ✓）*/
/* 没人处理 ⇒ 到达入口 ⇒ **trap**（带 file:line + 错误消息 ✓）—— trap 退成"最后一道网" ✓ */
```

**为什么是这个形状**：
* 传播**不跳栈** ✓（编译器把失败编成**普通返回** ✓）⇒ 每层的块 arena `release`、zone `LeaveTo`
  **照常执行** ✓✓ —— 这正是 longjmp 方案过不去的坎 ✗；
* **不用 effects** ✓、**不动签名** ✓ ⇒ "写起来顺手" ✓（失败只在 *产生点* 和 *处理点* 出现 ✓）；
* "可能失败"由编译器**算不动点**得出 ✓（和 `makesPool` 同一类 ✓）⇒ 不需要人写 ✓。

**代价（诚实）**：这是**编译器级**改动 ✓ —— 每个"可能失败"的函数多一条隐藏的失败通道 ✓，
调用点要检查它 ✓。它是"顺手度"的关键一格 ✓，但**不能顺手做** ✗。

**明确放弃 `longjmp`/真异常** ✗：跳过块 arena 清理 ✗（除非把块 arena 也登记进 zone 栈 ✓ ——
那是另一条路，记录为设想，不排期 ✓）。

**fatal 的仍然直接 trap** ✓（A 类：null 解引用 · arena 误用 · `dyn` 陈旧 · 掉出函数体 · 用户 `trap()`）
—— 它们是"**检查器的承诺被违反**" ✓，此时响亮退出才是对的 ✓。

### 施工顺序（据此修正）

| 序 | 事 | 说明 |
|---|---|---|
| 1 | **S1 checked 原语**（stdlib ✓ 零语言改动）| 立刻把目标产物里 10 处可恢复 trap 变成值 ✓ |
| 2 | **`if let` 部分匹配**（语言 ✓ ~200 行）| 处理 `option`/`result` 不再被迫写全 `match` ✓ 也是 S3 处理点的语法 ✓ |
| 3 | **S2 `?` 允许错误转换**（trait ✓）| 各层错误不必挤进一个巨型 enum ✓ |
| 4 | **S3 隐式传播**（编译器 ✓ 大）| 目标形态："不处理 ⇒ 到入口 trap" ✓ |

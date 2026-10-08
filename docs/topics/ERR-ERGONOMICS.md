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

### S3（设想，不建议现在做）：`recover { }` 恢复边界

让 A 类 trap 在**显式边界**内变成 `result` ✓。代价大 ✗：trap 必须"可展开"，
而 arena/zone 的清理语义要一起想清楚（`extc_die` 现在是 `exit(1)` ✓ 且有 `__extc_dying` 钩子 ✓
—— 那是个起点，但不是恢复机制 ✗）。**记录为设想** ✓ 不排期 ✓。

## 四、验收：把"太容易 trap"变成**可测的数字**

* **指标**：目标产物（TLS+WS）里，**B 类 trap 数 = 0** ✓（现在：窄化 2 + 除零 4 + 移位 1 + 空视图 3 = **10** ✓）；
* 剩下的 trap 应当只有 **A 类误用** ✓（null/arena/dyn 陈旧/未返回 ✓）；
* 每条 checked API 配"不 trap"判据 ✓ + 一条**统计判据**（扫产物里的 trap 消息，B 类关键词必须不出现 ✓）。

## 五、与其它主线的顺序

| 序 | 事 | 理由 |
|---|---|---|
| 1 | **S1 checked 形式**（stdlib ✓）| 零语言风险、立刻把 10 处可恢复 trap 变成值 ✓ |
| 2 | **S2 `?` 异质错误**（语言 ✓）| 让"处理失败"从"写巨型 enum"变成"各层自洽" ✓ 这是顺手度的关键一格 |
| 3 | `f"…"` v2 | 已定落点 ✓（写法顺手 ✓）|
| 4 | S3 `recover` | 设想 ✓ 不排期 |

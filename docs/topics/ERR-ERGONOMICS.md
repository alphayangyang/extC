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

---

# 施工步骤（2026-10-08 定稿，等审）

## 决定记录：null 解引用为什么保持 fatal

**实测**（`?ref` 上）：`p.v` ⇒ **编译错误** ✓；`!p` ⇒ **编译错误** ✓（引用没有 `!` 后门 ✓）；
`if p == null { return }` 之后 ⇒ 编译通过且**不生成任何运行期检查** ✓（编译器原话：
*"inside that branch the compiler knows it is not null and generates no runtime check"*）。
⇒ 安全代码里**写不出** null 解引用 ✓，所以残留的检查只可能在三种情形下触发：
**① 检查器的证明是错的**（审计 P0-6/P0-13/P0-17 这类 ✓ 已修）· **② `@unchecked` 误用** · **③ 边界**。

**为什么不给 undefined**：
1. **引用给不出安全的"undefined"** ✗ —— 值中毒（NaN / SQL NULL ✓）对**数值**成立 ✓，
   对**指针**不成立 ✗（任何非零垃圾指针一解引用就是 UB ✗；要安全就得给指针加标签 = 装箱 = 变贵 ✗）；
2. **它把"证明被违反"变成静默错码** ✗✗ —— 而这类（P0-12 `1.0/2.0`→`0` · P0-19 1000→232）是审计里最重的一类 ✓；
   对跑预约的 bot：**订错房间比崩一次重启严重** ✓；
3. 项目立场：`pFmtF` 注释原文 *"与其安静地打出错的数，不如响亮"* ✓；`@unchecked` 的语义是
   "**不插检查**"（裸 C 解引用 ✓ 后果自负但**看得见** ✓）而不是"给假值" ✗。

**分界线（据此定案）**：**数值类失败 ⇒ 值（`option`）✓；引用可空性 ⇒ 值（`?ref` + `?`/`??`/`if let`）✓；
"证明被违反" ⇒ fatal trap ✓；确实想不查 ⇒ 显式 `@unchecked` ✓**。一句话：**安全 = 值；不安全 = 显式** ✓。

## S1 —— checked 原语（**纯 stdlib，零语言改动**）

**落点**：新建 `stdlib/std/slice.extc`（切片/数组取值 ✓）与 `stdlib/std/num.extc`（数值 checked ✓）
—— 不要塞进 `mem.extc`/`io.extc`（它们已经很大 ✓）。

**API（判据先写，每个一条）**：

| API | 语义 |
|---|---|
| `slice::at(i) -> option<T>` · `slice::atMut(i) -> ?mut ref T` | 越界 ⇒ `none` ✓（与 `stl::vector::get` 同名同义 ✓）|
| `asI64(v: f64) -> option<i64>` · `asU32(v: i64) -> option<u32>` … | 放不下 ⇒ `none` ✓（**不 trap** ✓）|
| `divChecked(a, b) -> option<i64>` · `remChecked` · `shiftChecked(v, k)` | 除零/溢出/移位越界 ⇒ `none` ✓ |

**判据**：`tests/err/` 下每个 API 一条（正常值 ✓ · 边界值 ✓ · 越界/溢出 ⇒ `none` 且**进程不死** ✓）。
**验收（数字）**：把 `qqbot-extc/examples/wss-echo.extc` 里那 10 处可恢复 trap 改成值 ⇒
**目标产物 B 类 trap = 0** ✓（见 S1b 的统计工具 ✓）。
**风险**：极低（纯新增 ✓ 不改既有语义 ✓）；回退 = 删文件 ✓。
**指纹预期**：**414 份 0 变化** ✓（语料不用新 API ✓）。

## S1b —— 让"太容易 trap"变成 CI 上的数字

`tools/trap_audit.py`：扫产物里的 trap 消息，按 **A（误用）/ B（输入相关）** 分类计数 ✓，
配一条**棘轮**（B 类不许增长 ✓，风格照 `tools/check_switches.py` ✓）。
**验收**：目标产物 B 类 **= 0** ✓；全语料 B 类不增长 ✓。

## S2 —— `if let` 部分匹配（语言，~200 行）

**落点**（关键：**不新增 AST 节点** ✓ —— `case EX_` 有 ~462 处，漏一处就是静默错 ✗）：
解析器在 `parseIf` 里见 `let` ⇒ 复用现有 match 臂解析造**单臂 `ST_MATCH`** ✓ +
一个解析器写的 `partial` 标志 ✓；检查器看到该标志 ⇒ **跳过穷尽性检查** ✓（其余复用臂绑定 ✓）；
代码生成**不用改** ✓（单臂 match 本来就是 if 链 ✓）。
**判据**：正例（绑定 ✓ · `else` ✓ · **部分匹配合法**（重点）✓）；反例（未知变体 ✓ · payload 数不符 ✓）。
**风险**：中（解析器改动 ✓ 可隔离 ✓）；回退 = 单文件 ✓。**指纹预期**：0 变化 ✓。

## S3 —— `?` 允许错误转换（语言）

**落点**：`src/check/check_escape.c:2432`（那条"错误类型必须一致"的规则 ✓）⇒ 改为：
若 `oe` 与 `re` 不同，则查找 `impl IntoErr<re> for oe` ✓（方法查找 ✓）；
**把"用哪个转换"记进 `plan` 侧表** ✓（AST 冻结 ✓ 不能改 AST ✗）；codegen 在 `?` 的降级处发那次调用 ✓。
**约定**：内建 trait `IntoErr<E>` ✓（像 `From` ✓）+ 标准库给常见组合实现 ✓。
**判据**：有转换 ⇒ 编译过且运行正确 ✓；没有 ⇒ 仍报错，且**提示**"实现 `impl IntoErr<X> for Y`" ✓。
**风险**：中（错误路径要逐条测 ✓）。

## S4 —— 隐式传播（编译器级，**单独排期，不在 S1–S3 内**）

表示：失败 = **`dyn Err` 值** ✓ + 每个"可能失败"的函数一条**隐藏失败通道** ✓
⇒ 传播走**普通返回** ✓（**不跳栈** ✓ 每层的块 arena `release`/zone `LeaveTo` 照常 ✓）。
步骤：① `mayFail` **不动点**（照 `makesPool` 的现成机器 ✓）→ ② codegen：失败产生点写通道、调用点检查 ✓ →
③ 处理点：`catch { e => … }`（或复用 `if let` ✓）→ ④ **入口兜底 trap** ✓（= 今天的 `extc_trapMsg` ✓ 但只是最后一道网 ✓）。
**必须先想清楚的交互**：协程（`yield` 中的失败要进帧 ✓）· `dyn`（vtables 已知安全 ✓ 但 `dyn Err` 的组合 ✓）· arena/zone ✓。
**判定"做到了"**：底层失败 + 入口不处理 ⇒ 到入口 trap，且**中间各层的 arena 都被释放** ✓（可测 ✓）；
处理了 ⇒ 正常继续 ✓。**量：多日** ✓。

## 顺序与验收（一览）

| 步 | 语言改动 | 验收 | 指纹预期 |
|---|---|---|---|
| S1 | 无 ✓ | 新 API 判据全绿 ✓ + 目标产物 B 类 trap = 0 ✓ | 0 变化 ✓ |
| S1b | 无 ✓ | 棘轮上线 ✓ 数字可见 ✓ | — |
| S2 | 有（小 ✓）| `if let` 正/反例 ✓ | 0 变化 ✓ |
| S3 | 有（中 ✓）| 转换有/无两条 ✓ | 仅用到转换的产物 ✓ |
| S4 | 有（大 ✓）| 未处理 ⇒ 入口 trap + arena 全释放 ✓ | 单独评估 ✓ |

---

# 独立评审与修订（2026-10-08）

**评审方式**：只读子智能体，自包含上下文（不给我的结论 ✓），自己去代码里核事实 ✓。
**结论**：**方向成立**（A/B/C 分类 ✓、S1 先做 ✓），但 **S4 的载体与"签名不变"通道与语言既有约束自相矛盾** ✗✗，
且 **S1/S1b 的前提有事实错误** ✗。

## 一、最严重的三条（已复核）

### 1. `!` 对 `?ref` 是**绕过 null 检查的后门** ✗✗（本设计的前提被推翻）

```
p!.v（p = null）⇒ **编译通过**（我复核 ✓）；子智能体实测运行 **rc=139（SIGSEGV）**
```
⇒ **"安全代码写不出 null 解引用"是错的** ✗；而且现状比"trap"与"undefined"**都更糟** ✗✗：
它既不是响亮的 trap，也不是可处理的值，而是**裸 segfault** —— 直接违反"运行时检查一律响亮" ✓。
**修订**：`p!` 必须编成**带 file:line 的检查解引用**（失败 ⇒ trap，与其它检查一致 ✓）。
这是一条**独立的小修**（比 S1 还急 ✓：它是"静默 UB"类 ✓），要配判据 ✓。

### 2. `dyn Err` 当失败载体**自毁** ✗（S4 必须换载体）

* dyn 载荷是**拷进当前 place 池**的（`docs/topics/DYN.md:194` ✓）⇒ 失败往上传播、下层 zone 已释放时，
  句柄变死 ✗（wss 产物里本就有 3 处 `extc_trap_dead_handle` ✓）—— 与"每层 arena/zone 照常释放"**直接冲突** ✗✗；
* dyn 需要 `makesPool` 隐藏参数 ✓，而 `@export` **明令禁止隐藏参数**（`C-ABI.md` §① ✓）
  ⇒ **凡可失败者不可导出** ✗✗。

**修订（采纳评审建议）**：载体换成**纯值** ✓

```extc
struct err { code: i64  file: slice<u8>  line: i64 }   /* 字面量 slice 深度 0 ⇒ 跨 zone/协程安全 ✓ 零分配 ✓ */
```
具体错误类型用**每模块的枚举** ✓ + S3 的**静态**转换 ✓ —— 不引入池、不引入隐藏分配 ✓ 也能过 C 边界 ✓。
代价（诚实）：**丢掉 dyn 的具体类型/载荷** ✓。

### 3. "签名不变"不成立 ✗（S4 的通道必须进**类型**）

* `fn` 类型里**没有地方记 mayFail** ✓（`C-ABI.md` 自己记着：fn 槽签名"拷到局部就丢" ✓）
  ⇒ 间接调用（fn 指针 / dyn / 泛型实例）**要么漏报、要么全保守** ✗；
* `?` **只在 4 个语句位置合法** ✓（`examples/try-positions.extc` ✓；`EX_TRY` 进表达式即 `internal` 错误 ✗）
  ⇒ S4 要在任意表达式点查通道（`while` 条件 · 短路 · 下标 ✓）就得把表达式**语句化** ✗ —— 正是 P0-7 那类**静默错**的高发区 ✗。

**修订**：mayFail 位**进 `fn` 类型** ✓ —— 关键是**由编译器推断、不由作者书写** ✓
（这正是与 `effects` 的区别 ✓：作者一个字母都不用多写 ✓ 满足"不滥用 effects" ✓）。

## 二、S1 / S1b 的前提修正

| 原说法 | 实际（复核）| 修订 |
|---|---|---|
| 新建 `stdlib/std/slice.extc` 提供 `slice::at` 方法 | `impl slice<T>` 做不出来（`unknown type T` ✗）；方法只能进 `prelude.extc` 的 `struct slice` 体 ✓，或退化为**泛型自由函数** `sliceAt(s,i)` ✓（实测可用 ✓）| 落点改为 `prelude.extc` 或自由函数 ✓ |
| "slice 只有会 trap 的 `[]`" | 半对 ✗：`slice` 有 `get -> T`（trap ✓）与 `hasAt` ✓；且命名**本就混乱**：`string::at -> ?u8` ✓ vs `set::at -> T` / `linset::at -> T`（trap ✗）| 一并**统一 `at` 语义** ✓（这是个顺手该修的 ✗）|
| S1b 扫产物 trap 消息分 A/B | 消息来自 runtime helper **定义**（除零/移位/窄化各 1 条 ✓），下标检查在 helper 内 `fprintf`、**无 site 信息** ⇒ "下标从哪来"在产物里**已丢失** ✗ 棘轮会钝 | A/B 分类移到**编译器已知来源处**（下标来源分析 ✓ / plan ✓），不是产物正则 ✓ |
| `case EX_` ~462 处 | 实测 **528 处 / 488 行** ✗ | 结论不变（更该避免新节点 ✓）|

## 三、S2 / S3 的补充

* **S2**：复用 `ST_MATCH` 是最稳的路 ✓（`stMatch` 生成 if/else、无 default ✓；穷尽性只服务诊断 ✓）；
  但 `partial` 仍是**新 AST 字段** ✓ ⇒ 要审 **4 处 codegen** + `check_dataflow.c` 的 arm join + escape 的臂绑定深度 ✓；
  且臂只能写变体名（`parser.c` ✓）⇒ **`else` 无法用臂表达** ✗，"单臂"描述不完整 ✓。
* **S3**：方向可行且只限 4 个 `?` 位置 ✓；但**隐式 `IntoErr` 违背"不能证明的必须看得见"** ✗
  （会静默换错误类型 ✓），且 `E = dyn Err` 时每次转换要 `dyn_put`（隐藏分配 + 寿命 ✗），
  blanket impl 会让一切转换合法 ✗ ⇒ **转换要显式声明、并给出可查的清单** ✓。

## 四、验收指标的缺口（评审指出，采纳）

* `golden` 已按定案 98 **降级为观察** ⇒ **不能当判据** ✗；
* "B 类 trap = 0"是**下游改代码后**的数字 ✓ 不是语言指标 ⇒ 补一条**语言级**判据：
  "**含外部输入下标的示例不再 trap**" ✓；
* S4 必须补"**错误跨 zone 之后 `message()` 仍可读**" ✓ —— 正是旧载体（`dyn`）会挂的地方 ✓；
* "各层 arena 全释放"可测 ✓（仿 R12 的 live/depth/RSS ✓）。

## 五、修订后的顺序

| 序 | 事 | 变化 |
|---|---|---|
| **S0（新增，最急）** | **`p!` 编成检查解引用（失败 trap 带 file:line）** | 修"静默 UB" ✓ 独立小修 ✓ —— **已修** 2026-10-08（`1249024`，判据见文末）|
| S1 | checked 原语（落点改为 `prelude` / 自由函数 ✓ + **统一 `at` 语义** ✓）| 落点修正 ✓ |
| S1b | A/B 分类改在**编译器侧**（下标来源分析 ✓）| 机制修正 ✓ |
| S2 | `if let`（`partial` 字段 + 4 处 codegen 审查 + `else` 方案 ✓）| 补审计面 ✓ |
| S3 | `?` 转换（**显式声明** ✓ 不隐式 ✓）| 可见性修正 ✓ |
| S4 | 隐式传播（载体 = **纯值 `err`** ✓；mayFail **进 fn 类型但由编译器推断** ✓）| 载体与通道重做 ✓ |

---

# `!` 与 `@unchecked` 的契约（2026-10-08 用户裁定 + 实测）

## 契约（这是**作者唯一要记**的两件事）

| 形式 | 编译期 | 运行时失败时 | 用途 |
|---|---|---|---|
| `xs[i]` · `p.v`（证明成立）| **插检查** ✓ | **trap，带 file:line，rc=1** ✓ | 默认 ✓ |
| `p!` / `o!`（显式硬闯 ✓）| **仍插检查** ✓ | **trap**（同上 ✓）| "我保证，错了就响亮地死" ✓ |
| `@unchecked`（**函数级** ✓）| **不插任何检查** ✓ | **UB** ✓（作者签字 ✓）| **换性能、牺牲安全** ✓ |

⇒ 一句话：**`!` = 便宜地写、但错了会死；`@unchecked` = 连检查都不要。两者语义互不重叠 ✓。**

## 文档其实早就这么写了 ✓（不是新设计，是实现没跟上 ✗）

`docs/manual/01-safety.md`：
* §5：*"越界 = trap 带 extC 位置 … **一处显式例外**：函数标了 `@unchecked` 时，该函数体的元素下标**不生成检查**，越界 = **UB**（写的人签字）"* ✓
* §154：*"把 C 关在门外（不写 `@unchecked`、**不用 `!` 硬闯**）时，语言里没有第二条通往 UB 的路"* ✓

## 实测：实现与契约不符（三处）

| # | 用例 | 实测 | 应当 |
|---|---|---|---|
| **G1** | `p!.v`（`?ref` 的值 = null）| **编译通过** ✓；运行 **rc=255 且 stderr 空** ✗（= 静默 UB，不是 trap）| **trap 带 file:line，rc=1** ✓ |
| **G2** | `o!`（`option` 的值 = `none`）| **rc=0 且无 trap** ✗（疑似取到零值 ⇒ 正是"给 undefined"那种行为 ✗）| **trap** ✓（待复核确认 ✓）|
| **G3** | `@unchecked` 函数体里的可空解引用 | 子智能体实测：**null 检查仍在** ✗ | **不插任何检查** ✓（`@unchecked` 的全部意义 ✓）|

⇒ 这三条都是**窄而可测**的修复 ✓（不是设计问题 ✗）：判据放在现有 `tests/annot/` 与 `tests/` 里 ✓，
每处配"失败必须 rc=1 + stderr 带 file:line"（G1/G2）与"`@unchecked` 下**不出现**检查调用"（G3 ✓ 可扫产物 ✓）。

## 与"让语言像 C 一样好写"的关系（用户 2026-10-08）

这两个正是**作者仅有的两个逃生门** ✓：一个**响亮** ✓（`!`）、一个**快** ✓（`@unchecked`）。
它们语义一清楚，作者就不用背那一大套（mayFail / effects / checked 家族 ✗）：
**默认什么都不用写 ✓；想省钱用 `!`，想要极限性能用 `@unchecked`** ✓ —— 复杂度继续留在编译器里 ✓。

## 修复计划：`!` 改成"插检查 + trap"，`@unchecked` 成为**唯一** UB 通道 —— **已修**（2026-10-08）

> **状态：已修 ✓**（判据先行，红 → 绿）
>
> | commit | 内容 |
> |---|---|
> | `67a7aa6` | 判据（红）：G1–G4 先钉住修复前行为 |
> | `4e23606` | `@unchecked` 关掉**全部**检查（除零/移位/窄化/范围/copyInto，不再只关下标）|
> | `1249024` | 后缀 `!` 改成"先断言再取值"：`extc_refAssert` / `extc_tagAssert`，签错带位置 trap |
> | `0af62b6` | 产物指纹修正：两个助手的说明注释移进各自定义（原先是无条件发射的 scaffold ⇒ 414/414 全变）|
> | `101b338` | 修回归：`o! = v` 写载荷仍是左值（断言移到语句前缀；逗号表达式不是左值）|
> | `e436284` | 更新钉住旧 `!` 语义的测试与注释（敌意套件 / cbindgen / `std::dl`）|
>
> **产物指纹**（`tools/codegen-baseline-2026-10-05-w4.sha256`，`-w --no-line-map`）：
> 414 份里 **350 份逐字节不变**；**64 份变化**，全部有**可达的** `!`（50 份源码自己写了后缀 `!`；
> 14 份源码里没有 `!`，但可达的 stdlib 函数（如 `std::stl::string` 的 `subView`/`grow`/`at`）用了它 ⇒
> 产物里出现 `extc_tagAssert`。逐份 diff 过：变化只有新增的两个断言助手、`!` 的 tag/非空断言、
> 不纯 `!` 的临时变量（导致同函数内 `__extc_pN`/`__extc_mN` 重编号），没有别的语义变化）。
> 基准里没有任何 `@unchecked` 夹具的新增检查需要消失（`matmul_unchecked` / `bounds_un` 只有下标，
> 修复前就已被关掉 ⇒ 它们逐字节不变）；`@unchecked` 的"关全部检查"由 G3 的新夹具钉住。
>
> **判据证据**（本机 2026-10-08，`build/extc`）：
>
> | # | 命令 | 实测 |
> |---|---|---|
> | G1 | `./build/extc -w --run tests/traps/sign_nullref.extc` | **rc=1**，`tests/traps/sign_nullref.extc:9: trap: the nullable reference was asserted non-null but is null`（修复前 rc=255、stderr 空）|
> | G2 | `... tests/traps/sign_option_none.extc` | **rc=1**，`...:8: trap: the variant was asserted to hold a value but is none or failure`（修复前 rc=0、无 trap）|
> | G2' | `... tests/traps/sign_result_failure.extc` | **rc=1**，同上（修复前 rc=9、把失败载荷当成功值返回）|
> | G3 | `tests/annot/unchecked_trap_{oob,div0,nullref,narrow}.extc` 的 `@unchecked` 产物 | `extc_trap` · `extc_narrow*` · `extc_checkedIndex` · `extc_checkedRange` · `extc_div*` · `extc_shiftCount` · `extc_refAssert` · `extc_tagAssert` · `_index(` **全为 0**（修复前 div0 有 `extc_divI`、narrow 有 `extc_narrowI`）|
> | G4 | 同上四份的孪生（`sed 's/^@unchecked$//'`）运行 | 四类**都** trap 且 **rc=1**（修复前 nullref 孪生 rc=255）|
>
> **套件**：`tests/annot/run.sh` **27/0**；`tests/run.sh` **330/0**；构建 **0 warning**。
> **手册已改口**：[01-safety.md](01-safety.md) §5 与 §154（`@unchecked` = 全部检查；`!` 从 UB 出口
> 表里删掉，改为带位置的 trap）、[18-modules.md](18-modules.md) §12.5（覆盖全部检查 + 新判据）、
> [04-types.md](04-types.md)（`!` 那一行）、[C-ABI.md](C-ABI.md) §9.18 的历史注记。
> **下面"冒烟的枪"与判据表保留为修复前记录。**

### 冒烟的枪（2026-10-08 读码所得，修复前）

| 位置 | 内容 |
|---|---|
| `src/front/parser.c:3028` | 注释明文：*"`e!` … the checker emits no test for it. **Being wrong is undefined behaviour rather than a checked error.**"* ✗ |
| `src/back/codegen.c:3085` `case EX_SIGN` | `?ref` ⇒ **裸解引用**（`return genExpr(g, operand)`）✗；`option`/`result` ⇒ **直接读 `.u.X._0`**（没读 tag ⇒ 垃圾值 ✗ 那正是 G2 的 rc=0）|
| `docs/manual/01-safety.md` §154 | 把 `!` 列为 UB 通道（*"不用 `!` 硬闯"*）✗ ⇒ **手册也要改** ✓ |
| `src/back/codegen.c:366/2569` | `g->uncheckedIdx` 目前**只**用来关掉**下标**检查 ✗ ⇒ G3：它应当关掉**全部**检查 ✓ |

### 补丁形状（小，但跨 3–4 处）

1. **运行时加两个小断言助手**（都是 `EXTC_NORETURN` 失败路径 + 带 file/line ✓）：
   `extc_refAssert(void *p, const char *file, int line)` 与
   `extc_tagAssert(int tag, int want, const char *file, int line)`；
2. **`codegen.c` 的 `case EX_SIGN`**：
   * `?ref` ⇒ `(extc_refAssert((void *)&(x), "file", line), x)` ✓
   * `option`/`result` ⇒ `(extc_tagAssert((x).tag, <mangled>_some, "file", line), (x).u.some._0)` ✓
     （tag 常量名按类型的 mangle 名生成 ✓ 例如 `option_val_some` ✓）
   * **两处都要先查"当前函数是否 `@unchecked`"** ✓ —— 是 ⇒ 保持今天的裸行为 ✓（= 唯一 UB 通道 ✓）
3. **把 `uncheckedIdx` 升级成语义正确的 `uncheckedBody`**（或加一个同值的新标志 ✓），并让**所有**检查发射点都查它：
   下标 ✓（已有）· 除零/整除溢出 ✓ · 移位 ✓ · 窄化 ✓ · 可空 ✓ · 空视图 ✓。
4. **手册改口**：§5 的"`@unchecked` 只关下标"扩成"**关全部检查**" ✓；§154 改成"**只有 `@unchecked`** 通往 UB；`!` 是**带位置的 trap**" ✓。

### 判据（先写，放进现有 `tests/annot/` 与 `tests/`）

| # | 判据 | 断言 |
|---|---|---|
| G1 | `p!.v`（`?ref` = null）| **rc=1** + stderr 含 `file:line` + `trap` ✓（今天 rc=255 无消息 ✗）|
| G2 | `o!`（`option` = `none`）| **rc=1** + stderr 带位置 ✓（今天 rc=0 无 trap ✗）|
| G3 | `@unchecked` 函数体里 (a) 越界下标 (b) 除零 (c) 可空解引用 (d) 窄化 | 产物里**不出现**任何检查调用 ✓（扫 `extc_trap`/`extc_narrow` ✓）；运行 = 原样 UB/裸行为 ✓ |
| G4 | 反向：**不带 `@unchecked`** 的孪生文件 | 上述四类**都 trap 且 rc=1** ✓（`tests/annot/unchecked_mixed.extc` 已有这个"孪生文件"套路 ✓ 照用 ✓）|

**验收**：`tests/annot/run.sh` + `tests/run.sh` 全绿 ✓；**产物变化**只应出现在用到 `!`/`@unchecked` 的夹具 ✓（全语料指纹要逐条解释 ✓）。

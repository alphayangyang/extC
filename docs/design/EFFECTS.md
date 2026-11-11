# 效果与协程（设计稿 v1）

> 状态：**设计稿 v1，未实现** —— 这篇是**判据来源**。今天的事实仍在
> [`RUNTIME.md`](../analysis/RUNTIME.md)、`tests/coro/`、`stdlib/std/coro/scheduler.extc` 里；
> 实现按本文来，改语义先改本文。

这篇回答三个问题：用户怎么写等待（`request` / `suspend` / `handle`）、编译器怎么在
不标任何函数的情况下知道谁会挂起（效果掩码推断）、以及取消与超时怎么既能停下又不丢清理。
它不改运行时的回收、停顿、内存三条契约：新机制的帧走**现有**的任务 arena，回收路径一个都不新增。

**第 0 条规矩**：在新的写完之前，旧的 `coroutine<A,B>`、`yield`、`std::coro::scheduler`、
`tests/coro/` 一个字都不改（用户裁决 ⑧）。新语法先在新目录里长齐，跑通对照再谈迁移。

## 1 范围

| | 内容 |
|---|---|
| **v1 做** | `request` 声明；`suspend`；`handle ... with ... in ...`；`resume`；`park` + `Wait`（封闭 enum）；取消双入口；效果掩码**推断**（直接调用 + 递归 SCC 不动点 + 单态化）；可选效果断言；`trait` / `dyn` / 函数值的效果规则；生成 C 契约；S0–S3 判据 |
| **v1 不做** | 泛型 `request`；多次 `resume`（重试语义）；handler 作为值（rank-2）；`Source` trait（`Wait` 用封闭 enum）；跨线程 `Send` 判据与 `spawn`（S4）；`io_uring` 后端；效果行多态与行 unification；`request` 值进 `any` |

## 2 为什么要换模型：现状的三处结构性代价

1. **业务程序必须手写驱动器**。`tests/coro/coro_echo.extc` 为了传 5 个连接 × 4 字节，要自己建 `tasks<T>`、写 `while` 循环调 `pump`、处理 accept 队列与 runq；语言能表达的「等一行数据」这件事，在源码里长得像调度器内部实现。
2. **`yield` 的值是双身份的**，协议只活在注释里。`frame->ret` 朝外（`value()` 读）、`frame->in` 朝内（`send()` 写），同一个 `i64` 既当返回值又当等待请求：`>= 0` 等 fd 可读、`-3` 等时刻、`-4` 等 fd 可写、其它负数表示不等；最后一次 `yield` 必须是负数，否则驱动把它当新请求、任务永不停（`scheduler.extc` 的注释记着这条坑）。协议靠约定而不是靠类型。
3. **换不了驱动器**。事件循环、线程池、测试用的确定性驱动器本该是同一件事的三个实现，现在只有一个：`domain` 这条语言入口下面的 `extc_dom_run`（`src/back/domain.c:62`）是**按登记顺序死推到结束**，完全不解析 `yield` 出来的请求；`extc_dom_free` 全仓库零调用点。也就是说 `domain` 现在同时扮演「任务所有者」「结构化边界」「驱动器」三个角色，而第三个是假的。

新模型把这三件事拆开，每件都给出类型：

- 「等什么」→ **`request`**（一个可声明的类型，不是字符串或魔数）；
- 「谁来等」→ **`handle`**（词法作用域，驱动器是普通代码，能读、能测、能换）；
- 「等到什么」→ **`resume` 的参数**（值只走一个方向）。

## 3 五个词

| 词 | 出现在哪 | 一句话 | 类型 |
|---|---|---|---|
| `request` | 模块顶层 | 声明一类等待，每个操作带返回类型 | 声明，不是类型 |
| `suspend` | 函数体，表达式位置 | 唯一的挂起点 | `suspend(op)` 的类型 = `op` 的返回类型（带 `?` 时是它的 Ok 侧） |
| `handle ... with ... in ...` | 表达式位置 | 一个作用域处理器，也就是驱动器 | 值 = `in` 块的值 |
| `resume` | handler 的 arm 体内 | 这次等待有结果了，继续 | 语句，参数类型 = `op` 的返回类型 |
| `park` | handler 的 arm 体内 | 现在还不行，登记一个 `Wait`，稍后重试 | 语句 |

`par` 保持原样（数据并行，`for` 上的动词），见 [`PARALLEL.md`](PARALLEL.md)。

全部是**上下文关键字**，不占保留字：和 `trait` / `impl` / `dyn` / `global` / `yield` / `trap` / `ext` 同一档（先例记在 `docs/usage/language.md:92`）。已经把它们当标识符的代码继续能编。

## 4 语法

### 4.1 声明一类等待

`request` 的形状是「带返回类型的 enum」：名字 + 一组操作，每个操作有参数和返回类型；没有函数体、没有字段、不能 `impl`。

<!-- fragment -->
```extc
request Net {
    Connect(host: slice<u8>, port: i32) -> Result<i64, NetErr>
    Read(fd: i64, into: mut slice<u8>) -> Result<i64, NetErr>
    Write(fd: i64, from: slice<u8>) -> Result<i64, NetErr>
}
```

规则同 `trait`：名字大写开头，名字**程序全局唯一**（和 `trait` 共用一个命名空间，见 `src/back/modules.c:1730`），同一 `request` 内操作名唯一。返回类型必须写，`-> ()` 也要写出来。

`request` **不是类型**：不能 `let n: Net`，不能当字段/参数类型，不能进 `any`。它只能出现在两个地方——效果行（`/ Net`）和操作构造（`Read(...)`）。

### 4.2 挂起

`suspend` 是表达式，操作数必须是一个操作构造：

<!-- fragment -->
```extc
fn load(fd: i64, buf: mut slice<u8>) -> Result<i64, NetErr> / Net {
    let n = suspend(Read(fd, buf)?)
    return Ok(n)
}
```

- `suspend(op)` 的类型是 `op` 的**完整返回类型**（这里是 `Result<i64, NetErr>`）；
- `?` **只能贴在操作上**：写 `suspend(Read(fd, buf)?)`，不能写 `suspend(Read(fd, buf))?`。`?` 是编译期语法糖，它改写的是**这个挂起点恢复之后走哪条续延**：Ok 变成 `suspend` 的值（这里是 `i64`），Err 直接走函数的早退路径（跑清理、返回 `Err`）。贴到 `suspend` 外面等于要求 `suspend` 先产生一个 `Result` 值、再在挂起点之外二次分派，挂起点会长出两条续延，与「挂起即控制转移」的定义冲突；
- 函数**不需要**标注 `/ Net`——不标就用推断（见 §5）。标注是可选的**断言**；
- 一个函数里可以有多处 `suspend`，每处一个断点。

### 4.3 处理器就是驱动器

<!-- fragment -->
```extc
let r = handle Net with {
    Connect(host, port) => {
        let fd = net::connectNow(host, port)
        if fd < 0 { park(Wait::Fd(fd, Wait::Writable, 1000)) } else { resume(Ok(fd)) }
    }
    Read(fd, into) => {
        let n = net::readNow(fd, into)
        if n == net::AGAIN { park(Wait::Fd(fd, Wait::Readable, 1000)) } else { resume(Ok(n)) }
    }
    Write(fd, from) => { resume(net::writeNow(fd, from)) }
} in {
    let fd = suspend(Connect(host, port)?)
    let n = suspend(Read(fd, buf)?)
    n
}
```

语义：`in` 块里发生的每个 `suspend` 被外层的 arms 接住；arms 是**普通代码**，在驱动器自己的栈上跑（不在协程帧里），所以它能调网络、能写日志、能查表、能被测试替换。`handle` 是表达式，它的值是 `in` 块的值。

### 4.4 `resume` 与 `park`

每次 arm 被调用（= 一次**激活**）必须有且只有一个结局：

| 结局 | 写法 | 含义 |
|---|---|---|
| 有结果 | `resume(v)` | 值类型 = 操作的返回类型；任务从断点继续 |
| 还不能 | `park(w)` | 登记 `w`；`w` 就绪后**重新调用同一个 arm**（请求参数不变） |
| 取消 | 由驱动器发起，不是 arm 写的 | 见 §7.5，走清理路径 |

`park` 让 arm 变成**可重试的**：超时重试、EAGAIN 重试都是同一条路，不需要第二套语法。`park` 之后 `w` 就绪 ≠ 一定 `resume`——下一次激活可能又 `park` 一个更短的 `Wait`。

`Wait` 是库里的封闭 enum（v1）。语言不认识 `Wait` 的任何成员，只要求它是值：

| `Wait` | 参数 | 谁懂 |
|---|---|---|
| `Fd(fd, Readable, ms)` | fd + 方向 + 超时 | 事件循环驱动器 |
| `Fd(fd, Writable, ms)` | 同上 | 同上 |
| `Until(t)` | 绝对时刻 | 任何驱动器 |
| `Now` | 无 | 驱动器立刻重试（不睡眠，用于让出） |
| `Task(h)` | 子任务句柄 | 线程池（S4） |

### 4.5 效果断言（可选）

不写就是推断。写了就是上界，编译器检查 `inferred ⊆ annotated`；函数今天不挂起、明天被改挂起了，会在这里炸，而不是默默传染到所有调用者。

## 5 类型系统：效果掩码

### 5.1 掩码

每个 `request` 名在编译期拿到一个 bit，一个函数的效果是一个 **64 位掩码**（用户裁决 ②：怎么快怎么来）。超过 64 个 `request` 报错而不是静默退化——这是硬上限，写进错误信息。

效果行是集合，`/ Net + Timer` 就是两个 bit 的并。**内层 handler 可以消化一部分**：库自己 `handle Timer`，对外只露 `/ Net`。所以从下往上看效果行是**变窄**的——这和 `async` 的染色方向正好相反。

### 5.2 掩码出现在哪

| 位置 | 掩码从哪来 |
|---|---|
| 普通函数 | 推断（函数体里自身 `suspend` 的 bit ∪ 所有被调函数的掩码） |
| 用户写了 `/ R` | 断言，检查是否为推断值的超集 |
| trait 方法 | 恒 ∅（v1：trait 方法不许带效果，见 §10.1） |
| `dyn` 方法 | 恒 ∅ |
| 函数值 / 闭包 | 恒 ∅（v1：可挂起函数不能当值传，见 R12） |
| `handle` 的 `in` 块 | 块内推断值减去 R，就是它对外的贡献 |

### 5.3 推断算法

不做行多态 unification，做传播 + 不动点。整程序生成一个 `.c`，所以不存在跨单元问题。

1. **建图**：对每个函数体，收集直接 `suspend` 的操作 bit，以及每个调用点的被调者；
2. **传播**：`mask(f) = self(f) ∪ ⋃ mask(callee)`；被调者是函数值 / `dyn` / trait 方法时贡献 ∅；
3. **不动点**：调用图按强连通分量（SCC）收缩，分量内统一求并直到不再变化；自递归不需要特例；
4. **单态化**：泛型函数在每个实例上各算一份（v1 不做泛型 `request`，但泛型函数可以挂起）；
5. **核断言**：用户写的 `/ R` 与第 2–4 步的结果比，超出就报错，报错里给多出来的 `request` 名和一条调用链。

复杂度：每个函数体的边只扫一遍，SCC 用 Tarjan（线性），不动点最多 |V| 轮且实际是一次 DFS 的收敛。不需要用户写任何标注，也不会因为推断失败退回「全部标上」的染色世界。

### 5.4 与 `async/await` 的差别

| | `async/await` | 本文 |
|---|---|---|
| 函数签名 | `async fn` 改变返回类型（`T` → `Future<T>`），必须写 | 不写就是普通函数，返回类型不变 |
| 传染性 | 调用者必须 `async` 或阻塞 | 调用者什么都不用做（推断） |
| 谁来等 | 运行时（executor）藏在 `main` 里 | `handle` 是词法作用域，看得见摸得着 |
| `dyn` / 函数值 | 返回不透明类型，特殊处理 | 恒 ∅，直接禁掉，规则简单 |
| 换驱动器 | 换全局 executor | 换一个 `handle` 块 |

代价说清楚：不写签名，`-O2` 下「这个函数会不会挂起」在源码里不可见。补偿手段是 `-Wmay-suspend`（观测项，不是 v1 判据）和对模块顶层 API 要求写断言（以后再定）。

## 6 检查规则

诊断是英文短句，风格与现有 checker 一致。每条规则都要有**一个红自测**（先红后绿）。

| # | 规则 | 违约诊断 | 为什么 |
|---|---|---|---|
| R1 | `request` 名 / 操作名大写开头、全局唯一 | `request name Net is already used by a trait` | 和 trait 共用命名空间，避免歧义 |
| R2 | `request` 不是类型 | `Net is a request, not a type` | 效果只能是行，不能当值 |
| R3 | `suspend` 的操作数必须是操作构造，且 `?` 只能贴在这个操作上 | `suspend expects an operation of a request` | 否则挂起内容无法打包；`?` 是挂起点的续延，必须和挂起点一起编译 |
| R4 | `suspend` 只能在函数体里（顶层/常量位置不行） | `suspend is not allowed here` | 需要帧 |
| R5 | `handle` 必须覆盖 `in` 块的推断掩码 ∩ R | `handler for Net is missing operation Read` | 漏一个操作 = 运行期没有驱动器接 |
| R6 | 每次激活恰好一个结局（`resume` 或 `park`），且是终点 | `this handler arm falls through without resume or park` | 否则任务永远停住 |
| R7 | `resume` 实参类型 = 操作返回类型（与 `?` 无关：handler 永远看到完整 `Result`） | `resume of Net.Read expects Result<i64, NetErr>` | 值只走一条通道，类型必须对 |
| R8 | `park` 实参必须是 `Wait` | `park expects a Wait value` | 驱动器要能解释它 |
| R9 | 跨 `suspend` 存活的局部变量不得含 `mut ref` | `a mutable reference cannot live across a suspend point` | 挂起期间另一个读者可能写它，恢复后顺序不可推理 |
| R10 | 清理路径（`defer` 体、析构调用）不得挂起 | `the cleanup path may not suspend` | 取消的有限性靠它 |
| R11 | 断言是上界：`inferred ⊆ annotated` | `load may suspend on Timer, not declared in its effect row` | 断言要能挡住「偷偷多出一种等待」 |
| R12 | 函数值 / 闭包 / `dyn` / trait 方法掩码为 ∅ | `a function value may not suspend; call it directly` | 否则推断要跟着指针走，且 `dyn` 无法表达 |
| R13 | `par` 体内不得 `suspend` | `a par body may not suspend` | v1 `par` 只跑完型，不挂起 |
| R14 | `request` 值不得进入 `any` | `a request value cannot be stored in any` | 描述符里没有 request，先把口子关死 |
| R15 | 同一函数不得同时用 `yield` 与 `suspend` | `a function cannot both yield and suspend` | 两套帧机制并存会撕裂回收与取消语义 |
| R16 | `trap` 不执行 `defer`（沿用现状），取消要执行 | —（无诊断，语义） | 取消是正常结束的一种，清理必须跑 |

## 7 语义

### 7.1 一次 `suspend` 做三件事

1. 把「哪个操作 + 实参」打包进帧的请求槽（`tag` + `payload`）；
2. 把断点号写进 `pc`；
3. 返回「我挂起了」——控制权回到驱动器。**不调用、不分配、不落堆**。

恢复是反向的一步：驱动器把结果写进帧的 `in` 槽，再把 `pc` 分派到对应的 `case`。**值只走一个方向**（`in`），不像 `yield` 那样既当返回值又当请求（§2 第 2 条）。

`?` 就落在 `case k` 的开头：先看恢复值的 tag，Ok 把载荷赋给局部变量、继续往下跑；Err 把错误存进帧的返回槽、跳到与取消**共用**的清理出口（区别只在出口之后：取消丢弃结果，`Err` 把错误交给调用者）。所以带 `?` 的挂起点在生成物里仍然是 O(1) 的两个分支，不需要第二套机制。

### 7.2 handler 在哪跑

在**驱动器的线程和栈**上，不在协程帧里。所以：

- arm 里可以分配、可以调普通函数、可以再套一个 `handle`；
- arm 里的局部变量属于驱动器，任务结束不影响它们；
- arm 抛出的错误沿驱动器自己的错误路径走，不跨帧。

### 7.3 `resume` 恰好一次

v1：每个挂起点**最终**恰好一次 `resume` 或者被取消。不允许同一激活内 `resume` 两次（多 `resume` = 重试语义，需要幂等论证，见 §14）。重试的正确写法是 `park`：同一份请求参数重放，arm 从头再跑一遍。

### 7.4 `park` 与驱动器

`park(w)` 把控制权交给驱动器，驱动器负责：

1. 若 `w` 是 `Now`，立刻重试同一个 arm；
2. 否则把 `(任务, w)` 登记到自己的等待表，回到主循环；`w` 就绪时重试 arm；
3. `w` 自带超时就同时登记超时，超时到点**先重试一次 arm**（arm 可以 `resume(Err(Timeout))`），再不行就走取消（§7.5）。

`park` 不占 CPU、不占帧外内存：等待表是驱动器自己的容器，稳态由驱动的库负责有界。

### 7.5 取消

取消的发起方是驱动器（超时、父任务死、程序退出），不是业务代码。**取消不是丢弃帧**——帧要走完清理：

1. 驱动器调库里的 `task.cancel()`；
2. 驱动器再推一步，这次从 `CANCEL_<k>` 入口进（§8.2），跳过赋值，直接走作用域退出；
3. 帧执行 `defer`、析构 owned 局部变量（fd `close`、缓冲区归还），返回「结束」；
4. 驱动器回收任务（任务 arena 整块归还，走现有 closing 队列，O(1)）。

三条钉死：

- **取消不能编码成 `Result::Err`**。否则业务代码一个 `?` 就把取消吞了、继续跑，有界性直接丢。取消是控制流，走 `CANCEL_k` 入口，不可被 `match`。
- **清理路径不许挂起**（R10）。否则取消可以无限持续，就有第二个停顿源。`defer` 体里只允许同步、可数的操作。
- **超时先重试再取消**，不是「到点就删任务」，因为 arm 需要机会写 `resume(Err(Timeout))`。

取消统一了三种情况：超时、父任务死、程序退出。判据是**可观测量**：fd 打开数归零、`defer` 计数对得上、RSS 不涨。

## 8 代码生成契约

### 8.1 复用现有帧

现在的 `yield` 已经生成对了帧的形状：`ST_YIELD` 落成 `frame->ret = v; frame->pc = k; return true;`（`src/back/codegen.c:9112-9127`），`value()` 读 `frame->ret`（`codegen.c:9622`）、`send()` 写 `frame->in`（`codegen.c:9641`）。新语法用**同一个** `pc` + `case` 降级，只是：

| 槽 | 旧 `yield` | 新 `suspend` |
|---|---|---|
| `ret` | 交出去的值 | **不存在**（值只走 `in`） |
| `in` | 送回来的值 | 恢复值 / 取消标记 |
| 请求 | 编码进 `ret` 的魔数（`-3` / `-4` / …） | `tag`（操作编号）+ `payload`（实参结构） |

### 8.2 三个入口 + 双 `case`

每个可挂起函数合成三个 C 入口（名字按现有 `$` 规则）：

<!-- fragment -->
```c
int64_t f$step(void *frame);              /* 0 = 结束，1 = 挂起（tag/payload 有效） */
void    f$resume(void *frame, void *in);  /* 写 in 槽；随后由驱动器再调 step */
void    f$cancel(void *frame);            /* 置取消标记；下一次 step 从 CANCEL 入口进 */
```

每个挂起点在 `step` 里有两个 `case` 标签：

<!-- fragment -->
```c
case 3:        v = f->in;  goto cont_3;   /* 正常恢复 */
case CANCEL_3: goto cleanup;              /* 取消：跳过赋值，直接走清理 */
```

`cleanup` 是编译器生成的作用域退出序列（`defer` + owned 局部变量的析构），所有 `CANCEL_k` 汇到一处。

带 `?` 的挂起点只是把 `case k` 拆成两条续延（Ok / Err），`Err` 也跳到 `cleanup`，唯一的差别是出口之后给调用者一个错误值：

<!-- fragment -->
```c
case 3: if (f->in.err) { f->err = f->in; goto cleanup; }  v = f->in.ok;  goto cont_3;
```

### 8.3 handler 的降级

`handle R with { ... } in { body }` 降级成：

1. `body` 是一个普通函数 `body$step`（可挂起，带帧）；
2. 每个 arm 是一个普通函数 `R$h_<Op>(void *bodyframe)`，返回决策：`RESUME` / `PARK(w)` / 结束；
3. driver 侧一段 `switch (tag)` 分派到 arm，`RESUME` 就 `body$resume` 再 `step`，`PARK` 就把 `w` 交给驱动器的等待表。

生成的 C 里**没有**闭包、vtable、`setjmp`/`longjmp`、`goto` 之外的异常机制；handler 与 body 之间的接口就是上面那三个函数。

### 8.4 发射条件

`needFx` 只在 parser 见过 `request` / `suspend` / `handle` 时置位；不置位就**一行都不发**。这与 `needPar`（`src/back/codegen.c:421/422`）、`needEvent`、回收运行期的分节发射同一套做法（`[rt:name]` 分节见 `src/back/reclaim.c`）。判据：四个不用新语法的程序生成 C **逐字节不变**。

### 8.5 可读性

生成的 C 是交付物（`bench/`、`examples/` 都拿它做对照），所以：

- 每个 `suspend` 点旁一行注释 `/* suspend: Net.Read @load.extc:12 */`；
- 帧结构体按字段名可读，不用数字下标；
- 每个可挂起函数打印一行 `[fxframe] name=load frame_bytes=48 slots=3`（探针，默认关），让「帧多大」在生成物里读得出来，而不是猜的。

## 9 内存与停顿：三条契约不动

| 契约 | 新机制怎么满足 |
|---|---|
| 内存有界 `M_peak ≤ L(1 + κ_M r_max)` | 帧来自**任务 arena**，任务结束整块归还；不新增对象头、不新增堆分配路径 |
| 停顿有界 `P_step ≤ …` | `suspend` 是 O(1)（写 3 个槽 + return）；回收预算照旧；handler 的运行时回收仍是那套 |
| RSS 不显著增长 | 帧的字节数编译期可知（§8.5 探针）；常量级，不随等待次数增长 |

诚实记录两处**新增**的成本：

1. **取消的清理路径不设预算**。清理是有限指令（R10 保证不挂起），但一个 `defer` 里写长循环就能破停顿界。v1 靠纪律 + 观测判据（取消测试里单步 ≤ 200 µs），不做预算化；要预算化得把 `defer` 也编成可中断的，那是以后的事。
2. **`park` 的等待表由驱动器库负责有界**。语言只保证「一次 `park` 不分配」，表本身大小是库的属性；事件循环驱动器的表大小 = 并发任务数，跟 `tasks<T>` 同一个数量级。

## 10 与既有特性的交互

### 10.1 `trait` / `dyn`（三条规则）

晦涩的唯一来源是**效果进了 trait 签名**（Rust `async fn` in trait 的坑：返回不透明 future ⇒ `dyn` 不可用、签名传染）。你现有的 `impl` 全是纯方法（`impl Shape`、`impl io::ostream`），本来就在正确一侧。规则：

1. trait 方法默认 `/ none`，带效果必须显式，且只允许**静态分发**（泛型 bound / 单态化）；
2. `dyn Trait` 只允许纯方法（效果恒 ∅，与 R12 一致）；
3. **I/O 能力不是接口，是 `request`**——`Source` 那样的 trait 会立刻把 `dyn` 和泛型搞脏。

实用形状是「功能核心 + 命令外壳」：`parseHeader(...)` 纯函数、可测试、可并行；`load(self: ref Conn)` 带 `/ Net`，里面只有 `suspend`。

### 10.2 `par`

两个动词分工不变：`par` 摊数据并行（只跑完，v1 不挂起）；`suspend` 表达等待。`par` 体内出现 `suspend` 报 R13。等 S4 的 `spawn` 落地后，并发任务的等待走 `Join` 这个 `request`。

### 10.3 借用与所有权

跨 `suspend` 存活的局部变量只允许三类：**owned 值**、**只读视图**（`ref` / `slice`，生命周期由帧内 owner 或结构化 join 保证）、**`?ref`**。`mut ref` 禁止（R9）。

为什么只读视图可以跨：任务是结构化的——`handle` 块结束前必须把 `in` 块跑完，调用方在这期间不会撒手，所以借用的生命周期覆盖整个挂起期间；而 `mut ref` 的问题是**别名写**：挂起期间另一个读者写它，恢复后的读写顺序不可推理，这会推翻「可变数据有唯一主人」那条总规则。

### 10.4 闭包与函数值

v1：函数值 / 闭包的效果恒 ∅（R12）。想把一串操作交给别人跑，交的是 `handle` 块而不是函数指针。这条限制让推断不需要跟着指针走，`dyn` 也不需要表达效果——代价是「回调式」写法在 v1 里不能挂起。

### 10.5 泛型

泛型函数可以挂起，推断在单态化后每个实例各算一份。**泛型 `request` 不做**（用户裁决 ⑦）：`request Join { Wait(h: Task) }` 里的 `Task` 先写成 `i64` 句柄，等 S4 再谈参数化。

### 10.6 `any` / 描述符

`request` 值不进 `any`（R14）；`any` 的描述符表、装箱、JSON 编码都不认识 `request`。原因很简单：`request` 是控制流的标签，不是数据。

### 10.7 `defer`

`defer` 在正常结束、`return`、`?` 早退、以及**取消**时都执行；`trap` 时不执行（沿用现状）。`defer` 体不得挂起（R10）。

### 10.8 确定性布局

帧是编译器合成的结构体，用户看不见、也不能取地址，因此**不影响**「普通对象布局是确定性布局」这条契约（没有对象头、没有隐藏字段、`sizeof` 稳定）。

## 11 兼容与迁移

| 现状物 | v1 处置 | S3 之后 |
|---|---|---|
| `coroutine<A,B>` + `yield` + `next`/`send`/`value` | 一个字不改 | 保留成库，或标记 deprecated（看对照结果） |
| `std::coro::scheduler` | 不改 | 用新语法写一份等价实现，双跑对照 |
| `tests/coro/` | 不改 | 同一批用例用新写法再写一份，输出逐字节比对 |
| `domain` + `ext` | 不改；`ext` 的语义仍是今天的事实 | 新语法落地后，`domain` 拆成「执行器 + 词法作用域」，`ext` 退役 |
| `tests/effects/`（新） | S1 新建，只在显式开关下跑 | 并入 `tests/run.sh` |

双轨期的硬约束：**`tests/run.sh` 仍必须 380/0**，`tests/effects` 即使红也不许让它变红（S1 期间新目录不注册进总跑，只在 `tests/effects/run.sh` 里跑）。

## 12 判据（每阶段，先红后绿）

| 阶段 | 判据 | 命令 |
|---|---|---|
| S0 本文 | 文档门禁绿；三条断言被写清（R5/R9/R10 各有红自测计划） | `./check.sh docs` 13/0、`check_tree.py --gate`、`check_changelog.py --gate` |
| S1 竖切 | 20 行程序输出正确；不用新语法的四个程序生成 C 逐字节不变；`tests/run.sh` 380/0 | 见 §12.1 |
| S2 补全 | `park` / 取消 / 多 request / defer 的用例全绿；取消测试 fd 归零 | `tests/effects/run.sh` |
| S3 迁移 | 新旧两份 scheduler 在同一批 `tests/coro` 用例上输出逐字节相同 | 双跑对照脚本 |
| S4 并发 | 8 线程分治 ≥5×；跨线程 `mut ref` 编译失败；主线程 p99.9 不退化 | `tests/par` + 停顿量具 |

### 12.1 S1 的判据细节

- **正例**（§附录 B.1）期望 stdout 是 `12`；
- **三条红自测**（写进 `tests/effects/`，先确认它们报错、再确认报错文案对）：
  1. handler 漏一个操作 ⇒ R5；
  2. `mut ref` 跨 `suspend` ⇒ R9；
  3. `defer` 体里 `suspend` ⇒ R10；
- **不退化**：`examples/new.extc`、`tests/any/box_ref_field.extc`、`bench/gc/v2/src/promote.extc`、`bench/gc/v2/src/chase.extc` 由旧/新编译器生成的 C **逐字节相同**；
- **回收不退化**：`tests/reclaim/run.sh`、`tests/reclaim-perf/gate.sh`（Ir ≤ 88,029,115）、`tests/reclaim-gaps/gate-release.sh` 4/4。

判据只严不放宽：新机制跑通后，上面这些**已有的**门禁一个数字都不许退。

## 13 v1 明确不做

| 不做 | 为什么 |
|---|---|
| 泛型 `request` | 需要参数化效果与单态化后的掩码求和，先把非泛型跑顺（用户裁决 ⑦） |
| 多次 `resume`（重试） | 需要幂等论证与「已产生副作用怎么办」的语义；`park` 已经覆盖 EAGAIN/超时 |
| handler 作为值（rank-2） | 效果行要变成类型参数，推断和 `dyn` 都要重做 |
| `Source` / `Executor` trait | 会把效果塞回 trait 签名，和 §10.1 的三条规则冲突；`Wait` 用封闭 enum |
| 跨线程 `Send` 判据、`spawn` | S4 的事；等待图从树变 DAG 要重证无死锁 |
| 效果行多态 / unification | 推断用传播就够，多态是纯增量复杂度 |
| `request` 进 `any` | 控制流标签不是数据（R14） |
| `io_uring` 后端 | 驱动器是库，随时可以换；语言侧不承诺具体 reactor |

## 14 开放问题

1. **推断可见性**：要不要 `-Wmay-suspend`，要不要对 `pub` 顶层 API 强制写断言。
2. **多次 `resume`**：重试语义如果要做，要定「副作用已发生」的判据（幂等 token？）。
3. **handler 作为值**：rank-2 效果的代价（类型系统工作量）是否值得，等真有需求再谈。
4. **`Wait` 从封闭 enum 换 `Source` trait**：v2 的事，换的时候要保住 `dyn` 禁令。
5. **`Send` 判据细则**：「捕获里没有 `mut ref`、没有父帧视图」够不够，需要 S4 的实测。
6. **与回收的交互**：帧里的 owned 值跨挂起时，任务 arena 的回收时机是否需要额外约束（当前结论：不需要，整块归还）。
7. **错误传播**：`?` 与 `suspend` 组合的语义已定（`suspend` 返回 `Result`），但「handler 内部出错」怎么向上传还没定。

## 附录 A：与今天机制的一一对应

| 今天 | 本文 |
|---|---|
| `yield fd` 等可读 | `suspend(Read(fd, buf))` |
| `yield -3` 等时刻 | `suspend(Sleep(ms))` |
| `yield -4` 等可写 | `suspend(Write(fd, from))` |
| `scheduler.resumeRow` 里的 `switch` | 用户写的 `handle` arms |
| `tasks.pump` 主循环 | 驱动器库（事件循环那份） |
| `h.send(v)` | `resume(v)` |
| 用 `Result` 一路 `?` 传出去 | `?` 贴在操作上：`suspend(Read(..)?)`；handler 仍看到完整 `Result` |
| `domain::single()` + `d.run` | `handle ... in ...` |
| 「最后一次 `yield` 必须负数」 | 不存在：结束就是 `return` |

## 附录 B：目标程序

### B.1 S1 的 20 行判据程序

<!-- fragment -->
```extc
request Ask {
    Num() -> i64
}

fn twice() -> i64 {
    let a = suspend(Num())
    let b = suspend(Num())
    return a * 10 + b
}

fn main() -> i32 {
    var calls: i64 = 0
    let r = handle Ask with {
        Num() => {
            calls = calls + 1
            resume(calls)
        }
    } in {
        twice()
    }
    print(r)
    return 0
}
```

期望 stdout：`12`（第一次 `resume(1)`、第二次 `resume(2)`）。`calls` 是驱动器（`main`）的局部变量，`twice` 的帧只看得到 `in` 槽。

### B.2 S4 的目标形状（草图）

<!-- fragment -->
```extc
handle Net + Spawn with {
    Spawn(body) => { resume(executor::spawn(body)) }
    Net.Join(t) => { if executor::done(t) { resume(executor::result(t)) } else { park(Wait::Task(t)) } }
} in {
    let a = suspend(Spawn(|| sumRows(rows, 0, n / 2)))
    let b = suspend(Spawn(|| sumRows(rows, n / 2, n)))
    suspend(Net::Join(a)) + suspend(Net::Join(b))
}
```

这里用到的闭包与 `Spawn` 都在 v1 之外；它只是说明「并发 = handler 的属性」：同一个 `Spawn` 请求，事件循环 handler 让它变成协作式任务，线程池 handler 让它变成真并行。

## 附录 C：参考落点（现行口径）

| 东西 | 位置 |
|---|---|
| `yield` 的降级（`ret` / `pc` / `return true`） | `src/back/codegen.c:9112-9127` |
| `extc_coro_next` | `src/back/codegen.c:9560`（原型）/ `:9597`（定义） |
| `extc_coro_value` / `extc_coro_send` | `src/back/codegen.c:9622` / `:9641` |
| `ST_YIELD` 的解析与检查 | `src/front/parser.c:2938`、`src/check/check_expr.c:5293`、`src/check/check_stmt.c:904/3691/5221` |
| `par` 的降级与发射 | `src/check/check_stmt.c:1083`（`parLower`）、`src/back/codegen.c:15037`（`emitParallelRuntime`） |
| `needPar` 的置位与消费 | `src/back/codegen.c:421/422`、`:3207/3210`、`:14901`、`:14929` |
| `ext` / `domain` 的检查 | `src/check/check_expr.c:4419`、`src/check/check_stmt.c:5642/5644` |
| `domain` 运行时（只同步死推） | `src/back/domain.c:7`（`extc_dom_add`）、`:62`（`extc_dom_run`） |
| 事件循环调度器 | `stdlib/std/coro/scheduler.extc`（`row` / `loop` / `resumeRow` / `pump`） |
| 单线程 echo 的现状写法 | `tests/coro/coro_echo.extc` |
| 上下文关键字先例 | `docs/usage/language.md:92` |
| 名字的程序全局性（trait 先例） | `src/back/modules.c:1730` |
| 并行的证明义务 | [`PARALLEL.md`](PARALLEL.md) |
| 回收与停顿契约 | [`RECLAMATION.md`](RECLAMATION.md)、[`MEMORY-MODEL-CURRENT.md`](../analysis/MEMORY-MODEL-CURRENT.md) |

## 附录 D：术语表

| 词 | 意思 |
|---|---|
| 请求（request） | 一类等待的声明；有名字、有一组带返回类型的操作 |
| 操作（operation） | `request` 里的一个动作，比如 `Read`；`suspend(Read(...))` 是它的唯一用法 |
| 效果行（effect row） | 函数签名上可选的 `/ Net + Timer`，是推断值的**上界断言** |
| 掩码（mask） | 效果行的 64 位表示，一个 `request` 一个 bit |
| 激活（activation） | 驱动器调用一次 handler arm；必须以 `resume` 或 `park` 结束 |
| 挂起点（suspend point） | 帧里的一个 `pc` 值 + 两个 `case` 标签 |
| 取消（cancel） | 从 `CANCEL_k` 入口恢复进帧、走清理路径后结束；不是丢弃帧 |
| 结构化（structured） | 任务不逃出创建它的 `handle` 块；等待图是树，所以无死锁 |


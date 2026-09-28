<!-- 本页原来是 docs/MANUAL.md 的「8. 内建函数」一节（原 §8.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 7.9 动态数组 `varArray<T>`（对标 `std::vector` 的**基础部分**）](13-vararray.md) · [9. 错误信息 →](15-errors.md)

---

# 8. 内建函数

### ~~`print` / `println`~~ ⇒ **已弃置**（2026-09-26）：改走 `io::cout`

```extc
use std::io
io::cout << "sum = " << add(x, y) << "\n"
```

`io::cout` 是**唯一的控制台出口**：`<<` 按右操作数的**精确类型**选重载（`i64` / `f64` /
`bool` / `slice<u8>`，一个操作数一次），可以链着写 它是**缓冲**的（运行期的
`extc_cout_*`，`main` 返回前、以及 `atexit` 兜底时冲出去），格式化在**库里** ——
f64 走运行期那两行 `snprintf("%g")`，与老 `print` 用的**同一个格式** ⇒ 搬家不改输出一个字节

`print` / `println` **还能用，但每次调用都吐一条弃置警告**（`-w` 能关）：

```
x.extc:3:1: warning: `println` is deprecated
  note: `io::cout` is the one console path -- `use std::io`, then `io::cout << x`
        (`println(x)` is `io::cout << x << "\n"`)
```

**为什么弃置**：它是**按名字派发**的内建，写的是 C 的 stdio —— 那是**第二个缓冲**，
跟 `io::cout` 的顺序没法保证。实测两条：程序最后一句是 `io::cout << "x"` 时，
`x` 会排到 `println` 写过的东西**后面**；`main` 自然落空（没有 `return`）时，
那个尾巴**整个丢掉**（2026-09-26 修：`atexit` 兜底）。
搬家的账在 `PLAN.md` #67：实测 392 个文件 / 938 处，还缺 `<<` 的 `i32` 与结构化类型重载

老写法（**仅供对照**）：格式由编译器按静态类型选，用户永远不写 `"%d"`，支持所有整数、
浮点、`bool`、**枚举**、**struct**（递归）、**`slice<u8>`（按文本）**：

```extc
println(42)         // 42
println(3.14)       // 3.14
println(true)       // true
println("hi")       // hi
println(status.warn)// warn   ← 枚举自动有名字文本（定案 11）
```

> **格式串 `{}` 已定案要加**（编译期展开，不是运行时解析），但 week-0 还没实现。见第 10 节。

### `std::fs` —— 文件：**程序拥有**句柄（定案 79）

```extc
use std::fs
use std::io

fn copy(src: slice<u8>, dst: slice<u8>) -> result<unit, io::ioError> {
    var inp = fs::openRead(src)?          // 读型：看一眼名字就知道是读还是写（定案 77）
    var out = fs::openWrite(dst)?         // 写型（截断）；openAppend 保留原内容
    var buf: [65536]u8
    let n = inp.readAll(buf[..])?
    out.put(buf[0..n])?
    inp.close()?                          // 关：也是可检查的提交点
    out.close()?                          //    延迟写错误在这里冒出来
    return success(unit {})
}
```

**没有隐式关闭**（块退出**不会**代为关闭文件；`ownFd` 一类库层原语已删除）：

- 关就是 `f.close()`：幂等（句柄自己带 `open` 标志，**第二次不碰 `close(2)`**）
- **关闭后使用** ⇒ `failure(closed(fd))` **带位置**，不 trap（库不 trap，用户在处理点决定）
- **忘关不是静默**：能被证明的泄漏是一条**警告** ——「*opened and never closed*」（**不是错误**：故意留到进程结束是合法选择，`-w` 能关）：
  ```extc
  var f = fs::openWrite("out.txt")!    // ← 到函数末尾都没人关
  f.put("hi")!
  // warning: `f` is opened here and nothing in this function closes it
  ```
  检查认的协议是**库自己声明的**：struct 里声明了 `close` 方法 ⇒ 它是资源类型
  （编译器里不出现库名，跟 `slice` 靠 `data`+`len` 认出来一样）
  **它只证明能证明的**：句柄交出去 / `return` / 存进字段 ⇒ 静默（别人可能关它）
  另一半由运行时负责：泄漏至 fd 耗尽时，**open 所在行**返回带位置的 `failure`
- 故意留到进程结束也要写一行 `close()` —— 让它看得见

**为什么不隐式关**：块退时**没有失败通道**，写型的延迟写错误只能由 `close(2)` 报 ⇒
隐式归属会变成**静默丢数据**；而且 fd 是稀缺的 OS 资源，释放时机应当是看得见的一行
未来的自动释放若要做，仍要过**定案 78 留下的资格判据**（释放不会失败 / 无顺序语义 / 闭集获取）

---

**[索引](README.md)** · [← 7.9 动态数组 `varArray<T>`（对标 `std::vector` 的**基础部分**）](13-vararray.md) · [9. 错误信息 →](15-errors.md)

---

### `sys::domain::single()` —— 域的入口（**进行中**）

```extc
use std::sys::domain

fn main() -> i32 {
  let d = domain::single()      // 域：与 `slice`、`coroutine<T>` 同级的语言级特殊对象
  return 0 }
```

**域**是任务的归属处：`d.run { … }` 里用 `ext f(x)` 起的任务归它，**块结束前必须全部跑完**
（结构化）。它是**类型**说了算的 —— 不认名字、也不认注解（注解无法验证"我是域"，谁都能自称 ⇒
信任漏洞；按名字特判又会随改名悄悄失效）。

`single()` 是它今天唯一的公开入口（内建，见 `stdlib/std/sys/domain.extc`）。域在 extC 侧是
**不透明**的：C 里是一个指针，另外三个原语（`extc_dom_add` / `extc_dom_run` / `extc_dom_free`）
由**编译器**发射，普通程序既写不到也过不了 `extern!` 的边界检查。

进度：`single()` 可用，域的运行期按需发射（不用域的程序产物里一行都不出现）。
`ext` 的帧创建与驱动循环是**下一步**，所以域内写 `ext` 目前仍是编译错误（响亮拒绝，不是静默
编过去）。设计出处：`docs/topics/CONCURRENCY.md`「`ext` 与调度域」。

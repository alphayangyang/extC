<!-- 本页原来是 docs/MANUAL.md 的「9. 错误信息」一节（原 §9.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 8. 内建函数](14-builtins.md) · [10. 已定案、但还没实现 →](16-unimplemented.md)

---

# 9. 错误信息

错误带**文件、行、列、源码上下文和插入符**：

```
examples/bad.extc:3:17: error: cannot assign to `x`, which is a `let`
      x = 2
          ^
  note: use `var` to allow reassignment (`let` is an immutable binding)
```

现在检查这些，**全部由 extC 自己报，不再漏给 gcc**：

**名字与结构**
- 未定义名字、未知函数、未知字段、未知类型
- 重名的 struct / 函数 / 字段
- 参数个数不符
- 给 `let` 赋值
- 裸 `{}` 推导不出类型

**类型（T2）**
- 初始化式 / 赋值 / 实参 / 返回值 / 结构体字段的类型不符
- 有损转换（收窄）
- 整数字面量超出目标类型范围
- 条件不是 `bool`
- 两种类型没有公共类型的运算

**结构 / 枚举 / 引用（T3）**
- 重名的 struct / type / 函数 / 字段 / 方法 / 变体
- 字段和方法同名
- 不存在的变体（`status.nope`）
- `self` 不在 struct 体内、`self` 类型不对、`self` 不是首参数
- 对 `let` 取引用、对不是变量/字段的东西取引用
- 自由函数的 `ref` 实参没写 `ref`
- 零初始化 `ref T`

**只报第一个错误**，没有错误恢复。

---

### 9.0 编译器的资源上限（成文限制）

编译器对**嵌套深度**设有上限。超限时给出带位置的诊断，而不是让自身的递归耗尽 C 栈：

| 形状 | 上限 |
|---|---|
| 块与表达式的嵌套 | **64 层** |
| 类型的嵌套 | **96 层**（类型的 64 层上限由检查器先报，消息更具体） |

```
deep.extc:65:5: error: nesting too deep in a block (the compiler's limit is 64 levels)
      {
      ^
  note: Deeply nested syntax comes from a machine, and the compiler stops at a fixed depth
        rather than letting its own recursion overflow the C stack.
```

上限是**实测校准**的临时值：编译器自身在 release 构建下约 250 层、在 sanitizer 构建下约 96 层
会耗尽栈；手写代码的嵌套深度通常不到十层。把编译器的递归改为显式栈之后，上限会提高
（定案 100，2026-09-30）。

同一义务的另一个方向：**任何输入都必须在有限时间内给出结论**（编译成功，或一条诊断），
不允许挂住、也不允许崩溃。`tools/gate_scale.py` 把这条钉成常设判据（13 个用例，含时间与内存预算）。

---

### 9.1 警告（不是错误，`-w` 可关）

**未被使用的参数**。参数是签名的一部分 —— 编译器**不能**代为删除（会导致调用点不匹配），
因此采取的做法是：**在生成物中照常保留并标 `unused`**（C 编译器保持安静），**同时给出提示**：

```extc
fn pickBigger(a: ref i32, b: ref i32) -> ref i32 {
    return a                        // ← b 一次都没读过
}
// warning: parameter `b` is never used
//   note: a parameter belongs to the signature, so it is kept and marked `unused` in the generated C
```

判据是**"这个绑定到底有没有被读过"**（比的是绑定本身，不是名字 —— 所以 `let b = ...` 这种遮蔽不会被当成使用）；
`self` 不报（忽略接收者的方法很常见），编译器自己加的隐藏参数也不报
（比如出参函数那只 home arena 参数 —— 它现在只有在函数**真的用**它时才会生成）。

> 这条判据 2026-09-26 修过一族**误报**：走查只看了表达式的一部分，于是
> `a[lo..hi]` 的上下界、`T(x)` 的操作数、以及`T(x.m())` 里的方法接收者**都不算"用过"**
> —— `examples/gomoku-board.extc` 的 `lo`/`hi`、`examples/growth-factor.extc` 与
> `bench/stress/stress.extc` 的 `r` 全被误报。修法是把"问表达式里出现了什么"的走查**全部**
> 补上这些形状（切片三格 + 转换 + 聚合三形状），并把四格都钉进
> `tests/warnings/silent/params-used-deeply.extc`

**弃置的 `print` / `println`**（见第 8 节）：每次调用一条，`-w` 能关

```extc
println("hi")
// warning: `println` is deprecated
//   note: `io::cout` is the one console path -- `use std::io`, then `io::cout << x`
//         (`println(x)` is `io::cout << x << "\n"`)
```

两个警告都**不改退出码**（编译照常成功），`-w` 一关**全部**消失；
正例语料上**一条都不许响**（`tests/warnings/run.sh` 的 ②，弃置提醒单列计数、不算误报）。

---

**[索引](README.md)** · [← 8. 内建函数](14-builtins.md) · [10. 已定案、但还没实现 →](16-unimplemented.md)

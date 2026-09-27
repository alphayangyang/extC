<!-- 本页原来是 docs/MANUAL.md 的「13. 完整示例」一节（原 §13.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 12. 库与模块（2026-09-22 新增）](18-modules.md) · 下一页 →

---

# 13. 完整示例

见 `examples/`：

| 文件 | 演示 |
|---|---|
| `tour.extc` | **语言巡礼** —— 一份能跑的完整示例（`for` 与 `+=` 还没进这份示例）|
| `hello.extc` | 变量、`if/else`、`while`、函数调用、打印 |
| `fizzbuzz.extc` | `else if` 链、`%`、`while` |
| `types.extc` | 拓宽自动、字面量按值适配（T2） |
| `structs.extc` | struct、**方法写在 struct 体内**、裸 `{}` 推导、**零初始化** |
| `enums.extc` | `type` 枚举、`status.ok`、枚举自动打印名字 |
| `refs.extc` | **`ref` 表达式**、自由函数实参写 `ref`、方法接收者自动取地址 |
| `generics.extc` | **泛型 `struct Name<T>`**：四份实例同时工作、嵌套 struct、实例的自动调试打印 |
| `eq.extc` | **显式定义 `fn ==`**：普通 struct、`!=` 取反、泛型里推迟到实例化检查 |
| `debug.extc` | **自动调试打印**：递归打印 struct、枚举打名字、零初始化直接打 |
| `prelude.extc` | **prelude 里的 `slice<T>`** 直接用，两份实例 |
| `strings.extc` | **字符串 = `slice<u8>`**：字面量、`len`、prelude 的方法、空串、转义 |
| `arrays.extc` | **固定数组 `[N]T`**：多维、字面量、`...` 补零、越界 trap、值语义 |
| `slices.extc` | **切片视图**：四种写法、编译期证明的零检查、透过视图写 |
| `mut-views.extc` | **`slice<T>` / `mut slice<T>`**：源头继承、就地 `reverse`/`fill`、签名说实话 |
| `refs.extc` · `mut-ref.extc` | **`ref T` / `mut ref T`**：只读借用 vs 可写借用、`let` 也能借 |
| `ref-scalar.extc` | **标量引用**：显式 `*p` 解引用、写穿、**`swap`**（以前写不出来） |
| `array-of-struct.extc` | 数组元素是带 `fn ==` 的 struct（含一个**回归 bug** 的守卫） |
| `option-result.extc` | **`option` / `result` / `?`**：五子棋的落子与寻位 |
| `fenwick.extc` | **树状数组**：对拍 2 万次零不一致 + 逆序对（第一道真算法题） |
| `gomoku-board.extc` | **五子棋棋盘**：G2 里程碑 |

跑测试：

```sh
./tests/run.sh     # 正例跑通 + 反例必须被编译期挡掉
```

### 7.9.1 容器"造完返回"的可行性：**由推导决定，不做一刀切**（2026-09-21）

```extc
fn mk() -> varArray<i32> {          // 合法：内容落在**调用者的家** arena 里 ⇒ 活得比本帧久
    var v: varArray<i32> = varArray<i32>::withCap(0)
    v.push(42)  v.push(43)
    return v
}
fn mkSlice() -> slice<i32> {        // 合法：扩容过的容器，把视图返回也可以
    var v: varArray<i32> = varArray<i32>::withCap(0)
    v.push(7)  v.push(8)
    return v.asSlice()
}
```
判据（编译器自动算，用户不写注解）：**被调者会不会把"&实参"存进它分配的东西里**（地址流）
- **不会**（`push` 这类"只往容器里塞新东西"的）⇒ 家 arena 按**逃逸需求**选 ⇒ 想返回就活得够久
- **会**（`s.r = target` 这种把调用者的指针存进容器/家内存的）⇒ 仍然要求实参活得够家 arena ⇒
  想让它逃出去就**报错**（`tests/errors/ref_arg_too_deep` 就是这个形状）

**2026-09-22 加的第二半（定案 67 / PLAN #43）**：上面这条只说了"**地址**流"（把 `&实参` 存进容器）
还有一条**内容**流——**「从实参里读出来的、含引用的那份值」被存进容器**（`self.buf[self.len] = v`，
`v` 是带引用的 `T`）它同样**在调用点判**：那个实参（以及它携带的引用）必须活得 ≥ 目的地那一层
- 摘要说得清"第几个参数会被存" ⇒ 只查那几个
- 摘要说不清（递归 / 环 / 有解析不出来的调用）⇒ **每个含引用的实参都按最坏情况查** 消息会说清原因

推导见 [`ARENA-FORMAL.md`](../docs/topics/ARENA-FORMAL.md) §2/§3/§9（§9.5 = 落地实录 + 双向证据），
执行计划见 [`PLAN-REGION.md`](../docs/history/PLAN-REGION.md)

---

**[索引](README.md)** · [← 12. 库与模块（2026-09-22 新增）](18-modules.md) · 下一页 →

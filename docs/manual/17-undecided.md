<!-- 本页原来是 docs/MANUAL.md 的「11. 还没定的」一节（原 §11.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 10. 已定案、但还没实现](16-unimplemented.md) · [12. 库与模块（2026-09-22 新增） →](18-modules.md)

---

# 11. 还没定的

`result<void,E>` 还是 `result<(),E>`、`@main` 和 `module main` 的优先关系、`&&`/`||` vs `and`/`or`、无返回值函数要不要强制 `-> void`、要不要做多错误报告。

**`==` 右边的裸 `{}` 不推**（已定，见 [`DECISIONS.md`](../DECISIONS.md) 定案 27）：

```extc
println(ps[0] == { x: 1, y: 2 })        // cannot infer the type of a bare `{}` here
println(ps[0] == point { x: 1, y: 2 })  // 写全名字
```

`==` 是语法糖，展开成 `T.==(lhs, rhs)` 后右边**确实有**参数类型可推 —— 所以这是
「能推但不推」。**主人定调：显式是对的，不要那么多自动推导。**
（顺带避开一个坑：`fn ==(self: ref point, other: point)` 与 `other: ref point` 都存在时，
「拿左边类型当右边期望类型」在左操作数是 `ref T` 时会推错。）

---

**[索引](README.md)** · [← 10. 已定案、但还没实现](16-unimplemented.md) · [12. 库与模块（2026-09-22 新增） →](18-modules.md)

- **内联 C（`inline C!`）**：设想已归档，状态为**未定案**；签字的定位与待定问题见
  [`docs/topics/INLINE-C.md`](../topics/INLINE-C.md)，沙箱那一半见
  [`docs/topics/BOOTSTRAP.md`](../topics/BOOTSTRAP.md) §4.4。

## 数组长度作为类型参数（size 实例化）——**想过，暂不做**

**现状（实测）**：数组长度只能是**字面量**。

```
error: array length must be an integer literal, found `N`
```

所以写不出 `fn from<T, N>(a: [N]T) -> vector<T>` —— 也就是说 `vector::from([1, 2, 3])` 这种
"列表直接给 `from`"目前走不通，只能 `var a: [3]i64 = [...]` + `vector::from(a[..])` 两行。

**为什么暂不做**（作者口径）：size 实例化是签名层面的事 ⇒ 每个 `(T, N)` 一份展开 ⇒ 代码膨胀、
编译时间、mangle 与 `impl` 表都要带上长度、诊断更难读。这与"数组长度写进类型"本身无关
（Rust 也一样），代价来自**把它泛型化**。`@inline` 救不了：`N` 仍要出现在签名里。

**将来若要"一行"**，更便宜的路子是**字面量形态**（对应 Rust 的 `vec![1, 2, 3]`）：编译器在**现场**
就看见有几个元素 ⇒ 现场发一段构造，不参与泛型机制、不产生 N 份函数。它是**字面量**，不是特殊权限
（与 `[1, 2, 3]` 同级），所以和"不开特殊权限"的口径不冲突。

**今天够用**：`string::from("abc")` 一行；`vector::from(a[..])` 两行。


| 设想 | 一句话 | 文档 |
|---|---|---|
| **定长热路径 + 溢出冷路径** | 热路径定长零分配，溢出走容器，成批回填；按 deadline 的溢出必须是优先级 | [`BOUNDED-TABLE.md`](../topics/BOUNDED-TABLE.md) |
| **容器的半自动回收** | `vector` 缺的不是 GC，而是"什么时候可以缩"的可证明回收点 | [`VECTOR-GC.md`](../topics/VECTOR-GC.md) |

## 已定案、待实现

| 项 | 一句话 | 文档 |
|---|---|---|
| ~~**`global`**~~ | **2026-10-04 已实现**（`tests/coro/coro_global.extc`） | [`GLOBAL.md`](../topics/GLOBAL.md) |

## 研究材料（未定案）

| 项 | 一句话 | 文档 |
|---|---|---|
| **协程协议层** | 地基好（内存/唤醒有数字），协议层是"一串猜出来的负数 + 覆盖式 tag"；三条修法 A/B/C 与代价 | [`CORO-PROTOCOL.md`](../topics/CORO-PROTOCOL.md) |
| **普通语法的差距（手感）** | 拿真代码量：Python ~25 行 → extC 78 行（3×）；四条主因 + 优先级（格式串 → 表达式位置构造 → stdlib 补齐 → 容器字面量 → 默认参数） | [`ERGONOMICS.md`](../topics/ERGONOMICS.md) |
| **运行时块的按需发射** | `extc_print` 住在 `rtPrint` 里被**整块**发出（`needRuntime` 很粗），而"没人用就删"的 pass 只扫 `primText` ⇒ 它没被看见。规则本身是对的，方案 A：把扫描文本改成参数、两个缓冲区各扫一遍 | [`PRIM-DROP.md`](../topics/PRIM-DROP.md) |

<!-- 本页原来是 docs/MANUAL.md 的「11. 还没定的」一节（原 §11.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 10. 已定案、但还没实现](16-unimplemented.md) · [12. 库与模块（2026-09-22 新增） →](18-modules.md)

---

# 11. 还没定的

`result<void,E>` 还是 `result<(),E>`、`@main` 和 `module main` 的优先关系、`&&`/`||` vs `and`/`or`、无返回值函数要不要强制 `-> void`、要不要做多错误报告。

**`==` 右边的裸 `{}` 不推**（已定，见 [`DECISIONS.md`](../docs/DECISIONS.md) 定案 27）：

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

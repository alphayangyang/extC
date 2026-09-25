<!-- 本页原来是 docs/MANUAL.md 的「7. 结构体与方法」一节（原 §7.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 6. 语句](07-stmt.md) · [7.5 引用：`ref` 是表达式 →](09-ref.md)

---

# 7. 结构体与方法

### 7.0 可见性：`@private` 成员

声明**缺省公开**，隐藏要写出来 —— 与顶层一样，只是下沉到成员一级：

```extc
struct counter {
    @private seen: i64          // 只有本模块能读写
    label: i64                  // 公开

    fn bump(self: mut ref counter) -> i64 { self.seen = self.seen + i64(1)  return self.seen }
    @private fn zero(self: ref counter) -> i64 { return self.seen * i64(0) }
}
```

规则与理由：

- 私有成员**只能被声明它的模块使用**：跨模块读取字段、调用方法、以及在结构体字面量里写入，三者都是编译错误；
- 字段级私有存在的理由是**存储不外借**：`buf` / `vals` 一类字段一旦公开，别的模块即可取出指向池存储的视图，而池存储不允许被外部引用（`POOLS.md` 5.4）。容器应当只通过自己愿意提供的方法外借，并由方法决定借出的范围与期限；
- 同模块内一切照常：构造函数与方法体可以自由读写自己的私有成员。


**方法写在 `struct` 体内**（定案 9），首参数必须是 `self: ref 本类型`：

```extc
struct point {
    x: i32
    y: i32

    fn moveBy(self: ref point, dx: i32, dy: i32) {
        self.x = self.x + dx
        self.y = self.y + dy
    }

    fn magnitudeSquared(self: ref point) -> i32 {
        return self.x * self.x + self.y * self.y
    }
}

fn main() -> i32 {
    var p: point = { x: 1, y: 2 }
    p.moveBy(3, 4)                  // 接收者自动取地址，调用点不用写 ref
    println(p.magnitudeSquared())   // 52
    return 0
}
```

细节：

- **接收者自动取地址**：`p.moveBy(...)` 里 `p` 是值、`self` 是 `ref point`，编译器生成 `&p`。
  反过来（接收者是 `ref T`、`self` 是值类型）会自动解引用。
  > 规则：**`.` 本身就表示「在这个值上操作」**，所以方法接收者不用写 `ref`；
  > 而**自由函数的 `ref T` 实参必须在调用点写 `ref`**（见下）。
- **方法名有命名空间**：`point.eq` 和 `board.eq` 是两个不同的名字，
  顶层不用为了避冲突而发明 `point_eq` 这种名字。C 里生成 `point_eq` 做区分。
- 字段访问按类型决定用 `.` 还是 `->`，用户不用管。
- **自由函数不能有 `self` 参数** —— `self` 只属于方法。

### `impl` 块：方法写在类型体之外（2026-09-26）

方法**也可以写在类型体之外**的 `impl` 块里。它进的是同一个方法集 —— 与写在 `struct` 体内
的方法**完全同形**（同一套逐实例检查、同一套静态派发、同样的 `self` 规则）：

```extc
struct point {
    x: i64
}

impl point {
    fn sum(self: ref point, o: point) -> i64 { return self.x + o.x }
}

impl i64 {                                  // 内建标量也能挂
    fn twice(self: ref i64) -> i64 { return *self * i64(2) }
}
```

**为什么要有它**：内建标量（`i64` / `u8` / `bool` / `f64` …）是编译器内建、**没有声明体**，
而方法只能写在 `struct` 体内 ⇒ 从前**没有任何地方**能给 `i64` 写方法。标准库因此只能包一层
（`struct i64Key { v: i64  fn hash  fn == }` + `hashMapI64<V>` 适配器，见 `stdlib/stl/hashMap.extc`）。
`impl` 补上了这个挂载点，于是：

```extc
use stl::hashMap
var m: hashMap<i64, i32> = hashMap<i64, i32>::withCap(i64(8))   // 不需要任何包装
```

规则：

- **目标**可以是**别的模块里的类型**（`impl string { ... }` 写在一个 `use stl::string` 的模块里就行）——
  目标名走的是与类型标注**同一套**解析（别名表 / 歧义拒绝），不是"只认本文件"。标准库的
  `stl/stringio.extc` 就是这么给 `string` 挂流式 I/O 的。
- **目标**只能是 `struct` 或**内建标量**。枚举用 `match` 读，没有方法；泛型类型的 `impl`
  需要块自己的类型参数 —— 暂不支持（会在语法处报清楚）。
- **只加行为，不加存储**：字段仍然只在类型自己的声明里。impl 块里出现字段是错误。
- **顶层注解不适用**于 impl（`@private` / `@noCopy` / `@poolObject` / `@sharesStorage` 描述的是
  声明与存储，impl 两者都不声明）。
- **coherence：一个类型只有一个方法集。** 同一个方法名在类型体、另一个 impl、另一个模块里
  再出现一次 ⇒ **报错**（不是"后者覆盖前者"）。所以"给 `i64` 加 `hash`"这件事全程序只能有一个出处，
  标准库选择把它放在 `stl::hashMap` 里。
- **不是 trait**：impl 块里没有要求集合、没有"待实现"的约定，也没有 `dyn`。方法调用仍是
  **静态解析 + 单态化**，泛型里对 `T` 的要求仍走"推迟到实例化检查"那一套。

---

**[索引](README.md)** · [← 6. 语句](07-stmt.md) · [7.5 引用：`ref` 是表达式 →](09-ref.md)

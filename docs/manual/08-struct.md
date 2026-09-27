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

### 7.1 `impl` 块：在类型体之外挂方法

方法可以写在类型体之外：

```extc
impl point {
    fn norm2(self: ref point) -> i64 { return self.x * self.x + self.y * self.y }
}

impl i64 {                      // 内建标量没有类型体，只能这样挂
    fn hash(self: ref i64) -> i64 { return self * i64(2654435761) }
}
```

规则与理由：

- **一个类型只有一份方法集**：`impl` 是**追加**，不是替换。同名方法（运算符除外）报重复错误，
  并指出先前那一处的位置；
- 目标可以是**其他模块的类型**：`impl string { ... }` 写在 `use stl::string` 的模块里即可，
  该模块因此成为这些方法的归属；
- 目标可以写成**限定名**：`impl io::istream { ... }`（限定名与类型标注走同一套解析：别名表 + 歧义拒绝）；
- **可见性随模块**：这些方法只在该 `impl` 所在模块**被引入**时存在。未引入时报
  `` no method `x` ``，并提示应 `use` 哪个模块（`stdlib/INDEX` 提供该映射）；
- **内建标量**可以挂方法（`i64`、`u8` 等）；**枚举不能**（枚举用 `match` 读取）；
- **泛型类型的目标暂不支持**（`impl map<string, V>` 在语法处即被拒绝：块自身需要类型参数列表）。

#### 运算符重载

运算符就是名字为运算符的方法：

```extc
impl point {
    fn +(self: ref point, o: point) -> point { return point { x: self.x + o.x  y: self.y + o.y } }
}
```

- **按右操作数的类型**区分重载：`>>` 可以同时为 `i64`、`slice<u8>`、`string` 各写一个；
- 同一个**右操作数类型**写两次是错误（无法在两者间选择），诊断会指出冲突的两处；
- 运算符方法的形状固定（`self` 加一个操作数），因此操作数类型是两个定义之间唯一可区分的东西；
- 两侧类型都可比较时，`==` / `!=` 走原生比较，无需重载。



**方法写在 `struct` 体内**（定案 9），首参数必须是本类型的引用：只读用 `self: ref 本类型`，
**要写字段就得 `self: mut ref 本类型`**（`self: ref` 里写字段会报 "cannot write through a read-only
reference"）：

```extc
struct point {
    x: i32
    y: i32

    fn moveBy(self: mut ref point, dx: i32, dy: i32) {
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

### 7.2 trait 与实现（第一期：纯编译期）

```extc
trait Tag {
    fn tag(self: ref Self) -> i64
}

struct box { v: i64 }

impl Tag for box {
    fn tag(self: ref box) -> i64 { return self.v }
}

b.tag()                 // 普通方法调用：静态分发，生成的 C 里没有间接
```

规则：

- **trait 名首字母大写**。类型名是 camelCase；大写字母在**类型位置**留给类型参数 —— trait 因此自成一格，
  与两者都不会撞；
- **trait 体内只有签名**：没有函数体、没有字段；`@private` 与 `@inline` 属于有实现的地方；
- **`Self` 是 trait 的隐式类型参数**：`ref Self` 在实现处代入实现类型。`Self` 只在这两种块内有效；
- **方法进的是类型唯一的方法集**（与 `impl` 的固有方法同一份），因此调用形状与固有方法完全一样；
  这也使"两个 trait 在同一类型上声明同名方法"成为一个可见的编译错误；
- **可见性随模块**：与 `impl` 相同 —— 声明与实现所在的那个模块被引入，这些方法才存在。

第一期已实现的检查：

| 检查 | 效果 |
|---|---|
| trait 必须存在 | `impl Nope for box` ⇒ `` `impl` on unknown trait `Nope` `` |
| 一个 (trait, 类型) 一份实现 | 重复时报错并指出先前那一处 |
| 实现齐全 | 缺哪个方法就列出哪个 |
| 签名逐项相符 | 接收者形状、参数个数、参数类型、返回类型（`Self` 代入实现类型后比较） |

尚未实现：`T: Trait` 上界 · `dyn Trait` 值 · 关联类型与关联常量 · trait 默认方法 ·
孤儿规则的显式诊断 · 跨 trait 撞名的专门诊断（目前由"一个类型一份方法集"的重复检查兜住）。

### 7.3 `dyn`：运行期动态分发

```extc
trait Tag { fn tag(self: ref Self) -> i64 }

struct box { v: i64 }
struct pt  { x: i64 }
impl Tag for box { fn tag(self: ref box) -> i64 { return self.v } }
impl Tag for pt  { fn tag(self: ref pt) -> i64 { return self.x + i64(1) } }

io::cout << dyn Tag(b).tag() << "\n"      // 构造与调用一步完成
```

规则与承诺：

- **一步构造即调用**：`dyn Tag(x)` 把载荷**拷进池**，随后**经方法表**派发 —— 生成物里那是一次
  受控的间接调用（`((const struct extc_vt$…_t *)<槽>->vt)->tag(<槽>->addr)`），而不是直接调用实现；
- **载荷必须实现了该 trait**：表指向的就是那份实现；没有实现时在 `dyn` 那一行报错，
  而不是去引用一张不存在的表；
- **object safety**：经 `dyn` 派发的方法必须带 `self`、不能是泛型、不能返回 `Self`
  （静态调用它们仍然完全合法）；
- **池是对象表模式**（只追加）：块的替换或归还会让元素句柄指向别处，因此该模式下
  `give`/`resize` 一律 trap；
- **派发前校验世代**：值里带 `{池, 槽, 世代}`，表指针存在**槽**里。槽被复用 ⇒ 校验先失败 ⇒
  陈旧的值 **trap**（带源位置），而不是派发到另一个实现上；
- **可以存下来**：`let d: dyn Tag = dyn Tag(x)`；也可以放进**结构体字段**、**定长数组**与
  `varArray<T>` 等容器 —— 句柄是普通值；
- **逃出 place 的值会被提升**：`fn make() -> dyn Tag { return dyn Tag(…) }` 的返回值落在
  **调用者的 place** 里（home-zone 提升），因此仍然有效；
- **陈旧的值在派发前 trap**：载荷归 dyn 池所有，池属于它所在的 place；place 退出或池换代之后，
  旧句柄在取用时会 trap（带源位置），不会派发到别的实现上；
- **仍缺**：`dyn` 的开放注册（跨模块/动态加载，第三期）· `T: Trait` 上界 · 关联类型与关联常量 ·
  trait 默认方法。

实现与义务的完整记录见 [`docs/topics/DYN.md`](../topics/DYN.md)（第二期设计与四阶段）与
[`docs/topics/TRAITS.md`](../topics/TRAITS.md)（第一期七条决策）。

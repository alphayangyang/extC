# extC 代码速览：内存模型在代码里长什么样

> 六段**跑得起来**的代码，覆盖 Arena / Zone / Pool 三档内存与它们的接缝。
> 每段都给出实际运行输出。没有 `free`，没有生命周期标注，没有 GC，也没有静默的悬垂。
>
> 每段的源码都在 `examples/showcase-*.extc`，且由 `tests/showcase/run.sh` 在每次 `check.sh` 里
> **编译并运行、输出逐字比对** —— 本文与仓库里的代码是同一份，不会各自腐烂。

## ① 零标注：谁活多久，编译器自己算（`examples/showcase-arena-return.extc`）

```extc
struct point { x: i64  y: i64 }

/* 没有 free、没有生命周期标注、没有所有权标记。
 * 返回值的**缓冲区**落在调用者选的地方（home 机制）⇒ 返回值合法且安全。 */
fn build(n: i64) -> varArray<point> {
    var out: varArray<point> = varArray<point>::withCap(i64(4))
    var i: i64 = 0
    while i < n {
        out.push(point { x: i, y: i * i })
        i = i + i64(1)
    }
    return out
}
```

```text
$ extc --run s1_arena.extc
n=5 sum(y)=30
```

容器**值**本身是当前帧的局部量（返回时按值拷出），而它背后那块**存储**被放到了调用者的分区里
—— 两者都不需要书写。

## ② home 机制：编译器把推导结果直接报出来（`examples/showcase-home.extc`）

```extc
fn squares(n: i64) -> slice<i64> {
    var out: mut slice<i64> = new i64[n]      /* 这个分配点由**调用者**决定归属 */
    var i: i64 = 0
    while i < n { out[i] = i * i  i = i + i64(1) }
    return out
}
```

```text
$ extc --explain-memory -o /dev/null s6_home.extc
extC memory report
------------------
  One line per allocation site, with the numbers the checker decided on.
  `level` is the arena the site was placed in (0 = this frame, -1 = the caller's);
  `lexical` is the block it is written in. A site whose level is shallower than
  its own block is not released by that block.

  squares
    line 5     level -1  lexical 1    allocated outside any loop
```

`level -1` 即"放在**调用者**的分区"。这就是"全自动推导"的字面含义：不是猜，是有账可查。

```text
$ extc --run s6_home.extc
len=6 sum=55
```

## ③ Pool：同一格，不同代（`examples/showcase-pool-identity.extc`）

```extc
use stl::pool

var p: pool<i64> = pool<i64>::withCap(i64(4))
let h1 = p.insert(i64(111))        /* 句柄 = { 格, 代, 纪元 } */
let h2 = p.insert(i64(222))

p.remove(h1)                       /* 该格**换代**：h1 从此失效 */
let h3 = p.insert(i64(333))        /* 复用同一格 —— 但换了一代 */

match p.get(h1) {                  /* 陈旧句柄：读不到 h3 的值 */
    some(v) => { io::cout << "陈旧句柄读到 " << v << "（不应该发生）\n" }
    none    => { io::cout << "陈旧句柄 → none（不会指向 h3 的 333）\n" }
}
```

```text
$ extc --run s2_pool.extc
h1 格=0 代=0
h2 格=1 代=0
h3 格=0 代=1
陈旧句柄 → none（不会指向 h3 的 333）
h3 → 333
```

**`h1 格=0 代=0` 与 `h3 格=0 代=1`** —— 同一个槽位、不同的世代。池因此可以放心复用内存：
陈旧句柄指向的不是"别人的对象"，而是一个可判定的错误。

## ④ 视图：拷贝是默认，零拷贝要签字（`examples/showcase-views.extc`）

```extc
var s: string = string::withCap(i64(8))
s.append("hello world")

let copy: slice<u8> = s.toSlice()                 /* 拷贝：可以存、可以返回 */
let view: slice<u8> = s.subView(i64(0), i64(5))   /* 零拷贝：只在字符串不再变化时有效 */
```

```text
$ extc --run s3_views.extc
copy.len=11 view=5 sum=532
```

池底存储**永不外借**：要么拷贝（`toSlice` / `sub`），要么签名（`subView`，与 `!`、`extern!` 同类）。
arena 底则相反 —— `varArray` 的 `asSlice` 是**必需**的，因为 `sort(v.asSlice())` 排的必须是真存储。
分界不是风格，是两种存储的失效方式不同：

| 存储 | 交出视图 | 原因 |
|---|---|---|
| 池底（`string` / `vector` / `map`…） | **必须拷贝** | 块会被替换/归还 ⇒ 视图会真的悬垂 |
| arena 底（`varArray`） | **视图合法且必需** | 块不会被复用或提前释放 ⇒ 视图只会"停在旧快照上" |

## ⑤ `impl`：能力挂在类型体之外（`examples/showcase-impl.extc`）

```extc
struct vec { x: i64  y: i64 }

impl vec {
    fn +(self: ref vec, o: vec) -> vec { return vec { x: self.x + o.x, y: self.y + o.y } }
    fn norm2(self: ref vec) -> i64 { return self.x * self.x + self.y * self.y }
}

impl i64 {                                   /* 内建标量也能挂方法 */
    fn doubled(self: ref i64) -> i64 { return *self * i64(2) }   /* ref 取值要 * */
}
```

```text
$ extc --run s4_impl.extc
c=(11,22) norm2=605
i64::doubled=42
```

- 运算符就是方法：`a + b` 命中 `fn +`，**按右操作数类型**匹配重载；
- `impl` 可以写在**另一个模块**里（`impl string { fn parseJSON … }` 写在 `json` 模块中）⇒
  **引入那个模块才有这个方法**，这是一种"扩展方法"式的开放；
- 一个类型只有一份方法集：`impl` 是追加，重名会指名冲突的先前后。

## ⑥ 互操作：与 C 同层（`examples/showcase-extern.extc`）

```extc
extern!("libc") fn getpid() -> i32
    effects Addr=0 Cont=0                 /* 声明它对地址与继续执行的影响 */
```

```text
$ extc --run s5_ffi.extc
pid=434099（来自 libc，直通 C ABI）
```

生成物就是 C：`extern!` 直接声明 C 函数，格式串与结构体布局天然兼容，反向调用同样成立
（生成的函数就是普通 C 函数）。

---

## 这六段合起来说明什么

1. **能证明的证明**：返回一个 arena 结构不需要任何标注，深度账由编译器算，`--explain-memory` 可查；
2. **不能证明的运行期查**：池用 `{格, 代, 纪元}` 把"这还是当年那个对象吗"变成可判定的问题；
3. **不能查的签字**：`!`、`extern!` + `effects`、`subView` 三种，除此之外不开后门；
4. **失效方式决定接口形状**：池底拷贝、arena 底出视图 —— 同一门语言里两种存储各按自己的物理事实说话。

> 状态：以上全部**【已实现】**。运行期多态（`trait` / `dyn Trait`）为**【已实现】**（`tests/dyn` 常设验收）；"开放注册"按三档理解：**进程内选表已可用**（就是 `dyn` + 注册表），**同进程 C ABI 插件**与**独立进程插件**为**【计划中】**（`DECISIONS` ㉛ 的 2026-09-27 补记），动态链接（`@export`）同样**【计划中】**；这一整块的设计空间与结论见 [`topics/PLUGINS.md`](topics/PLUGINS.md)（暂缓实施），
> 设计已完成、尚未落地。

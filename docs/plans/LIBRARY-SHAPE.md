# 标准库的目标形状

> 状态：**计划** —— 不是现状；已经做完的部分在文内标了，剩下的按它排期。

这份文档是一次**结构盘点**：先把现状量清楚，再给出目标形状、命名规则、要拆掉的隐藏炸弹，和分四期的迁移路线。
范围是 `stdlib/`（不含 `extern!` 的外部库与 C ABI —— 那是下一件事）。

## 一、现状（测量得到，不是感觉）

| 指标 | 数值 |
|---|---|
| 模块数 | 45（`std/` 17 + `std/*/` 6 + `std/sys/` 10 + `stl/` 14，含 `prelude`）|
| 代码量 | 10,916 行 |
| 公开成员 | **834**，其中"按约定属于内部"的 **214**（25%）|
| 模块内顶层函数 | 222，中位名长 6，最长 15 |
| 跨模块重名 | 10 个名字各出现在 2–3 个模块（`sum`×3、`open`、`find`、`from`、`hex`、`decode`、`flushOut`、`inSync`、`inSave`、`resolve`）|
| 名字里编码类型 | 14 处（`hashI64`、`sumBlocksI64`、`maxBlocksF64`、`padI64`、`padF64`、`fromI64`、`toI64`、`toF64`、`fromF64`、`fmtI64`、`putI64`、`putHeaderI64`、`shuffleI32`）|
| 模块级可变状态 | **12 个**（`std/fs.extc` 的 `OUT_BUF/OUT_LEN/OUT_FD/OUT_FAILED/IN_BUF/IN_LEN/IN_POS/IN_FD/IN_FAILED`，`std/io/fmt.extc` 的 `FMT/PAIRS/PAIRS_READY`）|

公开面最大的六个模块：`std::http` 72、`std::data::hashMap` 44、`std::json` 41、`std::text::string` 40、`std::io` 34、`std::nd::tensor` 33。

## 二、五个根因

1. **"两个根"没有定义**。`std::` 与 `stl::` 混着放：`std::json` 不是平台、`std::text::string` 不是容器。名字层级反映的是各模块出现的先后，不是职责。
2. **"内部"只是约定**。语言不阻止跨模块访问，于是 214 个内部成员（`pStale`、`pPoison`、`extc_*`、缓冲状态机）全都出现在公开面上。清单反映不了真实 API。
3. **没有一条"返回值语义"规则**。`put` 返回"本来就在"、`set` 返回是否成功、`remove` 返回是否删掉、`insert` 返回句柄、`get` 返回 `?T` —— 同一个"修改"在五个地方有五种返回。
4. **类型后缀替代了泛型**。`sumBlocksI64` 与 `maxBlocksF64` 并存，是"标量没有 trait"的补丁，代价是名字与记忆负担。
5. **全局可变状态藏在模块里**。`fs` 的输出缓冲、`io::fmt` 的配对表都是模块级 `var`。`par` 分支或两个协程同时 `println` 时，它们不是线程安全的 —— 这是真炸弹，不是洁癖。

## 三、必须拆掉的隐藏炸弹

| # | 炸弹 | 现在会发生什么 | 目标 |
|---|---|---|---|
| 1 | `fs` 的 `OUT_BUF/OUT_LEN/OUT_FD` 是模块级 `var` | 两个 `par` 分支各写 stdout、或两个协程同时 `println`：缓冲被互相踩 | 缓冲属于 `Out` 对象；`io::out()` 返回单例并由运行期提供原子刷写；新增判据 `tests/io/concurrent-write.extc` |
| 2 | `io::fmt` 的 `FMT/PAIRS` 是模块级 `var`，且 `PAIRS_READY` 是一次性懒初始化 | 第一次格式化后的表被所有线程共享；初始化竞争 | 表是 `let` 常量表（编译期或启动期一次），`PAIRS_READY` 消失 |
| 3 | `snapshot()` 在 `vector`/`string` 上**拷贝**，而 `view()`/借出**不拷贝** | 用户以为拿到的是视图，改容器后读到旧数据（或反之）| 只剩两个名字：`view()`（借出、生命周期由检查器管）与 `snapshot()`（拷贝） |
| 4 | `std::data::pool` 的句柄生命周期是第二套世界观 | `ref` 悬垂是编译错，`handle` 过期是运行期 `none` —— 两套规则并存 | `slab<T>` 进 `std::data` 内部，句柄只在 `slab` 内部；主线只剩 `ref` |
| 5 | `string` 与 `slice<u8>` 是两种文本 | 字面量是 `slice<u8>`，`any` 装进去后 trap 报文里出现 `string$string` 这种内部名 | 字面量默认为 `string`；`slice<u8>` 只在字节流 API 上出现 |
| 6 | 两套 JSON 编码器（`std::json` 与 `reflect::json`）| 同一件事两种输出路径、两份维护成本 | `std::json` 只保留 `node` 与解析；编码统一走 `reflect`（描述符驱动）|
| 7 | 底层状态机直接对外（`io::inSync/inSave/inBad/inClear`、`fillState`、`writerOf/readerOf`）| 用户能用，但没有任何兼容承诺 | 全部 `@private`，公开面只留 `Out`/`In` 与 `|>` 式的组合 |
| 8 | `put` 的返回值是"键本来就在" | 误用成"是否成功"：`if m.put(k, v) { … }` 语义反了 | 见下方返回值规则 |

## 四、目标形状

### 4.1 依赖方向（只有三层，禁止回指）

```
prelude              ← 不依赖任何模块
std::mem, std::text, std::data, std::nd, std::reflect, std::json
                     ← 只依赖 prelude 与彼此的下层
std::io, std::fs, std::net, std::time, std::proc, std::term, std::loop,
std::http, std::ws, std::hash
                     ← 可以依赖上面两层
std::sys             ← 唯一允许出现 extc_* 与平台类型；被上面依赖，不反向依赖
```

### 4.2 模块合并表（45 → 20）

| 目标模块 | 由谁合并 | 说明 |
|---|---|---|
| `prelude` | 现有 `prelude` | 只留 `option/result` 辅助、`print/println`、`pcg32` |
| `std::mem` | `std/mem` + `std/heap` 的公开部分 | `copy/eq/find`、`pRoundUp` 改私有 |
| `std::text` | `stl/string` + `stl/stringio` + `std/io/fmt` 的格式化部分 | `string`、`format`（`any` 驱动，见 4.4）、`parse`、`split/join/trim` |
| `std::data` | `stl/{vector,hashMap,map,linmap,set,hashSet,linset,pool}` | 统一容器层，`pool` → 内部 `slab` |
| `std::nd` | `stl/{nd,tensor}` | 一种多维视图 + 两个形状 |
| `std::reflect` | `std/reflect` + `any` 描述符 | `typeName/field/format`，JSON 走它 |
| `std::json` | `std/json` | 只留 `node` + 解析 + `encode` |
| `std::io` | `std/io` + `std/io/*` | `Out/In` 对象 + `cout/cin` 门面 |
| `std::fs` `std::net` `std::http` `std::ws` `std::time` `std::proc` `std::term` `std::loop` | 各自模块 + 同名 `std/sys/*` 的公开部分 | 平台层，公开面只留"对象 + 操作" |
| `std::hash` | `std/hash/*` + `std/b64` + `std/uuid` | 摘要与编码 |
| `std::sys` | `std/sys/*` 的 `extc_*` | 全部 `@private` |

`stl::` 作为过渡别名保留两期（`use std::data::vector` 仍然可用），第三期删除。

### 4.3 命名规则（五条）

1. **成员 camelCase，类型 CamelCase**；模块常量 `UPPER_SNAKE` 且不得出现在公开文档里。
2. **名字不编码类型**：`sumI64`→`sum<T>`；`padI64/padF64/padText`→`pad(v: any, width, align)`；`fromI64/toI64`→`parse::int`/`toInt`。
3. **`extc_` 只允许出现在 `std::sys`**，并且一律 `@private`。
4. **内部助手带 `p` 前缀的一律改私有并去掉前缀**（`pStale`→私有 `staleOf`，`pRoundUp`→私有 `roundUp`），公开面不出现"魔法前缀"。
5. **视图与快照分开命名**：`view()` 借出、`snapshot()` 拷贝；禁止 `snapshot` / `view` 两种名字表达同一件事。

### 4.4 返回值语义（一条规则）

| 类别 | 返回 | 例 |
|---|---|---|
| 谓词 | `bool` | `isSorted`、`contains`、`hasAt` |
| 查找 | `?T`（或位置 `< 0`）| `get`、`find`、`indexOf` |
| 修改 | `unit` | `put`、`set`、`clear`、`release`、`push` |
| "是否新建" | `bool` 且**名字必须是 `insert`** | `insert(k, v) -> bool` |
| 删除 | `?V`（旧值）| `remove(k) -> ?V` |
| 失败可能 | `?T` / `result<T, E>` | I/O 一律 `result` |
| 程序员的断言 | trap | `v!`、`any` 隐式拆箱、越界索引 |

规则一句话：**能静态排除的错是编译错；程序员断言的是 trap；可预期的失败是 `option/result`；修改本身不返回"成功与否"。**

### 4.5 格式化：一个 `any` 换掉一族函数

`padI64`、`padF64`、`padText`、`fmtI64`、`toF64`、`fromF64`、`appendI64`、`appendF64` 这 8 个名字，本质都是"把一个值渲染成文本 + 排版"。
`any` + 描述符已经能拿到值的类型与内容，于是收敛成：

```
std::text::format(tpl: string, args: any...) -> string     // 编译期检查槽数与类型
std::text::pad(v: any, width: i64, align: Align) -> string
std::text::number-> text / text -> number 各留一个入口（`parse` / `render`）
```

代价：每次 `format` 多一次装箱（≤8 字节 0.59 ns、结构体 5.4 ns）；收益是 **8 个名字变 3 个**，并且 `"{x} {y}"` 的类型错误从运行期提前到编译期。

### 4.6 容器层的最小集合

只保留四个概念，其余是别名或删除：

| 概念 | 类型 | 为什么 |
|---|---|---|
| 连续序列 | `vec<T>`（= 现 `vector` + `varArray` 合一） | 一个就够了；`varArray` 与 `vector` 的差别只是"是否走池" |
| 映射 | `map<K, V>`（有序）+ `hashMapI64<V>`（整数键快速路径）| 两种实现，一个接口 |
| 集合 | `set<T>` + `hashSetI64` | 同上 |
| 多维视图 | `nd<T>`（= `nd` + `tensor` 合一） | 现在的 `tensor2` 是 `nd` 的 2 维特化 |

`linmap/linset`（保持插入序）保留，但明确标注"需要插入序时用，比 `map` 慢 2×"。

## 五、迁移路线（四期，每期有判据）

| 期 | 内容 | 判据 |
|---|---|---|
| **P0 收口** | 214 个内部成员全部 `@private`（含 `p*`、`extc_*`、状态机、`snapshot` 的双名）；`manual_surface` 未覆盖率 0 | `check_manual --gate` 未覆盖 = 0；`surface.md` 只剩真实 API；`tests/run.sh` 377/0 |
| **P1 拆炸弹** | `fs`/`io::fmt` 的模块级 `var` 改成 `Out`/`In` 对象与 `let` 表；`snapshot/view` → `view/snapshot` | 新增 `tests/io/concurrent-write.extc`（`par` 两分支 + 两协程同时 println，输出逐字节正确）；`bench/app` 与 `bench/crazy` 不低于现在 5% |
| **P2 合并** | `std::text`/`std::data`/`std::nd` 落地；`varArray`+`vector` 合一；8 个格式化名字 → 3 个 | `bench/crazy` 的 extC 臂从 182 行降到 ≤120 行，耗时不降；`tests/` 全绿；`docs/usage/stdlib.md` 重写 |
| **P3 换根** | `stl::` → `std::` 别名过渡后删除；`prelude` 只留语言必需 | 全仓 `use` 迁移完；`check_manual`/`docs`/`nobench` 绿；外部两个示例工程能编 |

## 六、预期收益

- 模块 45 → 20；公开成员 834 → ~600（内部 214 → ~40，且集中在 `std::sys`）；
- 顶层函数 222 → ~130；最长名 15 → 12；类型后缀 14 → 0；模块级可变全局 12 → 0；
- `bench/crazy` 的 extC 臂 182 行 → ~110 行（文本层 + 容器统一）；
- 代价：`std::text::format` 走 `any`，每次多 0.6–5 ns 装箱；`stl::` 别名期需要两份文档。

## 七、明确不做

- 不做"反射式动态调用"（`v.call("area")` 按字符串名），那会把单调性判据打薄；
- 不把 `pool`/`handle` 提升为一等公民（主线只用 `ref`，句柄留在 `slab` 内部）；
- 不为 `extern!` 的外部库设计包装（下一步单独做）。



### 容器为什么暂时**不**合并（2026-11-12 实测）

试过把 `vector<T>`（`@poolObject`，自己的板块、可原地长、可 `release`）与 `varArray<T>`
（region arena 的 `new T[cap]`）并成 `vec<T>`，撞上一个**能力差异**，不是代码风格问题：

<!-- fragment -->
```extc
fn lend() -> slice<i32> {          /* varArray 版：编得过，而且安全 */
    var a: varArray<i32> = varArray<i32>::withCap(2)
    a.push(i32(7))
    return a.view()                 /* 存储是 withCap 里 new 出来的，逃逸分析把它提到调用者那边 */
}

fn lend2() -> slice<i32> {          /* 换成池版本（vec）后：编译错误 */
    var a: vec<i32> = vec<i32>::withCap(2)
    a.push(i32(7))
    return a.view()                 /* 块住在 `a` 的池里，池随这个帧死 ⇒ 视图会悬垂 */
}
```

即：**arena 底让"视图带出调用"成立，池底让它不成立**（后者只能 `snapshot()`，而且两级返回还会再撞
深度规则）。合并要么放弃"视图带出"，要么放弃"原地长 + `release`"——两个都是用户能感知的能力。

所以暂时保持两个名字，并把差异写清楚（`vec` = 有板块、能释放、视图不外借；`varArray` = arena 底、
视图可以带出、但增长历史留在区域里）。真正的合一是语言级工作：元素引用或"冻结视图"
（见 `docs/plans/ELEMENT-REFS.md`），让一个实现同时拿到两种寿命故事。

## 进度

- **P0 完成（`981ff95`）**：`@private` 成为公开面的唯一凭据；公开面 834 → 515（公开 400 · 内部 115），
  `manual_surface --worklist` 归零；容器的存储字段（`vals`/`depth`/`chunk`…）在手册里注明"公开但不是 API"。
- **P1 完成（`62eb15a`）**：`toSlice()` → `snapshot()`（拷贝）、`asSlice()` → `view()`（借出），
  `vector`/`pool` 补 `view()`；编译器的自动借出与容器 `for` 脱糖改认 `view`/`viewMut`
  （`asSlice`/`asMutSlice` 仍接受，用户自己写的容器两种拼写都认）；手册写明"程序级单例"不是隐藏炸弹（`par` 体内不允许调用）。
- **P2 进行中**：`std::stl::*` 已拆成 `std::text::{string,stringio}`、`std::data::{vector,map,hashMap,
  set,hashSet,linmap,linset,pool,sort,seq}`、`std::nd`（`nd` + `nd::tensor`）；14 个 `stl` 模块归零。
  编译器糖里写死的模块路径（f-string → `std::text::string`、`stringio`，多下标 → `std::nd`）同步更新。
  模块级可变 `var` 已收进 `fs::OUT` / `fs::IN` / `io::fmt::FMTSTATE` 三个对象（`d67e8c6`），
  并发写下不再有"隐式共享的可变状态"这类隐藏炸弹。
- **P2 两项有意不做（带实测理由）**：
  - `vector` + `varArray` 合一：两者差的是**能力**不是风格 —— arena 底的 `varArray` 允许
    `return a.view()`（存储被逃逸分析提到调用者），池底的 `vector` 不允许（池随帧死）。
    合并要么丢"视图带出"，要么丢"原地长 + `release`"。见上面「容器为什么暂时不合并」。
  - 格式化 8 个名字收成 3 个：目标形状 `format(tpl, args: any...)` 需要**变参**，而变参按
    用户决定往后推；没有它就只能写成 `format(tpl, slice<any>)`，仪式感反而更重。
    等变参落地再收。
- **P2 合并（持续，已完成）**：已落地 `sort`→`seq`、`nd::tensor`→`nd`、`linmap`+`linset`、
  `sha1`/`sha256`/`hmac`→`hash::digest`（入口读作 `sha256.sum(...)` 的占位接收者）、
  `stringio`→`string`、`io::err`→`io`、`io::fmt`→`io`、`term`→`io`、`hashSet`→`hashMap`、
  `set`→`map`、`reflect`→`json`、`uuid`/`b64`→`digest`/`text::string`、`http`+`ws`→`net`、
  `heap`→`mem`、`time`→`loop`。**模块文件 46 → 30**（`grep -c '' <(find stdlib -name '*.extc')`）。
  两处不合并（带理由）：`std::parallel` 进不了 prelude（`@builtin fn run` 按模块限定名声明，
  prelude 不是可限定模块名 ⇒ `parallel::run()` 报 `unknown type`）；`uuid` 进 `digest` 时
  入口读作 `digest::v5*`。
- **容器家族按用户决定不合并**（m26697/m26698）：`data/{vector,map,hashMap,linmap,pool,seq,nd}`
  保持现状 —— 各自的能力差异（arena 底 vs 池底、有序 vs 哈希、视图带出）不是风格问题，
  合并会丢能力。目标的"~20 模块"按实际收敛为 **30 个模块文件**，其余收益来自命名与文档。
- **P3 完成**：全仓 `stl::` 已并回 `std::*`（`cbdb410`），`stdlib/stl/` 目录删除；
  `std::sys` 保持原样（用户明确说"不必全 private"）。

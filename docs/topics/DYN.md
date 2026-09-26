# dyn Trait 第二期：设计（运行期动态分发）

> **状态：第二期开工（2026-09-26）。本文是第二期的设计权威**；第一期（`trait` 声明 + `impl Trait for T`
> + 静态方法表）已落地，见 [`TRAITS.md`](TRAITS.md)。第二期的完成标准见 §5 的四个阶段，
> 义务表回填见 §6。

## 1. 一句话

第一期把"谁实现了哪个 trait"变成了 **一张静态表 + 稳定键**（`extc_vt$Trait$Type`，槽位顺序 =
trait 声明顺序）。第二期让**值在运行期携带这个身份**，并且**在池里安全地**做到 ——
派发前校验世代，**陈旧的值 trap，而不是跳到另一个实现上**。

## 2. 值的形状

```extc
trait Tag { fn tag(self: ref Self) -> i64 }

struct box { v: i64 }
impl Tag for box { fn tag(self: ref box) -> i64 { return self.v } }

dyn Tag(b).tag()        /* 阶段 1：构造 + 立即调用（不碰池） */
```

- **`dyn Tag` 是一个值类型**，不是引用。它的载荷是**一份拷贝**（放进 dyn 池的槽里），
  因此不存在"dyn 值指向栈上临时量"的寿命问题 —— 与语言既有的"引用不能活过它的帧"完全兼容。
- **构造写作 `dyn Tag(x)`**：与既有的 `T(x)` 转换同形，读作"把 `x` 装成 `Tag` 的 dyn 值"。
- **`ref dyn Tag` 第二期不提供**（第一期也不需要）：dyn 值是可复制的名字，借用它没有语义收益。

### 一条必须在 `dyn` 处检查的规则：object safety

不是所有方法都能进表。第一期的决策 3 已经写死：**无 `self` 的关联函数永不动态分发**。
第二期在此之上补三条，并在 `dyn Tag(...)` 的**使用点**报错（而不是等到派发）：

1. 泛型方法（`fn f<T>(...)`）不能进表；
2. 返回 `Self` 的方法不能进表；
3. 无 `self` 的关联函数不能进表。

## 3. 两条临时决定（作者尚未拍板，改动便宜 —— 请在开工阶段 2 之前确认）

| # | 决定 | 理由 | 若要改 |
|---|---|---|---|
| **A** | **陈旧的 dyn 值在派发前 `trap`**（带源位置），不返回 `failure` | 派发到另一个实现是**类型混淆**（UB 级），与本项目"bug 才 trap、条件给值"的口径一致 | 将来可加 `tryTag()` 一类返回 `failure(staleHandle)` 的形式；不影响既有程序 |
| **B** | 第二期**暂不允许** `dyn` 值进 `let` 绑定 / 字段 / 容器（阶段 3 放开） | 先把"取表前校验世代"这条不变量焊死，再扩大存储面；阶段 1 只支持"构造 + 立即调用" | 阶段 3 放开即可，不影响阶段 1/2 的判据 |

## 4. 必须做对的五件事（每一条都对应一条义务）

| # | 事 | 对应义务 | 今天的机制 |
|---|---|---|---|
| 1 | dyn 用的池**只追加** | **O4** | `extc_pool_new_table` + `extc_pool_kind`；`give`/`resize`/`_raw` 三处守卫（已落地） |
| 2 | 删除 = **墓碑**（对象在、标记已删） | O4 | 槽表里 `0` 表示空闲，即天然墓碑 |
| 3 | 派发前**校验 `gen`** | **O5** | ❌ **今天就缺这一条**（`SOUNDNESS.md`），也是第二期的核心工作 |
| 4 | 整池作废只需一次比较（`reset` 换代） | INV-G | `extc_pool_reset` 里的 `generation++`（已落地） |
| 5 | 深度上界与效应声明**只在被动态跨越的边界上**需要 | 深度/逃逸 | 静态部分照旧；动态边界单独处理，不给全语言加负担 |

## 5. 四个阶段（每阶段都要有判据，绿了才进下一阶段）

### 阶段 1 · 值形状与构造（不碰池）

- **交付**：`dyn Tag` 类型；`dyn Tag(x)` 构造；**立即派发** `dyn Tag(x).tag()`；
  object safety 三条检查；禁止绑定/字段/容器的诊断。
- **生成物**：第一次出现**受控间接调用** —— 表里取字段、按槽位调用。
  阶段 1 的值可以只活在表达式里（载荷是当前帧里的一份拷贝），所以**不需要池**。
- **判据**：`tests/dyn/dyn_call.extc`（正例：两个实现各调一次，输出正确）·
  `dyn_not_object_safe.extc`（三条各一例）· `dyn_bind_rejected.extc`（阶段 1 的存储限制）·
  生成物判据：**恰好出现预期次数的表调用**且合同编译零告警。
- **出口条件**：五个判据全绿 + `check.sh quick` 35/35。

#### 阶段 1 现状（2026-09-26，提交 b67a28c）

**已落地**：
- 形式 `dyn Trait(x).method(args)`（构造即调用）；`dyn Trait(x)` 未紧跟方法调用时报错，
  `dyn Trait(b).v` 也报错 —— **阶段 1 没有存储面可违反**；
- **生成物经表派发**：`extc_vt$Trait$Type.method(&(payload))`，即第一次受控间接调用；
- 表的**声明随原型早发、定义留在全部按偏移重写的趟之后**（只把定义放末尾会让 `main` 里的派发
  报 `undeclared` —— 实测）；
- 判据 5 条（`tests/dyn/run.sh`，已接入 `check.sh`）：正例 · 拒绝存储 · 拒绝字段 · **恰好 N 处经表派发** ·
  生成物合同编译零告警。

**两条重要经验**：
1. **`dyn` 不是保留字**：第一版把它当无条件关键字，立刻打破了 `examples/slices.extc:65`
   （`let dyn = a[lo..hi]`）。extC 没有保留字表 ⇒ **新语法必须按"形状"判定，不能按"单词"**
   （现在的条件是 `dyn <标识符> (`，用解析器既有的 `pk(p,k)` 前瞻）；
2. **故意编不过的用例必须放 `tests/<suite>/errors/`**：`check.sh` 有一节全量编译所有 `.extc`
   并与基线比对，顶层放反例会让那一节红。

**object safety（2026-09-26 更新）**：③ **已落地并配判据**（`tests/dyn/errors/dyn_self_return.extc`
⇒ `` `Dup::dup` returns `Self` and cannot be dispatched through `dyn` ``），且**静态调用仍合法**（正对照实测
`b.dup().v = 9`）—— 这正是"在 `dyn` 使用点报错，而不是禁止写这样的 trait"的口径。
① 与 ② 的现状（诚实记录，暂不重复实现）：
- ① **无 `self` 的关联函数**：调用语法上**不可达** —— 关联函数不会在接收者位置被解析成方法，
  所以写不出 `dyn T(x).assoc()`；这条作为"设计约束"保留，等阶段 2 真正建表时在**表侧**再检查一次；
- ② **泛型方法**：会被第一期的**签名比对**先拦下（trait 声明里的类型参数与 impl 无法逐项相符），
  因此当前不需要第二条诊断；等阶段 2 放开后再看是否需要更早、更准的报错。

**原先记录的落点（保留）**：
落点：`src/check_expr.c` 的 `EX_METHOD` 分支里**方法解析之后**（`e->func` 已定）。
需要按 `e->dynTrait` 在 `c->m->traits` 里找到 `TraitDef`，再对它声明的那个方法判三条：
① 必须带 `self`（`funcIsMethod`）；② 不能是泛型（`typeParams.len` 只有 `Self` 一个）；
③ 返回类型不得提到 `Self`（`mentionsParam`）。三条各配一条反例判据。
**注意**：这批检查是"在 `dyn` 使用点报错"，不是"禁止写这样的 trait" —— 静态调用它们仍然合法。

#### 阶段 1 的最小切面（2026-09-26 勘察后定，**下一轮照此机械执行**）

**决定：阶段 1 只实现"构造即调用"这一种表达式形式** —— `dyn Trait(expr).method(args)`，
**不引入新的类型 kind**。因此阶段 1 没有"存储面"可违反：`dyn Trait(x)` 出现在调用位置之外，
在解析期就是错误，诊断写成"这个形式一步完成构造与调用；**保存** `dyn` 值要到阶段 3"。

**为什么先这样**：`TY_DYN` 真正要解决的问题（值怎么进池、寿命与世代）是阶段 2/3 的事。
先做形式，可以让 object safety、trait 方法解析、签名比对（`Self` → 载荷类型）与
**生成物里的第一次受控间接调用**全部提前落地，而这些工作到阶段 2/3 **一字不改**。

**生成物形状（预期）**：

```c
box __extc_dyn0 = b;                     /* 载荷的一份拷贝：没有寿命问题 */
extc_vt$Tag$box.tag(&__extc_dyn0)        /* 从表里取字段 ⇒ 受控的间接调用 */
```

**锚点（已勘察，下一轮不必再找）**：`parseType` = `src/parser.c:1225`；方法调用节点在
`src/check_expr.c:835` 构造 `EX_METHOD`，检查在 `:2008`；codegen 走 `genMethodCall`
（`src/codegen.c:2167` 分派）；第一期的表在 `generateC` 收尾处发出（`extc_vt$…`）。

**第二次勘察的发现（让实现缩到两处 + 一个字段）**：解析器**本来就会直接构造 `EX_METHOD` 节点**
（`src/parser.c:2190` 的 `.name(args)` 分支：`recv` / `name` / `args`），而第一期的 trait 实现方法
**已经挂进了类型的方法集** ⇒ 既有的 `EX_METHOD` 检查路径（方法存在性、实参类型与个数、`self` 形状）
**一行都不用改**。dyn 额外需要的四条检查（trait 存在 · object safety 三条 · 载荷类型已实现该 trait ·
记住 dyn 标记供 codegen 用）挂在这条既有路径上即可。

于是实现只剩两处 + 一个字段：
- **解析器**：识别 `dyn Trait(expr)` 前缀，照常构造 `EX_METHOD`，并把 trait 名写进新字段
  `Expr.dynTrait`（`src/ast.h:319` 的 `method` 联合体旁）；
- **codegen**：`genMethodCall`（`src/codegen.c:1647`，分派在 `:2167`）加一个分支 —— 有 `dynTrait`
  就**取表字段调用**（`extc_vt$Trait$Type.method(&tmp)`），否则照旧直接调用。

**第三次勘察：两处细节定下来了，一处需要选择**

- **标记怎么活下来**：`dyn Tag(x)` 的载荷由 `parsePrimary` 产出，而 `.tag(args)` 是 `parsePostfix`
  里的循环**新建**一个 `EX_METHOD` 节点（`src/parser.c:2190`）—— 所以 trait 名必须在那一步
  **从接收者拷到新节点**（一行）。AST 侧新字段放在 `Expr` 的 `} u;` **之后**（联合体之外），
  这样既有代码读 `u.method` 不受影响。
- **`genMethodCall` 的 dyn 分支**：接收者类型是 `subst(g, e->u.method.recv->type)`（模板内可能是 `T`），
  间接调用写作
  `extc_vt$<Trait>$<接收者类型的 C 名>.<方法名>(&tmp)`；表符号里那一段必须与第一期发出时用的
  `tsd->name` 一致（即 `cType` 对结构体的输出），下一轮先在生成物里核对一次再写死。
- **需要选择的一处（阶段 1 的唯一开放实现细节）**：`dyn Trait(x)` **未紧跟 `.method(`** 时在哪报错。
  两个选项：
  ① 在 `parsePrimary` 的 `dyn` 分支里做**三 token 前瞻**（`.` + 标识符 + `(`）——报错位置最准，
     但要确认解析器有没有多 token 前瞻的现成手段；
  ② 标记留在节点上，在 `parsePostfix` 循环**结束时**检查"标记是否已被消费"——实现更简单，
     但要在那一步拿到循环收尾处的锚点（本次未打印）。
  **下一轮先打印 `parsePrimary` 开头与 `parsePostfix` 的收尾**，再据实选一个；不要凭猜。

**落地顺序**（第 1 步与第 2 步互不依赖，可以先做 1 再决定 2 的报错点）：
1. **解析器**：`dyn` 分支产出新节点 `EX_DYNCALL { trait 名, 载荷 expr, 方法名, 实参 }`；
   只在调用位置合法（否则报上面那条诊断）；
2. **检查器**：trait 必须存在（复用第一期的查找）→ 方法必须在 trait 里 → **object safety 三条**
   → 载荷类型必须**已实现**该 trait（复用第一期的 `(trait, 类型)` 记录）→ 按 trait 签名比对实参
   （`Self` 用既有 `ttSubstitute` 代入载荷类型）；
3. **codegen**：载荷落一个临时量，然后**经表字段调用**；
4. **判据**（注册进 `tests/dyn/run.sh`，再由 `check.sh` 拉起）：`dyn_call.extc`（两个实现，正例）·
   `dyn_object_safety.extc`（泛型方法 / 返回 `Self` / 无 `self` 的关联函数，三条各一例）·
   `dyn_store_rejected.extc`（`let d = dyn Tag(x)` ⇒ 语法诊断）·
   **生成物判据**：恰好出现预期次数的表调用，且过 `-std=c11 -fwrapv -Wall -Werror` 合同编译。

### 阶段 2 · 表进池 + 取表前校验世代（第二期的核心）

#### 运行期一半：**已落地**（2026-09-26，本轮）

`src/pools.c` 追加了一个**独立分块**（不动任何既有字符串），随池运行期一起按需发出：

| 名字 | 作用 |
|---|---|
| `ExtcDynHandle { pid, slot, gen }` | dyn 值本身：纯值、可复制 |
| `extc_dyn_put(payload, size, vt)` | 拷进池的 plate，登记槽，返回句柄 |
| `extc_dyn_vt(h, file, line)` | **派发前的校验**：槽在界内 → `pid` 相符 → 池仍是**对象表** → **世代未变**；任一不成立 ⇒ `trap`（带源位置） |

**两处实现决定（与设计稿略有出入，都是为了少碰既有文本）**：
1. **不给 `ExtcZone` 加字段**，改用按 zone 索引的旁表 `extc_dynPoolOf`，在 `extc_dyn_put` 里惰性增长 ——
   往"发出字符串里的结构体"加字段正是本会话出过事的地方；
2. 旁表里记下的池 id **可能是已死的池**（zone 下标会被循环的下一轮复用），因此只在它仍是
   "活着的对象表"（`extc_pool_kind == 1` 且 `extc_pool_generation != 0`）时才信任，否则新建一只。

**为什么这样 O5 就成立**：`vt` 存在**槽**里、不在值里 ⇒ 槽被复用成另一个实现时，**世代检查先失败** ⇒
旧值永远读不到新实现的表，E3 式类型混淆被结构性排除。

**教训（第三次同类）**：往 `src/pools.c` 追加内容时，**不能按行边界插入** —— 那里的 `bufPuts` 是
**跨行的一条语句**（`bufPuts(out, "…" \n "…")`），按 `\n` 定位会插进语句中间。正确锚点是**发出函数的
收尾大括号**，而且转义交给脚本做（不手写）。

#### 设计（2026-09-26 定，本轮实现运行期一半）

**① dyn 池是"地方的"池，不是程序全局的**

dyn 值的载荷是**一份拷贝**，它必须有个拥有者。选择**当前 place（zone）**：第一次在该 zone 里构造 dyn 值时，
惰性建一只**对象表池**（`extc_pool_new_table`），把它的 id 记在 **zone 记录**里（`ExtcZone` 加一个字段，
与既有的 `firstPool`/`color` 同一张表）。于是：

- 离开这个 place ⇒ zone 退出 ⇒ 池被丢 ⇒ **它的世代前进** ⇒ 旧 dyn 值**必然 trap**（这正是阶段 2 要的判据）；
- 不需要新的生命周期机制 —— 复用 zone 的进入/退出与 `extc_pool_drop`；
- 也**不会**把载荷的寿命拉长到超出它的地方（与 arena/zone 的口径一致）。

**② 值的形状：`{ pid, slot, gen }`（与池句柄同形，可复制、纯值）**

池里有两样东西：
- **槽表**（每次增长翻倍）：每槽 `{ int64_t gen; int64_t live; void *addr; const void *vt; }`；
- **载荷**：从池自己的 plate 取块存放（`extc_pool_take`）。

**墓碑**：删除把 `live` 置 0（内存不还，槽可复用 ⇒ 复用时代 `gen++`）——
这正是 `POOL-SOUNDNESS` INV-A 说的"删除用墓碑"。

**③ 派发前的校验（O5 的核心）**

```c
/* 运行期两个新助手（放进 src/pools.c 的池运行期文本里，与既有 extc_pool_* 同区） */
ExtcDynHandle extc_dyn_put(const void *payload, int64_t size, const void *vt);
ExtcDynSlot  *extc_dyn_slot(ExtcDynHandle h, const char *file, int line);  /* 不合法就 trap */

/* 返回**槽**而不是只返回表：调用点两样都要（`vt` 派发、`addr` 当接收者），而两样都必须来自
 * **已校验过的槽** —— 从值本身读载荷地址就会在"校验之前"使用它。 */
```

`extc_dyn_slot` 做三件事：① `pid` 必须是一只活着的**对象表**池；② 槽必须在界内且 `live`；
③ **`slot` 的世代必须等于值里带的世代**。任一条不成立 ⇒ **trap**（带源位置）。
只有全部通过，才把 `vt` 交给调用点。

**为什么这样就够**：`vt`（表指针）存在**槽里**，不在值里。因此"槽被复用成另一个实现"这种情况，
**世代检查会先失败** ⇒ 旧值永远读不到新实现的表 ⇒ `POOL-SOUNDNESS` 的 E3（类型混淆）被结构性排除，
而不是靠"运气好"。

**④ 生成物形状（预期）**

```c
/* dyn Tag(b).tag() */
ExtcDynHandle __extc_dyn0 = extc_dyn_put(&(b), sizeof(box), &extc_vt$Tag$box);
const void *__extc_vt0 = extc_dyn_vt(__extc_dyn0, "file.extc", 12);
((const struct extc_vt$Tag$box_t *)__extc_vt0)->tag(&(b));
```

载荷的地址从**值**里取（`addr`），不从栈上取 —— 这是阶段 2 与阶段 1 的关键差别：
阶段 1 直接 `&(b)`，阶段 2 必须用池里的地址，否则"拷贝进池"没有意义。

**④之一 · 接线方案（2026-09-26 第二次勘察，**上一轮那条"codegen 没有此机制"是错的**）**

**更正**：codegen **已经有**"表达式需要前置语句"的机制 —— `pfLine(CG *g, fmt, …)`（`src/codegen.c:335`）
把一条完整语句（带缩进）写进 `g->prefix`，由语句发出点统一 flush；既有用法见 `__extc_pn%d` 那处
（`:2129`，`e->needTemp` 的临时量就是这么发的）。所以**不需要**新造 `Buf pre`，也**不需要** GNU 语句
表达式。（上一轮我 grep 的关键词是 `prelude|preStmt|tmpDecl`，漏掉了 `pfLine` —— 教训：查"有没有某机制"
要按**行为**搜，不能只按想象中的命名搜。）

于是接线是**很小的改动**，全部落在 `genMethodCall` 的 dyn 分支里：
1. 两个临时量：`ExtcDynHandle __extc_dyn%d = extc_dyn_put((const void *)(<载荷>), sizeof(<T>), &extc_vt$T$T);`
   与 `ExtcDynSlot *__extc_ds%d = extc_dyn_slot(<handle>, "<file>", <line>);` —— 都用 `pfLine` 发；
2. **派发表达式**：`fname` 取 `((const struct extc_vt$T$T_t *)<slot>->vt)-><method>`，
   接收者 `recvC` 取 `<slot>->addr`；
   因为既有代码把调用印成 `"%s(%s"`，所以这两处替换后**自然**得到
   `((const struct …_t *)__extc_ds0->vt)->tag(__extc_ds0->addr, …)` ✓ 其余参数（home/池/`@overwrite`）
   一律不动；
3. 源位置照 `cgLine(g, "extc_trapMsg(\"%s\", %d, …)", <file>, <line>)`（`src/codegen.c:2559`）
   的既有写法取；
4. 生成物要能编过，还需**发出两个助手的 C 原型**（函数体在它们的定义之前）—— 与函数原型同一区。

**⑤ 判据（三条，按重要性排序）**

1. **`dyn_no_wrong_dispatch.extc`（O5 的核心）**：构造 A 的值 → 丢/复用它所在的 place 让槽被 B 复用 →
   用旧值派发 ⇒ **必须 trap**，且**绝不能**调用到 B 的方法（判据要能区分"trap"与"调错"）；
2. `dyn_stale_trap.extc`：存下 dyn 值 → 离开 place → 派发 ⇒ trap；
3. **防回归**：程序里出现 dyn 时，生成物中**必须**出现 `extc_pool_new_table`（不是 `extc_pool_new`）
   —— 这条挡住"忘了用对象表模式"这类事故（与 `tests/pool/rt_table_first.extc` 同一族）。

#### codegen 接线：**已落地**（2026-09-26）

生成物形状（实测）：

```c
ExtcDynHandle __extc_dyn0 = extc_dyn_put((const void *)(&(b)), (int64_t)sizeof(box), &extc_vt$Tag$box);
ExtcDynSlot  *__extc_ds1  = extc_dyn_slot(__extc_dyn0, "tests/dyn/dyn_call.extc", 15);
io$ostream_shl_i64(&(io$cout), ((const struct extc_vt$Tag$box_t *)__extc_ds1->vt)->tag(__extc_ds1->addr));
```

两条语句用**既有的 `pfLine`** 发（前缀语句机制）；派发表达式仍是既有那条 `"%s(%s"` 通路，只换了
callee 与接收者 —— 其余参数（home / 池 / `@overwrite`）一行未动。

**接线时撞到的两个顺序问题（同一个根因，值得记住）**
1. 我在 codegen 里置 `g->needPool = true` —— **太晚**：生成物报 `unknown type name 'ExtcDynHandle'`，
   因为池运行期的发出决定已经做过了；
2. 即使池运行期发出来，`zoneEnter` 也是**函数体开始之前**就决定好的 ⇒ 运行期 trap
   `a dyn value needs a place to live in`。
⇒ 正确做法是用**既有的 `makesPool` 标志**，由**检查器**在 dyn 调用点置位（`c->curFunc->makesPool = true`），
并把 codegen 里那行冗余删除（一处规则一处写）。**教训**：凡是"发出决定发生在体生成之前"的东西
（运行期文本、place 进入），都必须由**检查器**在类型已知处标记，不能指望 codegen 在体中段补。

**判据（7 条，已接入 `check.sh`）**：正例 · 拒绝存储 · 拒绝字段 · object safety ③ ·
**`dyn_pool_dispatch`**（`= extc_dyn_put(`/`= extc_dyn_slot(` 各 2 处，且派发形如 `->vt)->tag(__extc_ds…->addr`）·
**`dyn_object_table`**（生成物里必须出现 `extc_pool_new_table`）· 生成物合同零告警。
（旧的 `dyn_table_calls` 判据按设计**翻面**为 `dyn_pool_dispatch` —— 形状变了，判据必须同改。）

**顺序上的一个发现**：阶段 2 的两条**行为**判据（陈旧值 trap、绝不调错实现）需要**保存** dyn 值，
而那正是阶段 3 的能力 ⇒ 它们随阶段 3 一起落地；阶段 2 先用**结构判据**（表与接收者都取自槽）
把 O5 的保证焊住。

#### 原范围记录（保留）


- **交付**：dyn 值落到**对象表池**（`extc_pool_new_table`）；表本身也在池里；
  派发前校验 `gen`；陈旧值 **trap**（临时决定 A）。
- **判据**：
  - `dyn_stale_trap.extc`：存下 dyn 值 → `reset` 池 → 派发 ⇒ **trap**（进程死于 trap，不是错误结果）；
  - **`dyn_no_wrong_dispatch.extc`（O5 的核心判据）**：构造实现 A 的值 → 让它的槽被复用为
    实现 B → 用旧值派发 ⇒ 必须 trap，**绝不允许**调用到 B 的方法（这正是 `POOL-SOUNDNESS` 的 E3）；
  - 生成物判据：dyn 池的建池调用是 **`extc_pool_new_table`**（不是 `extc_pool_new`）——
    这条判据防止"忘了用对象表模式"这一类回归。
- **出口条件**：O5 判据绿；`SOUNDNESS.md` 的 O4/O5 可以翻面（阶段 4 执行）。

### 阶段 3 · 存储面放开 + 卸载墓碑

#### 实施设计（2026-09-26 勘察，锚点已备）

**① 给 `dyn Trait` 一个类型**：在 `TypeKind`（`src/ast.h:28`）加 **`TY_DYN`**，`t->name` 存 **trait 名**
（与 `TY_STRUCT` 存结构体名同形）。三处映射：`cType`（`src/codegen.c:397`）加
`case TY_DYN: return "ExtcDynHandle";`；`ttEquals` 对同名 trait 视为相同；类型打印走 trait 名。

**② 解析器**：`parseType`（`src/parser.c:1225`）加 `dyn <TraitName>` 分支（trait 名按第一期的规则：
首字母大写、**不是**保留字形状 —— 与表达式侧同样的纪律：按"形状"判定，别霸占标识符）。

**③ 检查器**：`dyn Trait(x)` 这个**值**的类型是 `TY_DYN`；载荷类型必须**已实现**该 trait（复用第一期
`(trait, 类型)` 记录）。**新的解析路径**：`EX_METHOD` 的接收者若是 `TY_DYN`，方法要在 **trait 的签名**里找，
实参按 trait 签名比对（`Self` 代入……注意这里没有具体类型，所以只比"参数个数 + 各参数类型是否与 trait
声明一致"，`Self` 位置按 dyn 值自身的类型处理）。

**④ codegen 复用既有通路（关键）**：今天的两步发出（`extc_dyn_put` → `extc_dyn_slot` → 经槽派发）
**保持不变**，只是分两种来源：
- **立即形式**（现在的 `dyn Tag(x).m()`）：载荷 = 那个表达式，照旧；
- **存储形式**（新的 `d.tag()`）：载荷已在池里 ⇒ **只发 `extc_dyn_slot(<值>, "file", line)`**
  一步，receivers 取 `->addr`。也就是说：**"存"发生在构造处，"取"发生在派发处**，两者共用同一套检查。

**⑤ 要翻面的判据（按项目纪律，行为改了判据必须同改）**
- `tests/dyn/errors/dyn_store.extc`：今天断言"保存 dyn 值被拒" —— 阶段 3 起**保存是合法的**，
  该反例要么删除，要么改成**别的**非法形状（例如 `let d: dyn Tag = box{…}` 直接赋具体类型 ⇒ 类型不符）；
- `dyn_store`/`dyn_field` 两条判据的期望文本必须跟着改（`dyn_field` 保留：dyn 值没有字段）。

**⑥ 新增判据**
- `dyn_stored.extc`：`let d: dyn Tag = dyn Tag(x)` + `d.tag()`（正例，与立即形式输出一致）；
- **`dyn_stale_stored.extc`（语言级 O5）**：保存值 → 离开 place → 派发 ⇒ trap（与运行期判据同义，
  但走语言语义）；
- **`dyn_no_wrong_dispatch.extc`**：保存 A 的值 → 让池被 B 复用 → 派发 ⇒ trap，绝不调 B 的方法；
- 卸载墓碑（第三期的动态链接未就绪时，以设计记录 + 单测式判据存在）。



- **交付**：允许 `let d: dyn Tag`、字段、容器元素；模块卸载时其 dyn 值立**墓碑**（失效 ⇒ trap）。
- **判据**：`dyn_in_field.extc` · `dyn_in_container.extc`（含遍历与派发）·
  `dyn_unload_tombstone.extc`（若第三期的动态链接未就绪，此判据以"设计记录 + 单测式判据"形式存在）。

### 阶段 4 · 文档与义务表回填

- `SOUNDNESS.md`：O4（⚠️→✅）、O5（❌→✅），并写明判据名；
- `POOL-SOUNDNESS.md`：把 E3 从"设计期反例"改为"已被判据焊住"；
- 手册：语言页补 `dyn` 一节；`16-unimplemented.md` 的 `dyn` 条目改为已落地（保留仍缺项）。

## 6. 与第一、三期的边界

- **第一期**（已落地）：`trait` 声明、`impl Trait for T`、六条检查、孤儿规则、静态方法表；
- **第二期**（本文）：上文四个阶段；
- **第三期**：开放注册与动态链接（前置：接口文件、ABI 冻结 —— **表的槽位顺序就是 ABI**、
  卸载墓碑语义）。

## 7. 开工前需要确认的（若作者不同意，改动便宜）

1. 临时决定 **A**：陈旧值 `trap` 还是给 `failure`；
2. 临时决定 **B**：阶段 3 才放开存储，是否接受；
3. 是否同期做 `T: Trait` 上界（**不属于**第二期，但会是"用 trait 写库"最想要的东西）。

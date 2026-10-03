<!-- 一次审计的快照：以代码为基准找出文档里过时的说法。修完即删，不要当长期文档维护。
     日期：2026-09-27。方法见下面「怎么核的」。审计员报告与 Lead 复核分开标注。 -->

# 文档过时清单（以代码为准）

## 怎么核的

机械类（Lead 逐条跑命令复核）：markdown 链接**精确解析**（不按文件名回退）· 手册里有 `fn main` 的完整示例
逐个试编 · 计数重新数（判据条数、公开面成员、`--help` 开关）· 文档引用的路径 `ls` 一遍。

语义类（Lead 写最小探针复现，审计员分片核对）：每条 stale 都要求给出**可复现的命令**与输出要点；
拿不准的判 `unclear`，不硬判。

## 一、用户手册（`docs/manual/`，优先级最高）

1. **`06-expr.md:64` 的语义断言已经反了。** 原文说"透过 `ref` 写**字段和元素**是允许的"；现在的编译器对
   任何透过 `ref` 的写都报错。探针：`struct board { moves: i64  fn clear(self: ref board) { self.moves = i64(0) } }`
   → `error: cannot write through a read-only reference`。正确写法是 `mut ref`。
2. 同一类错误带出两个**编不过的示例**：`08-struct.md`（`fn moveBy(self: ref point, …) { self.x = … }`）、
   `04-types.md` 泛型示例块（`fn set(self: ref box<T>, v: T) { self.value = v }`）。
3. **`16-unimplemented.md:54`** 写"**仍缺**：保存 dyn 值（`let d: dyn Tag = …`）与字段/容器元素" —— 已经能跑。
   探针 `let d: dyn Tag = dyn Tag(b); return i32(d.tag())` 编过并跑出 7；`tests/dyn/` 里 `dyn_stored` ·
   `dyn_stored_call` · `dyn_in_field` · `dyn_in_array` · `dyn_in_varArray` 都在。
4. **`16-unimplemented.md:53`** 写"`tests/dyn` **9 条判据**"，实际 `./tests/dyn/run.sh` 是 **22 条 ok**、
   顶层 13 个夹具。`08-struct.md:192` 仍列"尚未实现：… `dyn Trait` 值"，而同页 `:220` 又说可以存 —— 自相矛盾。
5. **`18-modules.md` 那段 `match run() { … match er { … } }` 编不过**：`match does not handle notATerminal`
   —— io 的错误枚举加了变体，示例没跟上。
6. **相对链接指错目录**：手册里 17 处（全书 29 处 / 12 份文档）。典型 `16-unimplemented.md:44` 写
   `../docs/DECISIONS.md`，从 `docs/manual/` 出发会解析成 `docs/docs/DECISIONS.md`；正确是 `../DECISIONS.md`。
   同类还有 `04-types.md` ×4、`README.md` ×3、`19-examples.md` ×2、`01-safety.md`、`05-vars.md`、`06-expr.md`、
   `17-undecided.md`。
7. 手册里 9 个带 `fn main` 的完整示例，3 个编不过（上面第 2、5 条），另 3 个失败是省略号 `...` 与多文件示例
   （有意为之，不算错）。

## 二、专题页（`docs/topics/`）

8. **`CONCURRENCY.md:134`** 把调度器模块写成 `stdlib/std/sched.extc`，实际是 `stdlib/std/coro/scheduler.extc`。
9. **`CONCURRENCY.md:276`** 引用 `tests/arena-soundness/H2_home_zone_depth2.extc`，该目录不存在。  —— **2026-09-28 已修**：该文件已在 `f3b0c13` 改名为 `H2_home_zone_two_hops.extc`；`src/check/check_top.c` 的两处注释已跟着改（`CONCURRENCY.md` 里其实已无旧引用）。
10. **`POOLS.md:332`** 写 `stdlib/std/pool.extc`，实际是 `stdlib/std/sys/pool.extc`。
11. **`DYN.md` 有 13 处引用了不存在的夹具名**（`dyn_in_container` · `dyn_object_safety` · `dyn_not_object_safe` ·
    `dyn_bind_rejected` · `dyn_no_wrong_dispatch` · `dyn_stale_stored` · `dyn_rt_p1.c` …）—— 要么改名没跟，
    要么计划中的夹具没标"计划"。
12. **`EXTC-SHOWCASE.md`** 的命令行示例用过时文件名（`extc --run s1_arena.extc`），实际是
    `examples/showcase-*.extc` 六份。正文代码块**不是**过时的：它们被 `check.sh` 每次编译并逐字比对输出。
13. **`MODULES.md:281`** 写 `gomoku/board.extc` 与 `gomoku.extc`，实际是 `examples/gomoku-board.extc`。

### `DYN.md` / `TRAITS.md`（审计员报告，Lead 抽样复核过）

基线：`./tests/dyn/run.sh` → **通过 22，失败 0**；`tests/dyn/` 顶层 13 个夹具全部编得过。

| 位置 | 过时说法 | 代码现状 |
|---|---|---|
| `DYN.md:3` | 状态：第二期**开工**（2026-09-26） | 第二期已完成（同文件 `:622` 写着"四条目标全部达成"）|
| `DYN.md:38` | 两条临时决定"作者尚未拍板，开工阶段 2 前确认" | 两条都已定案并落地 |
| `DYN.md:43` | 第二期**暂不允许** dyn 值进 `let`/字段/容器（阶段 3 放开）| 已放开（`dyn_stored`/`dyn_in_field`/`dyn_in_array` 绿）|
| `DYN.md:51` | 派发前校验世代"**今天就缺这一条**" | 已落地（`extc_dyn_slot` 五段校验链，`dyn_o5_stale_trap` 绿）|
| `DYN.md:66,76` | 出口条件"五个判据全绿" | 现为 22 条 |
| `DYN.md:89-91` | 无 `self` 的关联函数"调用语法上不可达，写不出 `dyn T(x).assoc()`" | 写得出，但**随后段错误**（见下"代码 bug"第 1 条）|
| `DYN.md:92-93` | 泛型方法"会被签名比对拦下，当前不需要第二条诊断" | 声明期就报 `unknown type T`；`check_expr.c:2266` 的专门诊断成了死代码 |
| `DYN.md:104-106` | 阶段 1"只实现构造即调用，不引入新 kind" | `EX_DYN`/`TY_DYN` 已在（`ast.h:139/:327`、`parser.c:1240`）|
| `DYN.md:175,235` | 派发前校验叫 `extc_dyn_vt(h, file, line)` | 符号是 `extc_dyn_slot`（`src/back/pools.c:785`；改名见提交 `3c4191e`）|
| `DYN.md:264-268` | 判据三条：`dyn_no_wrong_dispatch.extc` · `dyn_stale_trap.extc` | 两个夹具名全仓不存在；现用 `dyn_o5_stale_trap`、`dyn_object_table` |
| `DYN.md:270-278` | 生成物形状里有 `extc_dyn_put(…)` | 立即形式 `put`/`slot` 计数都是 0（直接查表调用）|
| `DYN.md:292-295` | 判据 7 条；`dyn_pool_dispatch` 断言 `put`/`slot` 各 2 处 | 现共 22 条；该判据已翻面成"立即形式不碰池：put=0 slot=0" |
| `DYN.md:358-360` | 待办："要翻面的判据 `tests/dyn/errors/dyn_store.extc`" | 该夹具不存在；已翻面为 `dyn_stored` 正例 |
| `DYN.md:379,410` | "`16-unimplemented.md` 的 dyn 条目已改为已落地" | 手册那页还没改（见上文第 3、4 条）|
| `DYN.md:401,404` | 判据名 `dyn_store` · `dyn_field` · `dyn_pool_dispatch` | 现名 `dyn_stored` · `dyn_in_field` · `dyn_object_table` |
| `DYN.md:425` | `d.tag()`（接收者是 dyn）"下一件" | 已落地（`dyn_stored_call` 绿，calls=7,105）|
| `DYN.md:431-432` | 解析器仍保留"必须立即调用"的限制 | `EX_DYN` 已构造值、`TY_DYN` 已定型 |
| `DYN.md:577,601` | 槽回收"已定案，待落地"；运行期文本暂存 `/tmp/dyn_rt_p1.c` | 已落地（`extc_dynSweep` + 空闲链，`dyn_slot_reclaim` 绿）；`/tmp` 那个文件早没了 |
| `DYN.md:612,625,745` | "判据 16 条全绿" · "17 条，check.sh 第 80 行" · "19/19" | 现 22 条；`check.sh` 里在**第 86 行** |
| `TRAITS.md:3` | 状态：第一期"已定案待实现" | 已落地（`parser.c:772` `parseTrait`；`tests/impl` 判据绿）|
| `TRAITS.md:24,70` | "没有任何代码使用它，生成物里不应出现函数指针调用" | 生成物里有 thunk 与经表间接调用（`tt.c:920-922`、`imm.c:915`）|
| `TRAITS.md:28,36,37` | "实施清单（下一轮照此执行）"整节 | 已落地（`codegen.c:6903-6971`）；判据已翻面成 `trait_static_only.extc` |
| `TRAITS.md:62-63,118` | 静态表形状 `int64_t(*tag)(circle *)`；字段类型用 `__typeof__` | 现为"每 trait 统一签名 + `void *` 擦除 + thunk"；`__typeof__` 0 处 |
| `TRAITS.md:124-125` | 判据"生成物里没有间接调用：vtable 符号只出现一次" | 现口径：`vtable_no_call` = 对**不用 dyn 的程序**断言表/调用计数为 0 |

### `WHY-EXTC.md` / `DESIGN.md` / `DECISIONS.md`（审计员报告；数量类与 `PLAN.md:339` 由 Lead 复核过）

| 位置 | 过时说法 | 代码现状 |
|---|---|---|
| `WHY-EXTC.md:76` | "`dyn` 的实现会引入受控间接调用，属**【计划中】**" | 已实现（`tests/dyn/dyn_shape_area.extc` 编过；`codegen.c:6916` 发表、`:1805` 间接调用）|
| `WHY-EXTC.md:107,141` | "`trait` 声明与 `dyn Trait`｜**计划中**（设计已完成）" | 已实现（`tests/impl/trait_ok.extc` 编过）；"计划中"只剩开放注册/动态链接 |
| `WHY-EXTC.md:143` | 一行里把 `main(args)` · `impl` 泛型目标 · 枚举方法 · `[N]K` 全标"计划中" | 前半已实现（`tests/argv/args.extc`、`tests/extern/main.extc`），后半仍不支持（`tests/impl/errors/*`）⇒ 该拆行 |
| `DESIGN.md:76` | "`a[i]!`：越界会 trap" | `!` 只吃 `option`/`result`/可空引用；越界 trap 由普通 `a[n]` 提供（`tests/traps/index_out_of_range.extc`）|
| `DESIGN.md:187` | "无 vtable / RTTI" | `dyn Trait` 是有意引入的受控间接分派（`codegen.c:6900-6960` 函数指针表）|
| `DESIGN.md:360,367,369` | "泛型约束靠签名，**不靠 trait，不靠运算符重载**"、两行"已判出局" | 两者都已落地（`tests/ops/concrete.extc`、`tests/impl/trait_ok.extc`）|
| `DESIGN.md:439` | "`fn ==` 必须写在 struct 体内" | struct 体内**或 `impl` 块**都行（`tests/impl/` 一整套）|
| `DESIGN.md:444` | "只开放 `==` / `!=`" | 比较六 + 算术五 + `<< >>`（`tests/ops/`）|
| `DESIGN.md:142,822` | "`region`：全语言唯一为 arena 新增的语法" | 词法器里没有 `region`；现役机制是 stdlib 的 `pool`/`zone`（原 `region` 已改名）|
| `DECISIONS.md:29,37,38` | "### 待定：整数的算术语义"（除零 SIGFPE 无位置、移位静默算错） | 定案 ㉒ 已落地（`tests/traps/div_zero`、`shift_too_big`）|
| `DECISIONS.md:43` | "### 待定：`let` 的「浅」要不要靠可变性收口？" | 已由 ㊸ 收口（`mut ref` 落地）|
| `DECISIONS.md:180` | "方法写在 struct 体内；自由函数带 `self` 报错" | 也可写在 `impl` 块（`tests/impl/basic.extc`）|
| `DECISIONS.md:233` | "`for` 循环：没有，只有 `while`" | 四种形态已落地（定案 92，`examples/for-loops.extc`）|
| `DECISIONS.md:242` | "`+=` 等复合赋值：词法器认、parser 不认" | 已落地 |
| `DECISIONS.md:316` | "现在这个编译器…一点 extC 的安全保证都还没有" | 逃逸/深度检查已成体系（`tests/asan`、`tests/attacks`、`tests/arena*`）|
| `DECISIONS.md:2383` | "泛型体里不能再调用泛型函数" | 已落地（`examples/generic-calls-generic.extc`）|
| `DECISIONS.md:2387-2388` | "`T` 上的 `<` / `>` 还不支持" | 具体类型已支持；模板放行、实例化检查（`tests/ops/concrete.extc`）|
| `DECISIONS.md:1446` | "逃生舱（暂时不做）：`p!` 这类" | 已在定案 ㊻ 落地（`examples/sign.extc`）|
| `DECISIONS.md:2042` | "`@` 在词法里都还没有" | `@overwrite/@private/@inline/@noCopy/@sharesStorage` 都在；`@main/@recursive` 确实仍未实现 |

**这一片核过、不算过时的**（免得重复报）：`@main`/`@recursive` 确实没做（`parser.c:1090`）· `impl` 的泛型目标与
枚举方法确实不支持（`tests/impl/errors/`）· `varArray<T>` 仍不能 `for … in` · `stableSort<T>` 确实没有 ·
`PLAN §0.4` 的缺陷表刚被 2026-09-26 的审计维护过（未完成项属计划本身，不算 stale）。

**一根因**：`WHY-EXTC/DESIGN/DECISIONS/PLAN/DEVLOG` 五份的 mtime 都是 2026-09-26 20:33，而协程与
`bench/echo` 那批提交（`674f0aa`…`60c01e9`）在其后 ⇒ 这批文档整体早于协程落地，"协程未构思/未实现"
（`PLAN.md:339`、`DEVLOG.md:3618`）是同一条。

## 一之二、`16-unimplemented.md` / `08-struct.md`（审计员报告；`impl i64`、`varArray` 字段、`08:100` 三条
由 Lead 复核过）

| 位置 | 过时说法 | 代码现状 |
|---|---|---|
| `16-unimplemented.md:31` | `varArray` 定义里有 `home: ref arena` | 实际字段是 `buf`/`len`/`cap`（`stdlib/prelude.extc:162`）；`len` 是**字段**，取长度的方法叫 `size` |
| `16-unimplemented.md:52` | "第二期阶段 1、2 已落地" | 阶段 3（存储面）也已落地 |
| `16-unimplemented.md:53` | "`tests/dyn` 9 条判据" | 13 个夹具 / 22 条判据 |
| `16-unimplemented.md:54-55` | "仍缺：保存 dyn 值与字段/容器元素" | 四个存储类判据全部通过（`dyn_stored`/`dyn_in_field`/`dyn_in_array`/`dyn_in_varArray`）|
| `16-unimplemented.md:55-56` | "仍缺：孤儿规则与跨 trait 撞名的专门诊断" | 孤儿规则已落地（`src/check/check_top.c:4793` 发出 `is an orphan`，`tests/impl` 常设断言）；只剩撞名一条 |
| `08-struct.md:39` | `fn hash(self: ref i64) -> i64 { return self * i64(…) }` | 编译报 `` `ref i64` is a reference, not a value -- dereference it first: `*p` ``；改 `*self` 后编过 |
| `08-struct.md:79-82` | `fn moveBy(self: ref point, …) { self.x = … }` | 需要 `mut ref`（同一个文件 §7.0 的 `bump(self: mut ref …)` 才是对的）|
| `08-struct.md:100` | "接收者是 `ref T`、`self` 是值类型会自动解引用" | 接收者必须是 `ref T`/`mut ref T`（`fn get(self: point)` 直接报错）|
| `08-struct.md:192,193` | "尚未实现：… `dyn Trait` 值 … 孤儿规则的显式诊断" | 两者都已落地 |
| `08-struct.md:210-211` | "`dyn Tag(x)` 把载荷拷进池…经槽派发" | 立即形式**不进池**（`extc_dyn_put` 计数 0、表地址是编译期常量、无世代校验）；池+槽只属于**存下来的** dyn 值 |
| `08-struct.md:26` | 引用 "`POOLS.md` 5.4" | 该节不存在（`POOLS.md` 只有 `## 5. 边界`）|
| `08-struct.md:226` | "仍缺：`dyn` 的开放注册（第三期）" | `DYN.md` 记明第三期**不做** |
| `16-unimplemented.md:30` | "`region` 显式命名…A4 待定…死了" | 与 `PLAN.md:340` 的"判据 `tests/region/`"对不上（该目录不存在）；`region` 也不是关键字 —— 两处要一起对账 |
| `16-unimplemented.md:34` | "已完成 —— 五子棋能真的跟人下" | 仓库里只有 `examples/gomoku-board.extc`（棋盘），没有对局程序；输入侧确实通了，但这句话没有产物支撑 |

**顺带（代码侧也有过时文案）**：`src/check/check_top.c:292` 的诊断说 "`self` is only allowed in a method
declared inside a `struct`" —— `impl` 块里的方法早已合法。

## 二之二、`IO.md` / `MODULES.md` / `BOOTSTRAP.md` / `SPEC.md` / `LIBS.md` / `SOUNDNESS.md`
（审计员报告；`openRead`/`fs.extc` 的归属口径、`SOUNDNESS` 自相矛盾、四个计数由 Lead 复核过）

这一片的模式是**"功能落地了但进度段没回填"**，加上**测试计数四处不一**：

| 位置 | 过时说法 | 代码现状 |
|---|---|---|
| `IO.md:34-36` | "IO 的缺口是 `open`/归属 + `nextInt` 一族 + `reader`" | 三项都已落地（`fs.extc:242` `openRead`、`io.extc:504/532` `nextInt`/`readerOf`、`tests/io/file-reader.extc`）|
| `IO.md:117` | `slice<T>` 定义在 `prelude.extc:98` | 在 `:245` |
| `IO.md:178,202-208` | `fn readAll(f: file, …)` / `fn skipSpace(s, i)` / `fn parseInt(s)` | 没有 `struct file`；`readAll` 是 `ifstream` 的方法（`fs.extc:71`）；`parseInt` 不存在；`nextInt` 一族是 `reader` 的方法 |
| `IO.md:594,639,659` | "`f64` 还没做" / "`cout << 3.5` 仍借内建 `print`" / "`>>` 只有 `i64`" | 三者都已落地（`io.extc` 有 `<<(ostream, f64)` 与三种 `>>`）|
| `IO.md:656` | "`io::cin.bad()` 不行（PLAN #66）" | 已修（`tests/qname/t17.extc`）|
| `IO.md:702-703,739` | "还欠：`readAll`…`nextInt` 一族与 `f.reader()`" | 都已落地 |
| `MODULES.md:10,39` | "横向调研：12 门语言" | 表里是 13 门 |
| `MODULES.md:33,85` | "还没有 `extern!` / 没有 C 互操作" | 已落地（`stdlib/std/sys/io.extc:29/31/39/41`，`tests/extern/`）|
| `MODULES.md:267-277,369-375` | "泛型自由函数今天写不出来" | 已支持（`examples/generic-free-fn.extc`，`check.sh` 有专节）|
| `MODULES.md:537` | "`tests/qname/` 6 正例" | 7 个 |
| `BOOTSTRAP.md:178` | 文件 IO 白名单 = `fopen`/`fread`/`fwrite`/`fclose` | 实际是 `read`/`write`/`open`/`close`（POSIX 原语）|
| `BOOTSTRAP.md:196-198` | "`option<ref T>` 今天造不出来" | 编得过（`examples/option-ref-payload.extc`）|
| `BOOTSTRAP.md:231-234` | "后缀 `!` 还没实现" | 已实现（`src/front/parser.c` 有 `at(p, "!")`）|
| `BOOTSTRAP.md:668-670,790,794-800` | 靶子清单里一堆 `- [ ]`（输入/argv、算术 UB、全局常量、带载荷枚举、模块系统） | 都已落地（同文件 §6 自己写着"三处全部还清"）|
| `BOOTSTRAP.md:1080-1086`、`SPEC.md:381,384` | "`let v = m  return v` 被挡住（保守）" | 已修（`Sym.depth` 与 `refDepth` 已分开）|
| `SPEC.md:56,629,501-502` | "用户永远不写 `close`" / "`open()` 返回 `mut ref file`（帧 arena 里的槽位）" | 定案 79：句柄**按值**返回（`fs.extc:242` `result<ifstream, io::ioError>`），**关文件是程序的事**（`close()` 是提交点，`tests/fs/close-twice.extc`）|
| `SPEC.md:78,679,648,688-690` | "零值毒标记…欠" / "还是 struct + bool 标签" / "要补 `allocSlice<T>`" / "`option<slice<u8>>` 编不过" | 都已了结（`prelude.extc:48/60`、定案 81 已删 `allocSlice`）|
| `SPEC.md:137` | "`alloc<T>(n)` 是裸 bump 分配（不清零）" | 分配器里已 `memset(p, 0, n)`（`codegen.c:6264`）|
| `SOUNDNESS.md:4,51` | "今天 4 条成立、3 条缺" / "## 2. 今天缺的三条" | 同文件 `:45` 已写"**7 条**（O1–O7 全部）" —— 自己前后矛盾 |
| `SOUNDNESS.md:55,62,68` | "`extc_pool_reset` 不换代" / "位置：不存在" / "没有 dyn 的派发键" | 三条都已修（`src/back/pools.c` 有 `generation++`、`extc_pool_new_table`、dyn 句柄四段校验）|
| `SOUNDNESS.md:43,46,114` | "ARENA 7 个 + POOL 1 个反例" / "16 条 dyn 判据" | 实际 `tests/arena-soundness` 3 个、`tests/pool-soundness` 4 个、`tests/dyn` 23 条 |
| `SPEC.md:13-14,682` · `LIBS.md:231` · `MODULES.md:189` · `BOOTSTRAP.md:135,204` | 测试计数 255 / 116 / 257 / 82 | 当前语料 **274**（examples 97 + errors 167 + traps 10）；`check.sh` 节数 **43** —— 每处都要带日期与命令重填 |

**这一片核过、不算过时的**：`LIBS.md:18-21` 的 "所有函数默认 `static`"（生成物里仍是 `static`）·
`SOUNDNESS.md:41` 的 "17 处 `trapMsg`"（`grep -c` = 17）· `IO.md:432` 的 EMFD 上限与 `IO.md:667` 的节号
（审计员自己也没取证，留 unclear）。

## 一之三、手册其余页（审计员报告；`+=`/`for`/值位置解引用/除零 trap 四条由 Lead 复核过）

这一族是**两种滞后叠在一起**：一是 2026-09-20 定案 ㉝ 之前的旧语义（"值位置自动解引用"），二是
"还没实现的清单"没跟着回填。

| 位置 | 过时说法 | 代码现状 |
|---|---|---|
| `01-safety.md:40` | 算术无 UB"部分"：除零没位置、移位静默错（待补） | 两处都 trap 且带位置（`trap: division by zero` / `shift count out of range`）|
| `01-safety.md:103-109` | "还剩下的：算术 UB 三处"整节 | 同上，整节该删或改成"已带位置 trap" |
| `04-types.md:298-315` | "值位置自动解引用"，`fn bump(p: mut ref i64) { p = p + 1 }`，`let m: i64 = ref n` | 当值用必须写 `*p`（`reference, not a value -- dereference it first: *p`）|
| `04-types.md:321-343` | "`r = r + 5` 写穿：n 变成 15" | `=` 只有换指向一个含义；写穿写 `*p = v` |
| `04-types.md:38` | 隐式转换表把 `f64 → f32` 留空（空 = 自动） | 有损，必须显式 `f32(x)` |
| `06-expr.md:29` | "`+=` 之类的复合赋值也还没有" | 已支持（`a = b = 1` 仍不支持，那半句对）|
| `06-expr.md:73-75` | "透过 `ref` 写标量写不出来…解引用写法还是待定项" | 写标量就是 `*p = v`（`examples/ref-scalar.extc`）|
| `07-stmt.md:62` | "**还没有 `for`**" | 四种形态都已落地（同页 9-28 行就在讲它们）|
| `11-alloc.md:132-134` | "`fn make() -> mut ref node { return new node }` 是编译错误…要等 A3" | 已可返回（同页 93 行自己写着 A3 三半全齐）|
| `13-vararray.md:49` | "迭代器/范围 `for`…要等 `for` 和函数值" | `for` 已落地；差的是 `varArray` 的 `iter`（先 `asSlice()` 就能迭）|
| `17-undecided.md:9` | "还没定的"清单里含 `+=` 复合赋值 | 定案 92 已落地 |
| `18-modules.md:116` | "`T` 上的 `<` / `>` 还没有" | 具体类型已支持；模板放行、实例化时查 `fn <` |
| `18-modules.md:117` | "泛型体里不能再调用泛型函数" | 已支持（PLAN #50 已划掉）|
| `19-examples.md:29` | "`ref-scalar.extc`：自动解引用、写穿" | 该示例用的是显式 `*a = *b` |
| `19-examples.md:13` | "`tour.extc` 把现在能用的东西全用上了" | `for` 与 `+=` 都没用上 |

**这一片核过、不算过时的**：`++`/`--` 有意不做 · 没有 GC · `insertAt`/`eraseAt` 未提供 · `<<` 缺 `i32`
与结构化重载 · 格式串 `{}` 未实现 · `owned` 未实现 · `@main`/`@recursive` 未实现 ·
`20-internals.md:9` 的"862 个成员（公开 540 · 内部 322）"· `28-coro-scheduler.md` 的新写法（手册里已经没有
`[512]`/`freelist`/`sched::add`/`newTasks()` 任何一处）· `28:131` 的"只能表达等可读"（`pump` 只注册 `EPOLLIN`）。

**注意**：`docs/manual/html/*` 是生成物，同样过时（例如 `html/04-types.html` 仍含"值位置自动解引用"）；
改完 `.md` 必须跑 `python3 tools/build_manual.py` 重新生成。

## 二之三、`ARENA*` / `ARRAYS` / `INLINE-C` / `MEMORY-SAFETY` / `MIGRATION` / `AST-WALKERS`
（审计员报告；`new`/`build/stdlib.c`/walker 检查器/`dfAnalyze`/`clone_warn` 五条由 Lead 复核过）

| 位置 | 过时说法 | 代码现状 |
|---|---|---|
| `ARENA-SOUNDNESS.md:511` | "3-a…合流处什么都不做（后写覆盖前写）⇒ 改成 max" 还是待办 | 已落地：`src/check/check_dataflow.c` + `check_top.c:3839 dfAnalyze`（单调前向分析、join=max、迭代到不动点）|
| `ARENA-SOUNDNESS.md:520` | "**P0**（不做就不 sound）档 0 四条 + 1-a + 1-c…256 通过 / 1 失败" | 全部已落地（`refreshRootDepth` 取 max、元素写不再清表、`closeReach`/`bodyReaches` 收成一处；`tools/check_walkers.py` 通过；`check.sh` 的误拒白名单已是空的）|
| `ARENA.md:221` | "### 8.6 今天（`new` 还没做）能凑合到什么程度" | `new` 早已实现（`examples/new.extc` 编过；`EX_NEW` 在 `ast.h:117`）|
| `ARENA.md:318,329` | "改成：不做 E2，改做 **甲′**…落地：`raiseMutRefTargets`" | 该符号在 `src/` 里不存在（`PLAN.md:216` 记明"甲′ 的 `raiseMutRefTargets` 已删"）；现在走效果摘要 `Addr`/`Cont` + E 选家 |
| `MIGRATION.md:9`（含 11-22） | "机制（已落地，见 T4b）" —— 说是 `build/stdlib.c` 那种两阶段 | `build/stdlib.c` 不存在；实际机制是**把 `stdlib/prelude.extc` 文本内嵌**（`Makefile:18` 的 `prelude_data.c` + `tools/embed.c`）|
| `MEMORY-SAFETY.md:253` | "不做别名分析 ⇒ `let b = a` 编译器不管" | 现在会警告："copies by value but **shares storage**"（`src/check/check_escape.c` 的 `warnSharedCopy`；判据 `tests/stl/clone_warn.extc`）|
| `AST-WALKERS.md:140,156` | "修法（已落地**一半**）" / "**根治**（仍未做）" | 通用访问器已落地（`src/ast/ast.c:185/223`），`tools/check_walkers.py` 报"every recursive kind-walker handles every kind"，棘轮 29→23 |
| `ARENA-MEMORY.md:11` | "峰值 98 MB 对 50 MB"（复现物在 `/tmp/rg/r1.extc`） | 复现物不在仓库，而且它量的是**已删掉的无条件兜底**那版 ⇒ 数字要加"旧兜底版（2026-09-24 前）"限定，或把探针放进 `build/tmp` 重测 |

**这一片核过、不算过时的**：`ARENA-SOUNDNESS.md:615`（`@overwrite` 的跨调用复用确实还没做，生成物里格子在本帧）·
`:743` §12.4 是"若做 §12 必须遵守"的设计表 · `INLINE-C.md`（整个功能没做，页内自述"实现与排期不在本文范围"）·
`MEMORY-SAFETY.md:264`（确实没有别名分析）· `ARRAYS.md:20,124`（`varArray` 在 prelude 里用 extC 写；索引一律
`.data[i]`，codegen 不按上下文分支）· `ARENA-FORMAL.md:93,417`。

**顺带（代码侧注释也过时）**：`examples/overwrite.extc` 开头写"格子由**调用点的帧**持有 ⇒ 跨调用复用得上"，
与现行生成物（格子在本帧、每次调用分配一块）矛盾；`DEVLOG.md:4906` 同一件事。

**Lead 交叉排查补漏**（同一族旧说法在两个审计分片之外还有残留）：

| 位置 | 过时说法 | 代码现状 |
|---|---|---|
| `SPEC.md:475-480,673` | 整节 "§5.2 值位置自动解引用 —— 拿到的是**拷贝**" 与 "完整语义" 表里的同一行 | 与 `04-types.md:298-343` 同一族：当值用必须写 `*p`（编译报 `` `mut ref i64` is a reference, not a value…… dereference it first: `*p` ``）|
| `SPEC.md` 其余 | 见上面第三片列的 §0.5/§4.4/§5.4/§12 | 落后于第三刀、定案 79/81 与分配器清零 |

**核过不算过时的**：`docs/history/VISION.md:114` 是历史目录里的文档（`docs/history/` 已隔离）·
`manual/09-ref.md:55` 说"**输出**自动解引用，`println(r)` 打的就是值" —— 实测 `println(p)` 打出 42，**成立**；
但 `io::cout << p` 会报"reference, not a value"，即**内建 `println` 与流运算符口径不同**（改文档时别把这两条混为一谈）。

## 二之四、`HANDOFF.md` / `EXTC-SHOWCASE.md` / `SYNTAX.md` / `WARNINGS.md` / `COMMENT-STYLE.md`
（审计员报告；`result<(),E>`、`sysio::open`、`scan_cjk` 中文注释 190 条、examples 97 四条由 Lead 复核过）

| 位置 | 过时说法 | 代码现状 |
|---|---|---|
| `HANDOFF.md` 整份 | 上次会话的交接：提交清单、"本轮落点 `1fed77e`"、"工作区干净"、`#73` 的 390/943、`-fwrapv` 指向 `MANUAL.md` §8、`tests/run.sh` 的 259+263 | 那些提交已是 HEAD 的祖先（`git rev-list --count 1fed77e..HEAD` = 168）；`docs/MANUAL.md` 只剩 5 行跳转壳；`println` 现在是 396 文件 / 962 处；`#73` 的 53 处中文串早已修（现在 `scan_cjk` 报 7 条输出串）|
| `EXTC-SHOWCASE.md:170` | "运行期多态（`trait`/`dyn Trait`）与开放注册为**【计划中】**" | `tests/dyn/` 13 例，`check.sh:86` 每次跑 |
| `EXTC-SHOWCASE.md:28,62,85,107,136,154` | transcript 文件名 `s1_arena.extc`…`s6_ffi.extc` | 实际是 `examples/showcase-{arena-return,home,pool-identity,views,impl,extern}.extc` |
| `EXTC-SHOWCASE.md:51,155` | `--explain-memory` 的输出片段、`pid=<具体值>` | 真实输出行数/换行不同；`pid` 应写 `<n>`（判据断言的是 `pid>0=true`）|
| `SYNTAX.md:45,48,78,81` | `-> result<(), moveError>`、`Ok(x)`/`Err(e)` | 编译报 `expected a type, found (`；现写法是 `result<unit, E>` + `success(unit {})`/`failure(e)` |
| `SYNTAX.md:56` | C-style `for var i: i32 = 0; …` | 现在的 `for` 是 `for i in 0..n`（C-style 形态的**语法**不同），旧写法直接报错 |
| `SYNTAX.md:115` | `io::open(…)`、`io::O_*` | 裸原语在 `std::sys::io`（应写 `sysio::open` / `sysio::O_*`）|
| `SYNTAX.md:196` | "还没实现的：`std::fs` 模块本身 · 归属 · `readAll` · `f.reader()` · `close(f)!`" | 全部已落地（`fs.extc` 的 `readAll:71`/`reader:100`/`close:180,227`/`openRead:242`）|
| `SYNTAX.md:254` | "类型标注能不能省"还列在"还没定" | 能省（`var n = 0` 编过，`05-vars.md:15` 也这么写）|
| `SYNTAX.md:92,99,102` | 规划中的名字 `array<T>` / `view()` / 泛型 `string<T>` | 实际是 `varArray<T>` / `subView` / 非泛型的池底 `string`；`Utf32String`/`toBytes`/`toIntChecked` 全仓不存在 |
| `SYNTAX.md:252` | "`box<T>` 还需要吗" | 当前没有内建 `box`（`src/` 里的 `box` 全是装箱/协程句柄）⇒ 标注成"待裁决"而非现状 |
| `WARNINGS.md:78,105-106` | `-Wunused-function` 还留着运行期原语，"等第一刀（按需发射）" | 按需发射早已落地（`484b872`），生成物里那些原语一个都不出现 |
| `WARNINGS.md:51` | "全量从 39/37 降到 6/4"、examples "89 个" | 实测 97 个 examples 全量生成后 gcc/clang 都是 0 条 |
| `COMMENT-STYLE.md:10-11` | "`scan_cjk.py` 现在报 0 条"（stdlib） | 实测 **190 条**中文注释（15 个文件）|
| `COMMENT-STYLE.md:14-15` | `examples/`/`tests/`/`bench/`/`tools/` "still Chinese（~3700 行）" | 实测约 7.8k 行（`grep -P` 计 7776 行）|

**这一片核过、不算过时的**：`MANUAL.md` 是有意的跳转壳；`EXTC-SHOWCASE.md` 的六段正文由
`tests/showcase/run.sh` 在 `check.sh` 里逐字比对；`HANDOFF.md` 列的 `#92`/`#83`/`#86`/`#49` 等未完成项
复现仍在（属计划本身，不算 stale）。

## 三、历史与计划类

14. **`docs/HANDOFF.md` 会误导**：它是上一次会话的交接，写着"一、当前树的状态：全部已提交，工作区干净"
    并列出旧提交号，还写着"本文件按惯例不提交"但它其实被跟踪。建议移进 `docs/history/` 或加历史抬头。
15. 两处做法是对的，不用动：`docs/history/` 已隔离 4 份历史文档；`docs/MANUAL.md` 是有意的跳转壳。

## 四、审计顺带查出的**代码 bug**（已由 Lead 复现，优先于改文档）

1. **P0 段错误：`dyn` 派发一个没有 `self` 的 trait 方法。**
   复现：trait 里加 `fn zero() -> i64`，然后 `dyn Tag(b).zero()`（立即形式）→ `./build/extc` **退出码 139**，
   无任何诊断。成因（审计员 `gdb` 定位）：`src/check/check_expr.c` 的 object-safety 诊断（`:2260`）只 `ckError`
   不致命，随后走到"`mut ref` 接收者"检查，对零参函数取 `params[0]` 拿到 NULL。
   修法：那一支 `ckError` 之后直接返回错误类型，或把接收者检查包在 `params.len > 0` 里。**必须配判据**
   （`tests/errors/`）与一条 `dyn` 正例防回归。
2. **`ref dyn Tag` 前端收下、生成物编不过**（违反"不许接受但生成错 C"）。
   复现：`fn take(r: ref dyn Tag) -> i64 { return r.tag() }` → extc **0 诊断通过**，gcc 报
   `incompatible type for argument 1 of 'extc_dyn_slot'`（把 `ExtcDynHandle *` 按值传）。
   修法：给出明确诊断（"`ref dyn T` 暂不支持"），或按指针传。
3. **trait 泛型方法无法声明**：`fn pick<T>(self: ref Self, x: T) -> T` → `error: unknown type T`；
   `trait Tag<T>` → 语法不认。所以 `check_expr.c:2266` 的 object-safety ② 诊断是**死代码**。

## 五、结构性建议（针对"太容易搞错"）

手册现有四道闸门（公开面清单 · 覆盖计数 · 文体 · HTML 新鲜度）**都不检查"手册里的话是不是真的"**。
建议加一条：把手册里带 `fn main` 的完整示例抽出来逐个编译（有意的片段用一行标记跳过）。
这条闸门会当场抓住上面第 1、2、5 条，而不是等人读出来。

## 七之前：修复进度（2026-09-27）

- **批次 1 已完成并提交**（`da45ed6`）：`dyn` 段错误、`ref dyn Tag` 生成错 C，各配判据；
  另加"手册示例编译闸门"（`tools/check_manual_examples.py`，接进 `check.sh` 第 49 节）。
- **批次 2 已完成**（同上提交）：闸门当场抓出并修好三个编不过的手册示例。
- **批次 3 已完成并提交**（`2dc6f04`）：手册 11 页的过时句子；三处语义示例改成闸门能编译的形状 ⇒
  闸门覆盖的完整示例 6 → **10** 个。
- **批次 4 进行中**：29 处坏链接已修完并提交（`ba7ed67`，现在全仓失效 0 处）；
  `DYN.md`（22 处）与 `TRAITS.md`（6 处）的状态句已改；`PLAN.md` 的计数与抬头、`DEVLOG.md` 的抬头、
  `HANDOFF.md` 移入 `docs/history/` 并加历史抬头 —— 这些在下一笔提交里。
- **批次 4 还剩**：`IO` · `MODULES` · `BOOTSTRAP` · `SPEC` · `SOUNDNESS` · `DESIGN` · `DECISIONS` ·
  `WHY-EXTC` · `SYNTAX` · `WARNINGS` · `COMMENT-STYLE` · `MIGRATION` · `ARENA*` · `AST-WALKERS` ·
  `MEMORY-SAFETY` 各若干条（明细见上文各节表格）。

## 七、修复批次（建议顺序）

| 批次 | 内容 | 为什么这个顺序 |
|---|---|---|
| 1 | **两个代码 bug**：`dyn` 派发无 `self` 方法段错误；`ref dyn Tag` 接受却生成错 C（外加"trait 泛型方法"的明确诊断）| 只有它会伤到用户；且"接受但生成错 C"违反既定合同 |
| 2 | **手册示例编译闸门**：抽出带 `fn main` 的代码块逐个编译，有意片段用一行标记跳过 | 加完之后第 3 批的改动会被它守住；现在四道手册闸门都不检查"话是不是真的" |
| 3 | **手册 11 页**：旧语义 3 页（`04-types`/`06-expr`/`08-struct`）+ 状态未回填 8 页（`01-safety`/`07-stmt`/`11-alloc`/`13-vararray`/`16-unimplemented`/`17-undecided`/`18-modules`/`19-examples`）| 用户第一眼看到的就是手册 |
| 4 | **专题页与链接**：`DYN` 20 条 · `DECISIONS` 10 · `DESIGN` 6 · `TRAITS` 5 · `IO`/`MODULES`/`BOOTSTRAP`/`SPEC`/`SOUNDNESS` 各 4 上下 · `WHY-EXTC` 3 · `PLAN` 4 · `SYNTAX`/`WARNINGS`/`COMMENT-STYLE`/`MIGRATION`/`ARENA*`/`AST-WALKERS` 若干 · 29 处相对链接 · `HANDOFF.md` 加历史抬头 | 量大但机械；改完跑 `tools/build_manual.py` 重生成 HTML |

## 八、审计覆盖情况（收口）

8 个分片：手册 `16`/`08` · 手册其余页 · `DYN`/`TRAITS` · 设计/计划/日志 · `IO`/`MODULES`/`BOOTSTRAP`/`SPEC`/`LIBS`/`SOUNDNESS` ·
`HANDOFF`/`SHOWCASE`/`SYNTAX`/`WARNINGS`/`COMMENT-STYLE` · `ARENA*`/`ARRAYS`/`INLINE-C`/`MEMORY-SAFETY`/`MIGRATION`/`AST-WALKERS` ·
`CONCURRENCY`/`POOLS`/`POOL-SOUNDNESS`/`ARENA-NOTES`。前 7 片由审计员回报（Lead 抽样复验，复验记录见各节的备注）；
**第 8 片审计员无回报，由 Lead 自己扫完**：那四份的主要结论是**大体准确**（`hashSet`、`pool::withParent`、
`map`/`set` 的"待做"、`CONCURRENCY.md` 里成串的"已落地"注记都核对无误），只有 `POOLS.md:332` 的路径
（`stdlib/std/pool.extc` 实际在 `stdlib/std/sys/`）与 `CONCURRENCY.md:697` 的 API 写法（现在是
`tasks.pump(ref s, …)` 方法）两处小问题。

未覆盖：`docs/topics/STL.md`、`REFS.md`、`IO-BENCH.md`、`GENERICS.md`（这四份里没有状态类断言，
机械扫描 0 命中）；`docs/history/*`（有意隔离的历史）。

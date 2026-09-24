# 生成物的警告策略（gcc / clang）

> 这份文档回答一个问题：**extC 生成的 C 要不要干净？** 要 —— 而且判据是两个编译器、三档开关。

## 0. 判据

```bash
# ① gcc 的目标：0 条
gcc -Wall -Wextra -c -o /dev/null prog.c
# ② clang 的目标：0 条
clang -Wall -Wextra -c -o /dev/null prog.c
# ③ clang -Weverything 的目标：0 条，但**允许一份写在这份文档里的 -Wno- 清单**（见 §3）
clang -Weverything $(cat docs/warnings-flags.txt) -c -o /dev/null prog.c
```

`-Weverything` 的清单是**官方承认的标准做法**：clang 自己写明 `-Weverything` 是用来发现**新**警告的，
不是给产品代码用的 ✓ 所以"清单"不是偷懒，但**清单只能收 §3.1 那类"对生成物结构上无解"的类别**，
不能拿它盖住 §3.2 那类"只是还没做"的 ✗ —— 两栏分开写，就是为了不让清单变成垃圾桶 ✓

## 1. 原则：**不发**，而不是**闭嘴**

生成物少一条警告有两条路：

| | 做法 | 评价 |
|---|---|---|
| ✗ | `__attribute__((unused))` / `(void)x;` 把警告捂住 | 一行代码都没少 —— 治标 |
| ✓ | **根本别生成那行** | 行数、体积、警告一起降 —— 治本 |

主人的原话：「**有很多的代码完全没必要生成，这是 C 代码体积膨胀的主要原因**」✓
所以本项目的顺序是：**先按内容判据把没用的整段不发**，`EXTC_UNUSED` 那类权宜属性是**待撤**的（见 §4）。

## 2. 现在的实测（`-Wall -Wextra`）

| 程序 | 生成物行数 | gcc | clang |
|---|---|---|---|
| `examples/globals.extc` | 324 | **0** | **0** ✓ |
| `tests/io/stream.extc` | 738 | **0** | **0** ✓ |
| `tests/io/stream-file.extc` | 1361 | **0** | 1 |

（**2026-09-25 现场量**：上表三行 ✓ 全部 `error: 0` ✓ **89 个 examples 全量：gcc 合计 6 · clang 合计 4 · error 0** ✓
开工时同一批代表程序是 452/990/1608 行、gcc 1/14/28、clang 25/21/47，全量是 **gcc 39 · clang 37** ✓）

⇒ 三个代表程序里**两个（`globals`、`stream`）已经两编译器全清** ✓ 全量从 39/37 降到 6/4 ✓

## 3. `-Weverything` 的清单

### 3.1 结构上无解（**允许**进清单）

| 类别 | 为什么无解 |
|---|---|
| `-Wdollar-in-identifier-extension` | mangled 名 `io$cout` / `fs$ifstream_shr_...` 是**命名方案**本身 ✓ |
| `-Wreserved-identifier` | `__extc_*` 前缀是生成物的保留命名空间，故意的 ✓ |
| `-Wpadded` | 结构体布局要与描述符表（`ExtcDesc`/`ExtcField`）一致 ⇒ 填充不是自由项 ✗ |
| `-Wdeclaration-after-statement` | C89 风格，而 extC 的目标是 **C11** ✓ 语句后声明是允许的 |
| `-Wtentative-definition-compat` | 同上，C89 兼容性问题 ✗ |
| `-Wunsafe-buffer-usage` | 生成物本来就在做指针算术（arena 分配、视图索引）✓ clang 自己说这条是给 C++ 加固用的 |
| `-Wcast-align` | `extc_arena_alloc` 保证对齐，但**类型系统上看不出来** ✗ |
| `-Wswitch-default` | 生成的 `switch` 对枚举**穷尽**，故意不写 `default`（写了反而盖住新增变体 ✓） |
| `-Wcomma` | 需要顺序求值的地方用逗号表达式（C 没有语句表达式 ✓） |
| `-Wunreachable-code` / `-Wunreachable-code-return` | **clang 自己的可达性分析有误报**（它不在 `-Wall` 里，clang 文档也写明这条不是给产品代码的）。实测：预置文本里 `case EXTC_D_F32: printf(...); return;` 的 `return` 被报成"永远不会执行"，而它明显可达 ⇒ 是工具的判断问题，不是生成物的问题 |
| `-Wfloat-equal` | 语言的 `==` 作用在 `f64`/`f32` 上**就是 IEEE 相等**；clang 建议的"用 epsilon 比较"会改变语义（NaN、`-0.0` 的行为都会变）⇒ 生成物只能这么写。出现处只有结构相等的运行时（`extc_eq` 的 F32/F64 两支） |
| `-Wcast-qual` | extC 的 `ref T` 在生成的 C 里就是 `T *`（语言里没有 `const`），而描述符把操作数作为 `const void *` 交出来 ⇒ 用户的 `==` 若按 `ref` 取参，适配器必须丢掉 const 才能调用它。（若改成"先把值拷进局部再取地址"，每个适配器多一行代码 —— 这个警告不值得那一行） |
| `-Wjump-misses-init` | 警告原文是 "jump … is **incompatible with C++**"：生成物是 **C11**，epilogue 用 `goto __extc_ret;` 统一出口，跳过的都是非 VLA 的初始化。这条服务的是"C 代码也想交给 C++ 编译器"，与 extC 的目标语言无关 |

### 3.2 只是还没做（**不许**拿清单盖住，要真修）

| 类别 | 现状与出路 |
|---|---|
| `-Wunused-function` | clang 对**没用到的 `static inline`** 也报（gcc 不报 ✓）⇒ 剩的就是运行期原语（`extc_rec_enter`/`extc_modI`/`extc_divU`/`extc_modU`/`extc_arena_init`/`extc_narrowI`/`extc_convFloat` ✓）⇒ 出路：**按需发射**（第一刀 ✓） |
| `-Wunused-variable` / `-Wunused-but-set-variable` | 剩下的少数几个（`d` / `skip` / `__extc_ret_v`）要**读写判据**（"提没提到"不够 ✓） |
| `-Wused-but-marked-unused` | clang 说"你标了 `EXTC_UNUSED`，可它其实被用了" —— 这正是 §1 那批权宜属性 ✗ **撤掉即消** ✓ |
| `-Wunreachable-code` / `-Wunreachable-code-return` | trap 之后的死分支（`extc_trapMsg` 不返回，但没标 `_Noreturn`）⇒ 标上或别发 ✓ |
| `-Wjump-misses-init` / `-Wmissing-noreturn` | 生成物的控制流/出口形状问题，逐处可修 ✓ |

### 3.3 复现清单的办法

清单是**量**出来的，不是抄的：逐轮跑 `clang -Weverything`，把当轮出现的类别加进 `-Wno-`，直到归零 ✓
（2026-09-25 实测：三个代表程序各 **17 类 ⇒ 0 条**，一轮收敛 ✓）

```bash
clang -Weverything $(sed 's/^/-Wno-/;s/$//' docs/warnings-flags.txt | tr '\n' ' ') -c -o /dev/null prog.c
```

## 4. 待撤的权宜属性（㉑ 那批）

`EXTC_UNUSED`（= `__attribute__((unused))`）当初是为了让 `-Wall -Wextra` 闭嘴加的，
**一行代码都没少** ✗ 现在内容判据（`DeadDef`/`DeadFunc` + `dropUnreferenced`）能真正把没用的定义不发之后，
这批属性应当**逐个撤掉并复核警告仍为 0** ✓ 撤的顺序：运行期原语（第一刀）→ 顶层全局/描述符（已落 ✓）
→ 函数（已落 ✓）→ 剩下的属性 ✓

| 属性位置 | 状态 |
|---|---|
| 顶层全局 / 描述符行 | ✅ 已由 `dropUnreferenced` 接管（属性不再是唯一屏障 ✓） |
| 函数原型 `EXTC_UNUSED static` | ✅ **已撤**（2026-09-25 ✓）函数剪枝落地后原型不再需要它，撤掉 gcc/clang 都不动（1/8 ✓） |
| 描述符行 / 字段表 / 视图索引 `static` | ⬜ **还不能撤**（**两次实测都涨** ✓）：2026-09-25 全撤 ⇒ `stream-file` gcc 1 → 3、`stream-sum` 0 → 3、`globals` 0 → 1 ✗ 范围已钉死为这三族（`%s_desc` / `%s_fields[]` / `%s_index` ✓）—— 它们"按需发射"，但**按需 ≠ 一定被引用** ✗ ⇒ 撤属性的前提是**先给这三族接上内容判据**（照 `DeadDef` 那套；注意描述符还有一份**前置声明**（`static const ExtcDesc %s_desc;`）⇒ 和函数一样要**成对判**：名字出现次数 == 2 且两段都定位到 ⇒ 一起删 ✓） |
| 运行期原语 `static inline` | ⬜ 等第一刀（按需发射）✓ 它们同时也是 clang `-Wunused-function` 那 7 条的来源 ✓ |
| 运行期原语 `static inline` | ⬜ 等第一刀（按需发射）✓ |

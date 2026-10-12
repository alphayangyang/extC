# escape-fuzz —— `new` 离「像 C++ 堆那样用」还差多远

用**生成器系统地组合维度**，给「像用 C++ 堆那样用 `new`」的每个形状同时写一份 extC 与一份等价
C++（裸 `new`、不 `delete`），然后看 extC 收不收、收下的在 ASan 下干不干净，最后把**被拒**的形状
按判据聚类、逐类判「真误拒 / 该拒」。

- 语料 **119** 个形状，生成器见 [`gen.py`](gen.py)，生成物在 [`cases/`](cases/)。
- 跑法：`bash tests/escape-fuzz/run.sh` —— **exit 0**，6 秒左右，打出一张分类表。
- 明细：运行后写 `last-run.tsv`（每个形状的 status/类/维度/首条报错）。

## 一句话结论

`new` 本身已经能提权（`fn make() -> ?mut ref node { var p = new node; return p }` 通过），
**拦住「像堆一样用」的不是分配，而是三层记账**：

1. **调用结果的寿命被记成绑定所在的块深度**，而不是被调者拿到的 home 层 —— 「经函数返回接链」
   这一最常用的写法成批被拒（16 例）。
2. **本地 `?mut ref` 的被指寿命只按「语句之前见过的赋值」记**，读不出来就保守当 0；
   `main`/不逃逸函数没有 home，0 层不可达 —— 同一段双向链，`cur = p` 写在语句后就被拒、
   写在语句前就通过（10 例）。
3. **「返回值经投影逃逸」既不被当成逃逸函数的返回（误拒），也不触发提权（漏收）** ——
   `return bx.mid.next` 直接 `new` 会被收下但 ASan 悬垂，换成 `mk()` 又会被拒（6 例误拒 + 5 例漏收）。

## 怎么跑

```sh
bash tests/escape-fuzz/run.sh            # 基线；exit 0
python3 tests/escape-fuzz/gen.py         # 重新生成 cases/（改了维度才需要）
```

`run.sh` 对每个形状：

- `./build/extc cases/<n>.extc -o <tmp>/<n>.c`
  - 编过 ⇒ `cc -fwrapv -O1 -fsanitize=address` 编 + 跑 + 看 ASan + 与 C++ 输出比对；
  - 被拒 ⇒ 抓**完整**报错文本，按判据聚类。
- `g++ -O1 cases/<n>.cpp` ⇒ 编 + 跑（对照：**堆式写法在 C++ 里就是普通程序**，119 个全过，
  且收下的 extC 输出与 C++ 完全一致）。

口径是**基线**（记录现状，不做门禁）：语法/类型错会单独列出，不计入误拒；脚本恒 exit 0。

## 维度与规模

| 维度 | 取值 | 覆盖 |
|---|---|---|
| 结构 family | list（单链表/自引用结构体）、dlist（双向）、tree（parent↔child）、ring（环）、selfref（自引用字段成环） | 5 |
| 目的地 dest | lfield（本地引用变量的字段 `a.next`）、dfield（经解引用 `(*cur).next`）、ff（字段的字段 `bx.mid.next`）、aelem（数组元素 `arr[0]`）、glob（全局）、pfield（经 `mut ref` 参数 `(*out).next`） | 6 |
| 流动 flow | direct（`new` 后直接存）、temp（经中间变量）、ret（经函数返回 `mk()`）、param（经 `mut ref` 参数）、cont（存进 `varArray`） | 5 |
| 位置 loc | main（无 home）、home（返回引用的函数）、nohome（不逃逸的函数） | 3 |
| 赋值次序 order | before（目标引用在语句之前已赋过值）/ after（只在语句之后赋） | 2 |
| 收窄 narrow | ifnull（`if x != null`）/ old（`var o = x` 后再收窄） | 2 |

规模：119 个形状 = list 机制矩阵（全交叉）66 + 全局 3 + 该拒对照 2 + dlist/tree/ring/selfref 各 12。
每个形状都是小函数、秒级；全部**无语法/类型错**（run.sh 里该项为 0），
C++ 对照全部编过跑过、输出一致。

## 总数字（`run.sh` 输出）

```
总数            : 119
extC 通过       : 70
extC 被拒       : 44
  其中真误拒    : 40（E_DEPTH_A0_* + E_DEPTH_CALLRES + E_ARG_DEPTH）
  该拒(真悬垂)  : 1（E_DEPTH_AN）
  设计限制      : 3（E_GLOBAL；先于逃逸分析）
通过但 ASan 脏  : 5（soundness hole，方向与误拒相反）
输出不符 C++    : 0
C++ 对照没编过  : 0（编过的 119 个全部跑通）
语法/类型错     : 0
分 7 类
```

## 分类表

| 类 | 触发判据（报错文本/机制） | 样例 | 最小复现 | 该不该修 | 理由 |
|---|---|---|---|---|---|
| `E_DEPTH_A0_STORE` | `this reference would hold ... (borrowed from depth 1, but this can only hold up to depth 0)` | 14 | [main_dfield_after.extc](min/main_dfield_after.extc) | **真误拒** | 见 §1 |
| `E_DEPTH_CALLRES` | `... (borrowed from depth 2, but this can only hold up to depth 1)` | 16 | [callres_ret_store.extc](min/callres_ret_store.extc) | **真误拒** | 见 §2 |
| `E_DEPTH_A0_RET` | `this return value would hold ... (borrowed from depth 1, but this can only hold up to depth 0)` | 6 | [home_ff_return.extc](min/home_ff_return.extc) | **真误拒**（且伴随漏收） | 见 §3 |
| `E_ARG_DEPTH` | `argument 2 of \`link\` points into a deeper scope (depth 2) than the arena this call may store it in (depth 1)` | 4 | [param_ret_arg.extc](min/param_ret_arg.extc) | **真误拒**（保守） | 与 §2 同根 |
| `E_DEPTH_AN` | `... (borrowed from depth 2, but this can only hold up to depth 1)`，值**不是**调用结果 | 1 | [deep_store.extc](min/deep_store.extc) | **该拒** | 真会悬垂 |
| `E_GLOBAL` | `cannot zero-initialize the global \`g\`: it contains a reference` | 3 | [global_ref.extc](min/global_ref.extc) | **该拒**（语言规则） | 见 §4 |
| `E_ACCEPT_UAF` | 收下了，但 ASan `heap-use-after-free` | 5 | [home_pfield_uaf.extc](min/home_pfield_uaf.extc) | **漏收，必须修**（方向相反） | 见 §5 |

> 还有一条判据 `cannot store a borrowed value into something that outlives this call`
> 在本语料里**没有样本**：它要求「目的地层 0 + 借来的值 + 目的地不是参数 + 函数没有 home」，
> 而本语料的 `new` 形状不满足最后一条（经参数存储的函数按规则都会拿到 home）。列在这里是为了
> 说明它没被漏掉，只是 `new` 这条路碰不到。

### §1 `E_DEPTH_A0_STORE`（14；10 例是决定性那条）

**最小复现（决定性实验）**：[`min/main_dfield_after.extc`](min/main_dfield_after.extc)
—— 与 `docs/usage/language.md` §8.1 的 `build` 双向链同构，只放进 `main`：

```extc
var head: ?mut ref node = null
var cur:  ?mut ref node = null
while i < 3 {
    var p: mut ref node = new node
    ...
    if head == null { head = p } else { if cur != null { (*cur).next = p } }
    cur = p                // ← 这一句在语句**之后**
}
```

报错：`(*cur).next = p` → `borrowed from depth 1, but this can only hold up to depth 0`。

**判「真误拒」的证据（同一语义、只动词序）**：把 `cur = p` 挪进 `if head == null` 分支
（语义不变，链一样、输出一样 10），`cases/m_main_dfield_direct_before` **通过且 ASan 干净**；
`cases/m_main_dfield_direct_after` 被拒。`nohome` 位置同理（`m_nohome_*`）。
所以被拒与「程序会不会悬垂」无关，只与「分析在走到语句时有没有见过对 `cur` 的赋值」有关。

机制：`storeLayer((*cur).next)` 走 `slotDepth` → `targetDepth(cur)`；本地 `?mut ref` 的
`anRefDepth` 在语句之前还是 0，`at = 0`。`at == 0` 的含义是「活过本帧 = 调用者的家用 arena」，
而 `main`/不逃逸函数没有调用者 ⇒ 没有 home ⇒ 0 层不可达
（`src/check/check_escape.c:1007` 的 `at == 0 && !anNeedsHome`），直接拒。
程序其实不悬垂：`cur` 只可能在第一次循环之后非空，指向的是同一帧里已提权到 1 层的节点。

**修法方向（任一即可覆盖这 10 例）**
- 本地引用的被指寿命按**整个函数**的赋值取上界（定点/流不敏感），而不是只看到语句之前；
- 或在 `at == 0` 且无 home 时，若目的地的根是本帧里由 `new` 建起来的对象，就把位降到本帧层 1。

### §2 `E_DEPTH_CALLRES`（16）

**最小复现**：[`min/callres_ret_store.extc`](min/callres_ret_store.extc) —— 链节点由一个
带 home 的被调者 `mk()` 返回，经中间绑定接上链：

```extc
while i < 3 {
    var p: mut ref node = mk(i)      // ← 调用结果落到中间绑定
    if head == null { head = p } else { if cur != null { (*cur).next = p }  cur = p }
    i = i + 1
}
```

报错：`head = p` → `borrowed from depth 2, but this can only hold up to depth 1`。

**判「真误拒」的证据**：同一个调用点**不落中间绑定**就直接存（`head = mk(1)`）编得过
（本目录 `cases/m_*` 里 `m_main_*_ret` 的对照；`tests/arena-promoted/alloc_return_local.extc`
也是这个道理）。所以问题在「`var p = mk(i)` 把 p 的寿命记成 p 所在块的深度（循环体 2 / 函数体 1）」，
而不是被调者分配的节点真会死在那里 —— 调用点的 arena 是编译器选的那只（home）。

`E_ARG_DEPTH`（4 例）是同一根因的另一个出口：把同一个 `p` 传给会存下它的被调者
（`link(ref *a, p)`），实参被判成「指向块内局部」而拒绝。

**修法方向**：调用结果的 `refDepth` 不能取绑定的槽深度，要取**调用点选定的 arena 层**；
并把随后对 `p` 的 store/return 的提权要求回传到调用点（现在只有「直存」那条路会回传）。
修好这一条，`E_DEPTH_CALLRES` + `E_ARG_DEPTH` + `E_DEPTH_A0_STORE` 里的 home 例一起消失。

### §3 `E_DEPTH_A0_RET`（6）+ 漏收（2）—— 同一个盲点

**最小复现**：[`min/home_ff_return.extc`](min/home_ff_return.extc)

```extc
fn build() -> ?mut ref node {
    var bx: outer = { mid: { next: null } }
    var p: mut ref node = mk(7)
    bx.mid.next = p
    return bx.mid.next           // ← 返回值经「字段的字段」投影
}
```

报错：`this return value would hold a reference to a local variable that dies first
(borrowed from depth 1, but this can only hold up to depth 0)`。
`return arr[0]`（数组元素投影）同样被拒（`m_home_aelem_*` 3 例）。

**同一个盲点的反面**：把 `mk(7)` 换成直接 `var p = new node`，形状一模一样
（[`cases/m_home_ff_direct.extc`](cases/m_home_ff_direct.extc)），extC **收下**了，
生成物里 `build` 没有 `__extc_home`，`new` 落在 `build` 自己的帧 arena，
`main` 读返回值时 ASan 报 `heap-use-after-free`。也就是说：

| 形状 | 节点来自 | 结果 |
|---|---|---|
| `return bx.mid.next` | `new` 直接 | 收下 → **ASan UAF** |
| `return bx.mid.next` | `mk()` 返回 | **拒**（A0_RET） |
| `return arr[0]` | `new` 直接 | **拒**（A0_RET） |

三种写法是同一件事，编译器给了三种不同答案 —— 判据在「返回值经投影」这件事上既不保守也不一致。

**判「真误拒」但必须和漏收一起修**：`home_ff_return` 若被直接收下，按现在的代码生成会悬垂，
所以**单独放开是错的**。正确地修是：`needsHome` 与返回值深度都要穿透投影（`EX_FIELD`/`EX_INDEX`）
追到「本帧的 `new`」，让 `build` 拿到 home、`new` 提到 home，并让 home 沿 `mk()` 转发。
改完这 6 例误拒与 2 例漏收同时消失。

### §4 `E_GLOBAL`（3）—— 该拒，但也是拦路石

**最小复现**：[`min/global_ref.extc`](min/global_ref.extc) → `cannot zero-initialize the global
\`g\`: it contains a reference`。`g_struct_global`/`g_array_global` 同理。

全局是层 0 的静态存储，含引用的全局**根本声明不出来**，这条规则在逃逸分析之前。
判定：**该拒**（现在的代码生成没法保证全局指向的存储活到进程结束）；
但它同时是「`new` 像 C++ 堆」这条路上一块明确的拦路石：C++ 里 `g = new node` 是最普通的写法。

### §5 `E_ACCEPT_UAF`（5）—— 方向相反的发现

`run.sh` 里 ASan 抓到 5 例「extC 收下、生成物悬垂」：

| 形状 | 所在函数有 home？ | ASan |
|---|---|---|
| `home_cont_after` / `home_cont_before`（存进 varArray 再取出接链） | **有** | heap-use-after-free |
| `home_pfield_direct`（`link3(ref *a, p)` 接链后 `return head`） | **有** | heap-use-after-free |
| `home_ff_direct` / `home_ff_temp`（`return bx.mid.next`，节点来自 `new`） | **没有** | heap-use-after-free |

最小复现 [`min/home_pfield_uaf.extc`](min/home_pfield_uaf.extc)：函数**有** home 参数，
但循环里的 `new node` 仍被放进函数自己的帧 arena（`extc_arena_alloc(&__extc_a[1], …)`），
函数返回时 `extc_arena_destroy` 释放，`main` 立刻读到已释放内存。

判定：**漏收，必须修**；方向不是「放开」而是「把提权落到 home」或「拒」。
这一类与 §3 是同一个盲点的两面，建议一起修。

## 前三类「误拒程度 × 常见程度」排序与修法方向

| 排名 | 类 | 误拒程度 | 常见程度 | 修法方向 |
|---|---|---|---|---|
| 1 | `E_DEPTH_CALLRES`（+`E_ARG_DEPTH`） | 高：同一调用点直存就过、经中间绑定就拒 | 高：「工厂函数造节点再链接」是最常用的写法 | 调用结果的深度取**调用点选定的 arena 层**，不取绑定槽深度；把 store/return 的提权要求回传到调用点 |
| 2 | `E_DEPTH_A0_STORE`（main/nohome，order=after） | 中高：判据对**词序**敏感，同语义 before 通过 | 高：「写回已有节点」是双向链/树/环的标准写法 | 本地引用被指寿命按全函数定点取上界；或 `at==0` 无 home 时降到本帧层 1 |
| 3 | `E_DEPTH_A0_RET`（含同盲点的 5 例漏收） | 高：`return box.slot` / `return arr[i]` 被拒 | 高：包装/取回刚建好的节点 | 返回值与 `needsHome` 都穿透投影追到本帧 `new`；home 沿被调者转发；**与漏收一起修** |

## `new` 像 C++ 堆这条路上的拦路石（按性质）

1. **调用链记账**（§2）：函数返回的引用被记成「块深度」，把「返回新节点」这条最像堆的路径堵死。
2. **词序敏感的引用寿命**（§1）：`cur` 的寿命取决于分析走到语句时见没见过赋值，而不是程序事实。
3. **返回经投影不认**（§3）：`return bx.mid.next` / `return arr[0]` 既误拒又漏收，答案还不一致。
4. **0 层缺 home 就不可达**（§1、§3）：`main` 作为入口天然没有 home，任何被读成「要活过本帧」的
   本地形状都会在 `main` 里翻车（而 `main` 正是用户最先试写的地方）。
5. **全局装引用直接不允许**（§4）：语言规则，先于逃逸分析；C++ 里最常见的 `g = new node` 写不出来。
6. **提权没落到 home**（§5）：有 home 也未必用上，说明「分配放置」与「检查结论」之间有裂缝。

## 目录

| 文件 | 说明 |
|---|---|
| [`run.sh`](run.sh) | 入口（755）：`bash tests/escape-fuzz/run.sh`，exit 0 |
| [`harness.py`](harness.py) | 跑语料、抓报错、聚类、打表（644） |
| [`gen.py`](gen.py) | 生成器（维度 × 骨架 → extc + 等价 C++）（644） |
| [`cases/`](cases/) | 119 组 `*.extc` / `*.cpp` + `index.tsv`（644） |
| [`min/`](min/) | 每类一个最小复现 + `manifest.tsv`（run.sh 会核对，当前 9/9 一致） |
| `last-run.tsv` | run.sh 的输出明细（每形状 status/类/维度/首条报错；运行时重新生成） |

再生成语料：`python3 tests/escape-fuzz/gen.py`（会清空并重写 `cases/*.extc`、`*.cpp`、`index.tsv`）。

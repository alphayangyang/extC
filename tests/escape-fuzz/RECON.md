# RECON —— 7 类误拒/漏收的根因地图与成组修复计划（task-9）

**只勘察，不修**。本文件与 [`recon/`](recon/) 是全部交付物；仓库 `src/` 一行未改
（可用 `git status --porcelain` 验证：只有 `tests/escape-fuzz/` 未跟踪文件）。

## 怎么复跑

```sh
bash tests/escape-fuzz/recon/run-probes.sh      # 约 5–15 秒，exit 0
```

它把每条实验的**原始输出**写到 `recon/out/`：

| 文件 | 内容 |
|---|---|
| [`q1-callres.txt`](recon/out/q1-callres.txt) | Q1：调用结果深度、调用点传了哪只 arena、`promoteInto2` 的 gate |
| [`q2-ordering.txt`](recon/out/q2-ordering.txt) | Q2：before/after、dead-assign 对照、DFA 答案、`[defer]` |
| [`q3-projections.txt`](recon/out/q3-projections.txt) | Q3：投影真值表、minAt/home、`[fs]`、5 例 UAF 的生成物 |
| [`q4-dualpointee-hole.txt`](recon/out/q4-dualpointee-hole.txt) | 反向发现：**新**的漏收形状 |
| [`q5-excc-dbg-survey.txt`](recon/out/q5-excc-dbg-survey.txt) | `EXTC_DBG=1` 下 12 例 rc=134（既存兜底） |
| [`q6-risk-surface.txt`](recon/out/q6-risk-surface.txt) | 闸门/基线/语料清单 |
| [`q7-citations.txt`](recon/out/q7-citations.txt) | 本文件引用的每个函数/判据的 `grep -n` 行号 |

探针在 [`recon/probes/`](recon/probes/)（20 个 `.extc`）。每条结论标 **【实测】**（有命令与输出）、
**【代码】**（有文件:行）、**【推断】**（由前两者推出、未直接观测）。没查清的直接写「未查清」。

---

## 0. 结论速览

| escape-fuzz 类 | 根因位置（文件:函数/判据） | 今天修对了吗 | 与谁耦合 |
|---|---|---|---|
| `E_DEPTH_CALLRES`(16) + `E_ARG_DEPTH`(4) | `check_stmt.c:481-499` 把绑定深度写成 `scopes.len`；`check_top.c:1439 markCallHomeIfEscaping` 同一步把调用点 arena 也钉在块层；`check_escape.c:1134-1161` 的 `promoteInto2(EX_CALL)` 只认建池调用 ⇒ **要求回不到调用点** | 拒是**安全**的（值真的活在块层）；按「new 像堆」的设计意图是误拒 | 接受 + arena 回传**必须同轮** |
| `E_DEPTH_A0_STORE`(14，其中 10 是同一条) | `check_stmt.c:673 at0 = storeLayer(target)` → `check_escape.c:125` → `targetDepth(cur)` → `anRefDepth(cur)`，而它在 body walk 中**按词法顺序**写（`check_stmt.c:498/689`），无 join | **真误拒**（同语义 before 版通过且 ASan 干净） | 目的地深度的**极性**：min 而不是 max（见 §4） |
| `E_DEPTH_A0_RET`(6) + `E_ACCEPT_UAF`(5) | `check_escape.c:1097-1100` 投影走 obj；`1062` `!sy->origin` 提前返回跳过 `promoteFieldsAt`；`check_top.c:2200 noteFieldSrc` 只写已存在的槽、槽名只取最后一段；`check_top.c:6099` 放置按 `minAt` 判 HOME/块层 | A0_RET 是误拒；5 例是**漏收**（最高优先级） | 三件事同一个决定，必须同轮 |
| `E_DEPTH_AN`(1) | 深度规则本职 | **该拒** | 无 |
| `E_GLOBAL`(3) | `check_stmt.c:469-480` 语言规则（全局=层 0，含引用不能零初始化） | 该拒（设计） | 与逃逸分析无关 |

---

## 1. Q1：`E_DEPTH_CALLRES`(16) + `E_ARG_DEPTH`(4) 的根因

### 1.1 现象（可复跑）

```
$ bash tests/escape-fuzz/recon/run-probes.sh     # 见 recon/out/q1-callres.txt
q1_direct   (head = mk(1))                    extc_rc=0  run CLEAN stdout=1
q1_binding  (var p = mk(1); head = p; return p) extc_rc=1  ... up to depth 0   ← A0_RET
q1_loop     (var p = mk(i); head = p; ...)      extc_rc=1  (depth 2 ... up to depth 1) ← CALLRES
q1_frame_nonescape   (var p = mk(5), 帧层)      extc_rc=0 生成物: mk(5, &__extc_a[1])
q1_inloop_nonescape  (var p = mk(i), 循环里)    extc_rc=0 生成物: mk(i, &__extc_a[2])
q1_homefn_loop       (有 home 的函数里同样写法) extc_rc=0 生成物: mk(i, &__extc_a[2])
```

**【实测】** 关键对比：同一个 `mk` 调用，`head = mk(1)`（直接存）生成 `mk(1, __extc_home)`；
`var p = mk(1)`（经绑定）生成 `mk(5, &__extc_a[1])`；`var p = mk(i)`（循环里）生成 `mk(i, &__extc_a[2])`。
**有 home 的函数也不例外**（`q1_homefn_loop` 仍是 `&__extc_a[2]`）。

**【实测】** `promoteInto2` 的调用分支拒绝非建池调用：

```
$ EXTC_DBG_ZONE=1 ./build/extc recon/probes/q1_loop.extc -o /dev/null 2>&1 | grep 'gate] call=mk'
[gate] call=mk ... mP=0 cmp=0 lvl=2 at=2
[gate] call=mk ... mP=0 cmp=0 lvl=2 at=1
```

`cmp=0` = `calleeMakesPool(mk)==false` ⇒ `check_escape.c:1150` 的条件不成立 ⇒ 返回 `false`。

### 1.2 机制（文件:行）

1. **调用点 arena 从哪来**：`check_expr.c:3256-3257` 在每次调用处调
   `callHomeDepth`（`check_top.c:1556`，只读 `mut ref` 实参）与 `setCallArenaArg`
   （`check_top.c:1519`）。没有 `mut ref` 实参时 `homeDepth=0` → 走 else 分支：
   `arenaArg = c->scopes.len` + `arenaArgPending=true`。
2. **var 声明会把上面那一步覆盖掉**：`check_stmt.c:415`
   `markCallHomeIfEscaping(c, s->u.var.init, (int)c->scopes.len)`；
   `check_top.c:1435-1440` 里 `anSetHomeDepth(v, (at < scopes.len) ? -1 : scopes.len)`
   —— 传进来的 `at` 恰好等于 `scopes.len`，于是取 `scopes.len`（不是 `-1`/HOME），
   `setCallArenaArg` 走 `homeDepth>=1` 分支把 `arenaArg` 钉成块层。
   **这就是 `mk(i, &__extc_a[2])` 的来源。**
3. **直接存不走第 2 步**：`head = mk(1)` 是「引用类型目标接收引用」的赋值分支
   （`check_stmt.c:668-721`），它在 721 行 `return`，不经过 `noteStore`，也不经过
   `markCallHomeIfEscaping`；站点保持 `arenaArgPending`，末轮
   `check_top.c:6150-6158` 看到 `planUsesHome(mk)` → `ARENA_HOME` → `__extc_home`。
4. **绑定深度**：`check_stmt.c:481-499`：`d = targetDepth(init)`（调用结果没有带引用的实参
   ⇒ `targetDepth` 的 EX_CALL 分支给 0）；然后
   `if (callee needsHome && scopes.len > d) d = scopes.len;`，`anSetRefDepth(sym, d)`。
   注释自己写明「被调方有 home arena 时用我的」。
5. **要求回不去调用点**：后续 store/return 读 `targetDepth(p)=anRefDepth(p)=scopes.len`，
   调 `promoteInto(p, at)` → 沿 `sy->origin`（= 调用表达式）走到
   `promoteInto2(EX_CALL)`（`check_escape.c:1134-1161`），非建池调用直接 `false`。
   没有任何 `LvlFact` 落到调用表达式上（`recordLvlFact` 只在 `hops==0` 且站点是
   `new`/`alloc` 时才写层号）。

### 1.3 判定：**今天拒是安全的一侧**，但按设计意图是误拒

- 【实测】`q1_inloop_nonescape` / `q1_homefn_loop` 的生成物把 `mk(i, …&__extc_a[2])` 指到循环块
  ⇒ 节点每轮随块释放。此时把 `p` 存进帧层（深度 1）**真的会悬垂**
  ⇒ 当前拒绝是正确/安全的。
- 【推断】但这与设计意图（`docs/usage/language.md` §8.1「逃逸提升」）冲突：调用结果的 arena
  本可以由调用点选（编译器已有 `ARENA_HOME` 通道，直接存那条就走通了）。缺的是
  **把 store/return 的层要求回传到调用点**。
- ⚠️ escape-fuzz README 把这一类写成「真误拒」需要修正为：
  **按设计意图是误拒，按当前生成器是安全拒**；只放宽深度不改 arena = 造 16 个 UAF。

### 1.4 修法方向（细节见 §7 步骤 2）

在 `promoteInto2` 的 `EX_CALL/EX_METHOD/EX_ASSOC` 分支里，对
`anNeedsHome(planCallee(val))`（或更精准的 `planUsesHome`）的调用，把 `at` 记成该**调用站点**的
要求（`anSetMinAt`/一张新的事实表），末轮放置时把 `arenaArg` 压到 `at`（`at==0` → `ARENA_HOME`）。
只把生命周期往长里挪，是安全方向。VAR_DECL 的 `markCallHomeIfEscaping(init, scopes.len)` 要么删掉、
要么改成「先按 pending，等事实表定」——否则它在前面就把 arena 钉死了。

---

## 2. Q2：`E_DEPTH_A0_STORE`(14) 为什么**词序敏感**

### 2.1 现象与决定性对照（可复跑）

```
m_main_dfield_direct_before   extc_rc=0  ASan CLEAN  stdout=10      ← 通过
m_main_dfield_direct_after    extc_rc=1  (depth 1 ... up to depth 0) ← 拒
q2_after                      extc_rc=1
q2_after_deadassign           extc_rc=0  ASan CLEAN  stdout=6       ← 通过
```

**【实测】** 两个探针的 diff 只有一行（见 `q2-ordering.txt`）：

```
10a11
>         if i < 0 { cur = p }
```

这条语句**运行期永不执行**。加在 store **之前**，被拒的形状就通过 ⇒ 决定性输入是
「分析走到 store 时有没有见过对 `cur` 的赋值」，不是程序语义。

**【实测】** 顺序无关的答案已经算出来了，只是来得太晚：

```
$ EXTC_DBG_DFA=1 ./build/extc cases/m_main_dfield_direct_{before,after}.extc -o /dev/null
[dfa] main: 4 vars
       p depth=1   head depth=1   cur depth=1      ← 两个形状都一样
```

**【实测】** `EXTC_DBG_DEFER=1` 的复核也不会翻案，因为它只比值、不比目的地：

```
[defer] line=15 at=0 check=1 settled=1 agree
```

### 2.2 机制（文件:行）

1. 目的地深度：`check_stmt.c:673 at0 = storeLayer(target)`；`storeLayer`（`check_escape.c:178-192`）
   对 `(*cur).next` 走 `slotDepth`；`slotDepth(EX_FIELD)=slotDepth(obj)`（147），
   `slotDepth(EX_DEREF)=targetDepth(operand)`（**125**）；`targetDepth(cur)` 对
   `EX_IDENT` 取 `anRefDepth(sy)`（`check_escape.c:443-449`）。
2. `anRefDepth(cur)` 在 body walk 里**按词法顺序**被写：
   - 声明时 `check_stmt.c:481-499`（`var cur: ?mut ref node = null` → 0）；
   - 引用赋值时 `check_stmt.c:689 anSetRefDepth(slot0, targetDepth(c, v))`。
   没有 join、没有定点、也不回看后面的语句。
3. 顺序无关的版本在 `check_dataflow.c`（头注释：`join 取最大`），由 `check_top.c:4206 dfAnalyze`
   在**函数体检查完之后**调用，写回只升不降（`4256`/`4267`）。而诊断
   （`check_escape.c:1482`）在 walk 里已经发出；`recheckLevelRejections`
   （`check_escape.c:900-911`）注释明说**不改变已报出的诊断**，而且它比的是 `lr->val`
   （值）的深度，不是目的地的 `at`。

### 2.3 判定：**真误拒**（有对照）

`before` 与 `after` 语义相同（同一链表、同一输出 10），`before` 被接受且 ASan 干净、DFA 给出
同样的 `cur=1` ⇒ `after` 拒绝是记账次序造成的误拒（10/14 例属这一类；其余 4 例是 home 函数里
的调用结果，根因回到 §1）。

### 2.4 修法方向：**极性必须先想清楚**（这是本任务最重要的耦合，见 §4）

`at` 要的是「目的地可能指向的存储里**活得最久**的那一层」= 各次赋值深度的 **min**；
而 DFA/`anRefDepth` 现在算的是 **max**（上界）。**不能直接把 DFA 的数搬来做 `at`**。
正确方向二选一：
- 为「目的地的可能被指物深度」单独做一次 **min/可能集** 分析（或对 `cur` 的每次赋值取最小深度），
  在 store 判定时读它；
- 或者把这条诊断**推迟**到所有事实定下之后重判（`recordLvlRejection` 已经在收集数据，
  需要扩展它同时记 `at` 的两个来源并允许撤销/重发）。

---

## 3. Q3：`E_DEPTH_A0_RET`(6) + 5 例 `E_ACCEPT_UAF` —— 投影与放置

### 3.1 「同一件事三个答案」的真值表（全部 【实测】，见 `q3-projections.txt`）

| 探针 | 写法 | extC | ASan |
|---|---|---|---|
| `F_direct` | `var p = new node; return p` | 收下 | CLEAN |
| `G_bind` | `bx.mid.next = p; return p`（返回绑定本身） | 收下 | CLEAN |
| `A_ff1_new` | `bx.next = p; return bx.next`（一层字段） | 收下 | **CLEAN** |
| `B_ff2_new` | `bx.mid.next = p; return bx.mid.next`（两层字段） | 收下 | **heap-use-after-free** |
| `B2_preexisting` | 同 B，但 `outer` 另有一个同名顶层字段 `next` | 收下 | CLEAN |
| `C1_two_stage` | 同 B，但先 `bx.mid.next = null` 再写 p | 收下 | **heap-use-after-free** |
| `H_ff1_mk` | `bx.next = mk(7); return bx.next` | **拒** | — |
| `C_ff2_mk` | `bx.mid.next = mk(7); return bx.mid.next` | **拒** | — |
| `D_arr_new` | `arr[0] = p; return arr[0]` | **拒** | — |
| `E_arr_mk` | `arr[0] = mk(7); return arr[0]` | **拒** | — |

两个**独立**的因子决定「字段的字段」该怎么办（`[fs]` 与 `[minAt]`/`[home]` 一起看）：

| | `bx` 有 origin？ | 写时槽 `next` 存在？ | `noteFieldSrc` 记到 src？ | `minAt` | build `uses` | 结果 |
|---|---|---|---|---|---|---|
| A_ff1 | 是（结构体字面量） | 是 | **是** | 0 | 1（有 home） | CLEAN |
| B_ff2 | 是 | 否（声明只建了 `mid`） | **否** | 1 | 0（无 home） | UAF |
| B2 | 是 | 是 | **是** | 0 | 1 | CLEAN |
| C1 | **否**（`var bx: outer` 无初始化式） | 是（第一次写建的） | **是** | 1 | 0 | UAF |

```
$ EXTC_DBG_FS=1 ./build/extc recon/probes/<p>.extc -o /dev/null   # [fs] 行数
A_ff1_new        fs-lines=1  [fs] bx.next d2=0 oldSrcDepth=0 srckind=4   (4 = EX_IDENT, 即 p)
B_ff2_new        fs-lines=0
B2_preexisting   fs-lines=1  [fs] bx.next d2=0 oldSrcDepth=0 srckind=4
C1_two_stage     fs-lines=1  [fs] bx.next d2=0 oldSrcDepth=0 srckind=4
```

### 3.2 机制（文件:行）

1. **投影怎么走**：`promoteInto2` 的 `EX_FIELD`/`EX_INDEX` 只沿 **对象** 走
   （`check_escape.c:1095-1100`：`return promoteInto2(c, val->u.field.obj, …)`），
   不会读到「这个字段里现存的是什么」。字段里存的分配要靠字段表：
   `promoteFieldsAt`（在 `EX_IDENT` 分支里，`check_escape.c:1080`）。
2. **因子一：`!sy->origin` 提前返回**（C1）：`check_escape.c:1062
   if (!sy || !sy->origin) return false;` —— 在 **1080** 调 `promoteFieldsAt` **之前**返回。
   `var bx: outer`（无初始化式）→ `noteOrigin(c, sym, NULL)` → `origin==NULL`。
   于是即便字段表里有 `src`，也没有人去提权 ⇒ `minAt` 停在 1（来自 store 的目的地层）。
3. **因子二：`noteFieldSrc` 只写已存在的槽**（B）：`check_top.c:2200-2215` 只在
   `root->fields[i].name == field` 时记录 `src`；槽名取**最后一段字段名**
   （`check_stmt.c:708 fn_ = target->u.field.name`，`noteStore` 里 `check_escape.c:847-849` 同理）。
   `var bx: outer = { mid: { next: null } }` 的声明只给 `bx.fields["mid"]` 建了槽
   （`check_stmt.c:503-512`），所以 `bx.mid.next = p` 找不到名为 `next` 的槽 ⇒ `src` 为空 ⇒
   `promoteFieldsAt` 无物可提。
4. **元素写永远记不到 src**（D/E）：`noteFieldSrc` 开头 `if (!root || !field || !src) return;`
   （`check_top.c:2201`）—— 元素写传 `field=NULL`（`noteStore`→`noteFieldDepthWrite(vs,NULL,d2)`），
   永远不记；`promoteFieldsAt` 的 `otherSrc` 半（`check_escape.c`）只为「被调者经 `mut ref`
   写入」而设（`check_top.c:1401`）。于是 `targetDepth(arr[0])` 回落到
   `slotDepth(EX_INDEX)` = 数组槽深度 1 ⇒ `return`（at=0）拒。
5. **调用结果同样提不动**（H/C_ff2_mk）：字段表里即便有 `src=p`，`p` 的 origin 是调用表达式，
   `promoteInto2(EX_CALL)` 对非建池调用返回 false（§1）⇒ 深度停在 1 ⇒ 拒。
6. **放置是哪一个 pass**：`check_top.c:6071-6128` 逐站点：
   `anMinAt(site)==0 → ARENA_HOME`；`>=1 → 那个块层`；没有约束才用 `anLexicalLevel`。
   然后 `anSetRefDepth(site, arenaDepthOf(want))`（6112）。
   `usesHome` 闭包（`closeReach(&closeCtx, REACH_USES_HOME, …)`，6136）与 6150-6158 那条
   「pending 且 callee `planUsesHome` → ARENA_HOME」决定**发不发 home 参数**。

```
$ EXTC_DBG_MINAT=1 EXTC_DBG_HOME=1 ./build/extc cases/<n>.extc -o <n>.c
m_home_pfield_direct  build: line7 minAt=0 L-1 | line13 minAt=1 L1 ; uses=1 needs=1 sites=2 L-1 L1
                      gen:  a → (*__extc_home) ;  p → __extc_a[1]      ← UAF
m_home_cont_after     build: line7 minAt=1 (lexi=2) → L1 ; uses=1 sites=4 L0 L1 L0 L0
                      gen:  p → __extc_a[1]                            ← UAF
m_home_ff_direct      build: line7 minAt=1 → L1 ; uses=0 needs=1 sites=1 L1
                      gen:  static node * build(void) ; p → __extc_a[1] ← UAF
```

- `pfield`/`cont`：**函数有 home**（`uses=1`），但逃逸的那个站点 `minAt=1` ⇒ 放在**自己的帧块**
  `__extc_a[1]`。原因：`return head` 的 `promoteInto(head,0)` 只沿 `head` 的 `origin` 走，而
  `head` 的 origin 在 `noteOrigin` 里被**展平**成第一个 `new`（`check_escape.c:929-947` +
  `originOf`）；循环里 `a = p` 更新的是 `a` 的 origin，`head` 的那份不会跟着变
  （`check_stmt.c:695`），于是循环里的 `new` 站点不可达 ⇒ 只有第一个站点被提到 HOME。
- `ff`：唯一站点 `minAt=1` ⇒ 没有站点落在 HOME ⇒ `usesHome=0` ⇒ 生成 `build(void)`；
  codegen 读 `planArenaLevel`（`src/back/codegen.c:3175/3305` 的 `arenaRefAt`）把它翻成
  `__extc_a[1]`，函数返回时 `extc_arena_destroy` 释放 ⇒ UAF。

### 3.3 判定

- **A0_RET(6)**：真误拒（`return bx.mid.next` / `return arr[0]` / `return head`（来自 `mk`）本该成立）。
- **5 例 UAF**：漏收，**优先级最高**，且与 A0_RET 是**同一个决定的两面**：
  接受 A0_RET 之前必须让这些站点真正落到 home。单独放开 = 直接开 UAF。

---

## 4. 反向发现（**新**）：目的地深度用 max-join 会漏收

`recon/probes/q2_dualpointee.extc`：`cur` 可能指向**参数**（调用者的存储，深度 0），
也可能指向帧内 `new`（深度 1）；store 的目的地是 `(*cur).next`。

```
$ ./build/extc recon/probes/q2_dualpointee.extc -o /tmp/x.c   # rc=0（接受！）
$ cc -fwrapv -O1 -fsanitize=address ... && /tmp/x
ERROR: AddressSanitizer: heap-use-after-free
$ EXTC_DBG_DFA=1 ./build/extc recon/probes/q2_dualpointee.extc -o /dev/null | grep -A4 '\[dfa\] f:'
[dfa] f: 3 vars
       p depth=1   cur depth=1   q depth=1
```

**【实测】** 编译器接受；运行时 `main` 通过 `h.next` 读到 `f` 的帧 arena 里已释放的节点 ⇒ UAF。
`cur` 的深度用 **max** 记成 1，而目的地要求「可能活得最久的被指物」= **min** = 0。

**【判定】** 这是一个**当前就活着**的 soundness 洞，不在 task-8 的 119 形状里，
也不在 `tools/gate-escape-known-bad.txt`（基线只有 `P0-6e_coro_ref_payload`）。
**它决定了 Q2 的修法**：把 `anRefDepth` 换成 DFA 的 max 值去当 `at`，会把这个洞放大而不是修好。
> 建议（Lead 决定）：把该探针加进 `tools/escape-corpus/`（那不在我 task-9 的写入范围里）。

---

## 5. Q4：哪些修改**必须同一轮落地**

| # | 必须成组 | 为什么 |
|---|---|---|
| C1 | **CALLRES 的"接受" 与 "调用点 arena 回传"** | 现状下 `mk` 的节点真的活在块层（§1.3 实测）。只放宽深度检查 ⇒ `mk(i, &__extc_a[2])` 的节点被存进帧层 ⇒ 16+4 例一起变 UAF。 |
| C2 | **`ARG_DEPTH` 的调用点检查与 C1 同一个数** | `checkCallRefArgs` 用的是同一个 `targetDepth`/`argDepth`（`check_expr.c:3296`）。改绑定深度记账会同时改这里的结论；`tests/reclaim-shapes/reject_deep_store_call.extc` 与 `tools/escape-corpus/control_*` 是它的对照，必须重跑。 |
| C3 | **A0_STORE 的目的地深度与 dual-pointee 洞修法** | `at` 要 **min**（可能活得最久的被指物），现有 `anRefDepth`/DFA 是 **max**。只换数不换极性 ⇒ 新洞；只改极性不动 CALLRES ⇒ 现状不变。两者都要在「pos/neg 深度」这一层同时定义清楚。 |
| C4 | **A0_RET 的"接受" 与 5 例 UAF 的"放置"** | 同一决定的两面（§3.3）。放开投影返回而不保证站点进 home，等于把 5 例 UAF 扩大到全部 `return bx.mid.next` 形状。 |
| C5 | **`needsHome` / `usesHome` / `planArenaLevel` / `arenaArg` 四个数** | 它们是同一条链：`needsHome`（`check_top.c:4099-4101` 直接判据 + `5895` REACH_HOME 闭包）→ `usesHome`（`6136`）→ 签名有没有 `__extc_home`（codegen）→ `planArenaLevel`（放置）→ `arenaArg`（调用点）。改任何一个都要四个一起自洽，否则生成物出现「声明了 `__extc_a[1]` 却没有该数组」这类编不过（代码注释 `check_top.c:6115-6124` 记载过 R2a 的先例）。 |
| C6 | **`promoteFieldsAt` 的覆盖面扩大** | 它现在会在 12 个自引用形状上撞 32 步预算（`EXTC_DBG=1` rc=134，见 §8）。扩大调用面（修 C4 的因子一/二）会让更多形状走到这条兜底。 |

---

## 6. Q5：风险面（会动到哪些既有闸门/金样例，为什么）

### 6.1 闸门与基线（棘轮，只往一个方向转）

| 闸门 | 位置 | 与本次修复的关系 |
|---|---|---|
| 闸门⑤ 逃逸健全性 | `tools/gate_escape.py` + `tools/escape-corpus/` + 基线 `tools/gate-escape-known-bad.txt` | 修好 §3 的 5 例 UAF：**新增**对应语料（必须被拒），基线里若有对应行就删。当前基线只有 `P0-6e_coro_ref_payload`。§4 的新洞也应进这里。 |
| ASan 正例 | `tools/gate_asan_corpus.py` + 基线 `tools/gate-asan-known-bad.txt`（当前空） | CALLRES 修好后会有更多程序被接受：新接受的形状必须 ASan 干净，否则这条立刻红。 |
| 反例库 | `tests/arena-soundness/run.sh` | 判据：`ACCEPT + ASan UAF` = 洞还在；`REJECT` = 毕业。§3 的 UAF 与 §4 的新洞应各有一条；修好后移出。 |
| 转正库 | `tests/arena-promoted/run.sh` | 必须被接受 + ASan 干净。CALLRES/A0_RET 修好后应往这里加正例；改记账若把这里弄红就是递归。 |
| arena 层号 | `check.sh:273-281` `python3 tools/parrun.py --mode arena-scan`（哨兵 `EXTC_DBG_ARENA`） | **任何动 `planArenaLevel`/`arenaArg` 的修改**都要过这条：它比对「检查器算的层」与「codegen 当前块」。改 C1/C4 必然触发它。 |
| 断言版全量 | `check.sh:45-58`：`EXTC_DBG=1 ./tests/run.sh` | 断言/兜底一被走到就 abort。改 `promoteFieldsAt`/放置要重跑（注意 §8 的 12 例既存 rc=134 会照旧红/被基线覆盖——先看它现在怎么处理）。 |
| 用例计数 | `check.sh:22-36` 读 `tests/run.sh` 的「失败 0」 | 任何新拒绝/新接受都会改计数；`tests/run.sh` 是唯一真源。 |
| 文档判据 | `check.sh:100-120`（`check_manual.py` / `check_doc_facts.py` / `check_doc_examples.py`） | `docs/usage/language.md` §8.1 与 `docs/RECLAMATION.md` §一/§五 的提权表述若改，示例必须能编译并标注。 |
| 结构闸门 | `tools/check_guards.py`、`tools/check_switches.py`、`tools/check_ast_freeze.py`（`check.sh:141/150/372`） | 新增 `EXTC_DBG_*` 开关或改 AST 会碰这几条。 |
| 差分闸门 | `tools/gate_difffuzz.py` + `gate-diff-known-bad.txt`（`check.sh:77`） | 改放置/arena 选择可能改变生成物语义：同一段语义 extC 与等价 C 必须同结论。 |

### 6.2 预期会红、而且**红得对**的条目

- `tests/arena-promoted/` 里与 CALLRES 同形的 `alloc_return_local.extc` / `h16_allocret.extc`
  目前是绿；若 C1 的修法让 arena 选择变化，arena-scan 哨兵可能先红 —— 那是**好在红**：
  它证明检查器与 codegen 对层号的看法漂了，必须一起改。
- `tests/reclaim-shapes/reject_deep_store.extc` / `reject_deep_store_call.extc` 的**错误文本**
  被 run.sh 钉住；改诊断措辞会红 —— 措辞可以改，但要把该文件一起更新，不能放宽判据。
- `tools/gate-escape-known-bad.txt` 里 `P0-6e` 与本任务无关，**不要**顺手改。

### 6.3 不受影响的

`E_GLOBAL`（`check_stmt.c:469-480`）与 `E_DEPTH_AN` 是正确拒绝；本计划不应动它们的判据，
只需在回归里确认它们仍被拒（`tests/errors/` 与 `min/deep_store.extc`、`min/global_ref.extc`）。

---

## 7. Q6：分步修复计划（健全性优先 + 风险递增）

> 每一步都可独立回滚；先修**漏收**再修**误拒**，因为漏收是 UB，误拒只是少收。
> 每步的闸门都是**单节**跑（不跑完整 `check.sh`）。

### 步骤 0（前置，不改行为）：把新洞与 5 例 UAF 变成可追踪语料

- 改：`tools/escape-corpus/` 新增 §4 的 `q2_dualpointee` 与 §3 的 3 个最小 UAF 形状；
  `tools/gate-escape-known-bad.txt` 相应加行（棘轮起点）。
  （⚠️ 这步超出我 task-9 的写入范围，需 Lead 另派。）
- 闸门：`python3 tools/gate_escape.py`。
- 红了说明：语料没被拒 = 洞仍在（这正是起点，基线先行）。

### 步骤 1（最高优先）：修 §3 的**漏收**（`return` 经投影 / 循环链接逃逸）

- 改：`check_escape.c` 的 `EX_IDENT` 分支（1062 的提前返回与 1080 的 `promoteFieldsAt` 调用次序）；
  `check_top.c:2200 noteFieldSrc`（允许**创建**槽 / 用完整路径而不是最后一段字段名）；
  `check_escape.c:1095-1100` 的 `EX_INDEX`（元素写的 `otherSrc` 记录，`check_top.c:1401` 附近）。
- 影响类：`E_ACCEPT_UAF` 5 例（应收敛为「接受且 ASan 干净」或「拒绝」）；`E_DEPTH_A0_RET` 6 例的放置侧。
- 闸门：`tests/arena-soundness/run.sh`（洞条应 `REJECT` 或 clean）+ `tools/gate_asan_corpus.py` +
  `bash tests/escape-fuzz/run.sh`（ASan 脏应从 5 降到 0）。
- 红了说明：要么新放的站点没进 home（ASan 脏），要么 `EX_IDENT` 的提前返回被改坏了别的形状
  （`tests/arena-promoted/` 变红）⇒ 回退，先只改 `noteFieldSrc` 的槽创建。

### 步骤 2：修 §2 的**词序敏感**（A0_STORE）—— 与 §4 的极性一起

- 改：`check_escape.c` 新增/复用一个「目的地可能被指物深度」的查询，取各赋值的 **min**
  （不要用 `check_dataflow.c` 的 max）；或把 `check_escape.c:1482` 的诊断推迟到
  `check_top.c:4206`（`dfAnalyze` + `levelPass` 之后）由 `recheckLevelRejections`
  用定下的数重发/撤销（需要扩展 `LvlRejection` 同时记目的地来源）。
- 影响类：`E_DEPTH_A0_STORE` 14 例；**并须验证 §4 的 dual-pointee 不被接受**。
- 闸门：`bash tests/escape-fuzz/run.sh`（40 真误拒应显著下降、ASan 仍 0）+
  `python3 tools/gate_escape.py`（新洞语料必须仍被拒）+ `tests/arena-promoted/run.sh`。
- 红了说明：`gate_escape` 红了 = 极性用错（把 min 写成了 max），**这正是最危险的一种红**；
  `arena-promoted` 红了 = 新判据把合法形状拒了，回退查 min 的来源。

### 步骤 3：修 §1 的 **CALLRES**（接受 + arena 回传，必须同轮）

- 改：`check_escape.c:1134-1161` 的调用分支对 `anNeedsHome/planUsesHome` 的调用记层要求；
  `check_top.c:1519 setCallArenaArg` 与末轮放置（`6071-6128` / `6150-6158`）消费这个要求；
  `check_stmt.c:415` 的 `markCallHomeIfEscaping(init, scopes.len)` 改成「不预先钉死」。
- 影响类：`E_DEPTH_CALLRES` 16 + `E_ARG_DEPTH` 4。
- 闸门：`python3 tools/parrun.py --mode arena-scan`（层号不许漂）+ `tools/gate_asan_corpus.py` +
  `tests/arena-promoted/run.sh` + `tests/reclaim-shapes/run.sh`（`reject_deep_store_call` 仍须拒）+ 本目录 `run.sh`。
- 红了说明：arena-scan 红 = 检查器与 codegen 的层号分头算（`back/codegen.c:4088` 的 `arenaDriftCheck`
  就是为此）；asan 红 = 回传通道只改了检查没改生成物；reclaim-shapes 红 = 把真悬垂的跨帧存储放过了。

### 步骤 4：修 §3 的 **A0_RET**（投影返回值）/ §4 的新洞

- 改：`check_escape.c:1062`（`!sy->origin` 也走 `promoteFieldsAt`）；`needsHome`/`usesHome`
  穿透 `EX_FIELD`/`EX_INDEX`（`check_top.c:4099` 直接判据 + `5895`/`6136` 闭包）；
  以及 §4 的 min 极性。
- 闸门：步骤 1–3 的全部 + `python3 tools/gate_escape.py` + `tests/arena-soundness/run.sh`。
- 红了说明：把「返回经投影」认成逃逸函数后若仍有站点停在块层 ⇒ 步骤 1 的放置没覆盖到该形状。

### 收尾：文档与既有语料

- `docs/usage/language.md` §8.1 的「逃逸提升」、`docs/RECLAMATION.md` §一/§五 的提权叙述
  按新语义更新（`tools/check_doc_examples.py --gate`）。
- `tests/errors/` 与 `tests/reclaim-shapes/` 的错误文本若变，随修改一起更新。

---

## 8. 未查清 / 受影响

- **未查清①**：`noteFieldSrc` 在 C1（`var bx: outer` 无初始化式 + 先写 `null` 再写 `p`）里
  `[fs]` 行出现且 `srckind=EX_IDENT`，但 **`minAt` 仍是 1**。我的解释是因子一
  （`check_escape.c:1062` 因 `origin==NULL` 提前返回、跳过 `promoteFieldsAt`），
  由 A/B/B2/C1 真值表支持，但**没有**用插桩/断点直接观测 `promoteFieldsAt` 是否被调用。
  列为**推断**；修之前建议在 `promoteFieldsAt` 入口加一次性 `EXTC_DBG_FS` 打印确认。
- **未查清②**：`m_home_cont_after` 的 `[home] build sites=4 L0 L1 L0 L0` 里，除循环的 `p`（L1）
  外还有三个 L0 站点（`varArray` 的 buffer / `grow` 的 `new` / 链头）——它们分别由哪条事实落到
  HOME 未逐条追。只确认了 UAF 的那一个（line 7, minAt=1）。
- **未查清③**：`E_ARG_DEPTH` 的 4 例里 `m_main_pfield_direct`（flow=direct）**被接受且 clean**，
  而 `m_main_pfield_ret`（换 `mk`）被拒——与 §1 同根，但没有单独验证「若步骤 3 改了 `argDepth`，
  `m_main_pfield_direct` 是否仍干净」。步骤 3 的闸门必须覆盖它。
- **`EXTC_DBG=1` 的既存兜底**：119 例里 **12 例** rc=134（`promoteFieldsAt: recursion budget
  (32) exhausted`），全部是 `ring`/`selfref`/`tree` 的 `lfield` 形状（清单见
  `q5-excc-dbg-survey.txt`）。其余 63 例 rc=0、44 例 rc=1。**这些不是本任务 7 类的结论**，
  但步骤 1（扩大 `promoteFieldsAt` 调用面）必须重新核对这张表；`min/` 的 9 个最小复现
  **都不在这 12 例里**（用 `EXTC_DBG=1` 逐个跑过，rc≤1）。
- **未验证**：`q2_dualpointee` 是否与 `tools/escape-corpus/control_*` 里已有的某条重复；
  我只比对了基线的 1 行 `P0-6e`。

---

## 附：本任务用到的 `src/` 位置索引（全部只读）

| 主题 | 位置 |
|---|---|
| `new` 站点的初始层 / refDepth | `check_expr.c:2425-2444` |
| 调用点 arena 选择 | `check_expr.c:3256-3257`、`check_top.c:1519-1542`、`check_top.c:1556+` |
| var 声明钉住绑定深度与调用 arena | `check_stmt.c:415`、`check_stmt.c:481-499` |
| store / return 的判定入口 | `check_stmt.c:673`、`684`、`689`、`740`、`760`；`check_escape.c:1448-1493` |
| `slotDepth` / `storeLayer` / `targetDepth` | `check_escape.c:121-161`、`178-192`、`368-528` |
| `promoteInto2` / `promoteFieldsAt` | `check_escape.c:969-1167`；`promoteFieldsAt` `728-770`（EX_FIELD/INDEX `1095-1100`、EX_CALL `1134-1161`、`!origin` `1062`） |
| 字段表 / 来源 | `check_top.c:2153-2314`（`noteFieldSrc` `2200`、`noteFieldDepthWrite` `2228`） |
| `markCallHomeIfEscaping` | `check_top.c:1435-1441` |
| `needsHome` / `usesHome` 闭包 | `check_top.c:4099-4101`、`5895`、`6136-6158` |
| 放置那一趟 | `check_top.c:6071-6128` |
| DFA | `check_dataflow.c:1-30`；写回 `check_top.c:4204-4271` |
| 拒绝记录 / 复核 | `check_escape.c:879-911`；`check_top.c:4217` |
| codegen 读层号 | `back/codegen.c:3175`、`3305`、`4088-4107` |

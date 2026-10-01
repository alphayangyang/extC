# V 批次：阶段契约显式化（V1 / A2）+ 命名与 0/⊤ 显式化（V2 / A1）

主人 2026-10-01：命名规范与注释规范需要完善 ⇒ **先 A2（阶段契约），再 A1（命名 + 0/⊤）**。

## V1（本轮完成）：A2 阶段契约

### 做了什么

1. **阶段是显式状态**：`CheckPhase`（`PHASE_INIT/DECLS/BODIES/POST`）+ `Checker.phase` + `phaseName()`；
   `checkModule` 在 ckB6 处置 `PHASE_BODIES`、ckB8 处置 `PHASE_POST`。
   过去"阶段"只由**代码位置**表达（`ctPhase("ckB0"…"ckB8")` 计时标记），改动放错阶段没有任何提示。
2. **唯一权威的一节**（`check_internal.h`）：一张表写清"每个阶段何时跑 / 能看到哪些表 /
   **看不到什么 ⇒ 因此必须靠什么记住**"，并挂上代码位置（阶段标记、各表的遍历点）。

### 本轮最有价值的两条**实测**结论（都是先假设、后被数据推翻）

| 假设 | 实测 | 结论 |
|---|---|---|
| "`lookup`/`placeRoot` 需要作用域 ⇒ POST 阶段非法，布断言" | 断言在套件里**响 313 次** ⇒ POST 阶段确实有大量 `lookup` | 假设**太强**。POST 阶段它们**合法但只能解析到模块级绑定**；调用名字是 `self` 这类**局部**名字 ⇒ 只能得到 `NULL` 或**同名全局** |
| "改成计数哨兵就够了" | `placeRoot` 无作用域解析成功 **0 次**（note 里没有它） | "错认同名全局"这条路**今天没被走到**，但从此被计数看守：数量一变就是要查的信号 |

**由此得到的实证意义**：这正是"为什么身份必须记在**节点/符号**上"（`identBindOf`、
`FuncDef.paramSyms`）的直接证据 —— 也是 **P0-6d 那一族的推广**。
契约里另记了一条**待查线索**：凡是在 POST 阶段依赖 `lookup` 结果做判断的地方，
都值得逐个核对是否在解析局部名字（本轮没有逐个核对）。

### "断言有牙"的证据

不需要另外造误用：上面那次**真实误用**（在 POST 阶段要求局部身份）连续 313 次让编译器
abort 并打印 `lookup('self') with no open scope (phase=post)` —— 包括消息里带阶段名与名字。
机制可用、信息够定位，这是 V1 想要的"响亮而不是静默"。

### 本轮踩的两个坑（都记下来）

1. **删注释忘了删语句**：把某个断言的注释删掉时，紧跟其后的 `EXTC_DBG_ASSERT_MSGF(...)` 还在
   ⇒ 套件仍绿（负例把 abort 当"预期报错"），但断言照样响 341 次 ⇒ **看计数、不只看套件是否绿**。
2. 又一次用了非 varargs 的 `EXTC_DBG_ASSERT_MSG` 传 3–4 个参数 ⇒ 编译期报错
   （V 批次起统一用 `EXTC_DBG_ASSERT_MSGF` / `EXTC_DBG_NOTEF`）。

### 验收（行为不变）

| 检查 | 结果 |
|---|---|
| `tests/run.sh`（发布 / `EXTC_DBG=1`） | **325 / 0** / **325 / 0** |
| 断言/兜底命中（断言版） | **0** |
| note 计数（断言版） | 84（与 V1 之前一致 ⇒ 没有新增噪声） |
| `check.sh quick` / 五道闸门 / walker / guards | 见本轮提交前的全量验收 |

## V2 第一刀（本轮）：三个"深度"改名 + 命名与注释规范成文

* **改名**（`src/`、`tests/`、`examples/`、`docs/manual`、`docs/topics`、`DESIGN-RESOLUTIONS`、
  `DECISIONS`）：`placeDepth`→`slotDepth`（55 处）、`exprRefDepth`→`targetDepth`（90 处）、
  `exprRefDepthPure`→`targetDepthPure`（28 处）、`solvedValDepth`→`solvedDepth`（16 处）；
  代码与测试里**旧名 0 处**。**历史记录不改**（审计报告与 `.audit/*-LOG.md` 是带日期的记账），
  对照表写在下面这份新文档里。
* **规范成文**：`docs/topics/NAMING.md` —— 词的纪律（一个词只指一件事：`depth` 必须带限定词、
  `level` 与 `depth` 不许混、`name` 不许当身份、`publish` 与 `invalidate` 成对…）、
  注释的五条义务（回答哪个问题 / 极性契约 / 证据 / 历史 / 阶段）、
  身份 vs 拼写的教训（含 PHASE_POST 的实测）、以及改名对照表。
* 验收：tests **325/0**（发布与 `EXTC_DBG=1` 两模式）、`check.sh quick` **55/0**、
  五道闸门全绿、walker 23 ✓、guards 18 ok。

## V2 第二刀（本轮）：0/⊤ 的第一批（死字段、魔数编码、兜底定性）

| 项 | 处理 | 证据 |
|---|---|---|
| `c->escapeesFor = (int)(size_t)f` | **删除**（字段只写不读；且把指针截断成 `int` 当"已算过"的键） | `grep -rn escapeesFor src/` 现在只剩解释性注释 |
| `effState` 的 `0/1/3` 魔数（2 空着） | 换成命名枚举 `EFF_NONE / EFF_DONE / EFF_IN_PROGRESS`（7 处 + 字段类型） | 语义写在枚举定义处 |
| `valDepthStructural` 的"认不出 ⇒ 0" | **定性为语义**：实测被 `tests/asan/promote-field-lit.extc`（正例）走到，kind = `0` = `EX_INT`（字面量，不带引用）⇒ 保留 NOTE 作**计数哨兵** | 先前的"整套零命中"读数**不准**，本轮纠正 |

**这一批的教训**：`grep "\[note\]"` 只在**测试失败时**才回显编译器输出（正例会吞掉 stderr）——
所以"零命中"的结论必须**直接对触发文件跑一次编译器**才算数，或者看 `check.sh` 里的计数。
这条已写进 V-LOG，也是下一步做 ⊤ 常量时要遵守的取证方式。

## V2 第三刀（下一轮）：⊤ 常量

* 三个"深度"改名区分：`placeDepth`（存的地方）→ `slotDepth`；`exprRefDepth`（值指向的东西）→
  `targetDepth`；`solvedValDepth`（电平稳定后重算）→ `solvedDepth`；头文件权威表同步。
* "未知"从 `0` 显式化：`0 = 活得最久` 只能有一个来源，兜底的"认不出"改成显式常量/`Top`
  并在比较处统一处理（`exprRefDepthPure`/`solvedValDepth` 的兜底、`otherDepth` 的双关、
  `minAt = -1`、`effState` 三态 int、`escapeesFor = (int)(size_t)f` 这类"指针塞进 int"）。
* 行为不变，验收同 V1（两模式 325 测试 + 五道闸门 + 18 份语料 + walker/guards）。
* 顺带沉淀一节**命名与注释规范**（哪些词只能指什么；注释必须带什么 —— 本仓库的好风格是
  "注释带实测数字与代码位置"）。

## V2 第三刀（本轮）：⊤ 常量 `DEPTH_UNKNOWN`

**做了什么**

* 在唯一权威处（`check_internal.h` 的"深度/身份：四个问题"一节）定义
  `DEPTH_UNKNOWN = 1 << 24` 与判据 `depthIsUnknown(d)`，并把语义写进极性契约：
  **"证不出它活得够久" ⇒ 在比较那一侧必须导致拒绝**；**不许**当层号传给提权
  （`promoteInto2` 的 `want` 是"要提到哪一层"，不是深度）。
* `targetDepthPure` 的两条兜底（来源链预算 40 跳、来源环）从 `return 0` 改成
  `return DEPTH_UNKNOWN`：`0` 在本仓库是"活得最久"= **放行**的方向，那是 P0-6c 的方向错误。
* NAMING.md 的 `depth` 表补一行（⊤ 是什么、不许怎么用）。

**为什么这是"准备性修复"（三条不可达证据）**

| 尝试 | 结果 |
|---|---|
| 45 跳**拷贝链**（`var aK = aK-1`） | 走不到：`noteOrigin` 把来源**压平**，链塌成一步 |
| 45 层**嵌套字段链**（`s.a.a…a.r`，`/tmp/rev/v2/deepfield.extc`） | 走不到：`EX_FIELD` 由**字段表**直接回答，不递归 |
| 整套件（`EXTC_DBG=1`） | 零命中 |

⇒ 今天改与不改对任何程序都没有差别（`deepfield` 仍 `rc=0`、套件与闸门全绿），
但**方向对了**：万一将来有形状真的走到这里，它会**拒绝**而不是放行。

**本轮踩的坑（第 N 次同类）**：补丁脚本把 `#define` 插进了**块注释中间**（那段注释在标记之后
还有一行），于是 `/* within comment` + 解析错误。教训：**在注释密集的头文件里做插入，锚点必须
带注释前缀（` * `），并且要确认插入点在 `*/` 之后** —— 用行级手术（先删后插到下一个
`#define` 之前）比"字符串替换"稳。

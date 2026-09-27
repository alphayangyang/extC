# 加固台账（HARDENING）

> **下一阶段入口（2026-09-28 收尾）** —— 接手时从这里开始 ✓

## 0.1 现在是什么状态（实测数字 ✓）

| 判据 | 结果 |
|---|---|
| `make -j` | **0 诊断** ✓ |
| `tools/golden.sh` | **413/413 逐字节 · 非法 C 0（已知 0）** ✓ |
| `python3 tools/parrun.py` | **295/0** ✓ |
| `tests/instimpl/run.sh` | **18/0** ✓ |
| `./check.sh quick` | **41/0**，退出码 0 ✓（见 0.5 的一段插曲 ✓）|
| `python3 tools/attack.py`（八组定向攻击）| **155 题 / 4 条问题** ✓ |

八组余量 ✓：generics **47 题剩 3**（B7 ✓ F1 ✓ F4 ✓）· coro 16 **0** ✓ · modules 23 **0** ✓ ·
views 16 **0** ✓ · dyn 16 **0** ✓ · arena 12 **0** ✓ · extern 13 **0** ✓ · **fs 12 剩 1**（S11 ✓）。

## 0.2 已经修掉的（16 条 + 1 条语言规则 ✓）

| # | 内容 | 提交 |
|---|---|---|
| H1 H2 | 主函数里 `5?` / `return {…} impl` 的空指针 | `724ca7d` |
| H3 | `1e400` ⇒ 生成 `inf` 未声明 | `c10eb27` |
| H4 | 字面量作赋值左值 | `a5f6cdd` |
| ② | 文本 pass 的偏移失效（prelude 非法 C）| `64e3c3b` |
| H5 | 空视图存储的未定义行为 ⇒ 带位置 trap | `21e0ec5` |
| H6 | 字符串里的非法转义直通 | `eed5ccb` |
| H7 | 数组与标量比较被放行 ⇒ `extc_eq` 越界读 | `fbc3685` |
| H8 | 自递归实例化让编译器挂死/OOM | `420df51` |
| H9 | 泛型函数返回 `T` 构造的实例 ⇒ 零值名错 | `f63cb5e` |
| H10 | 用户类型名撞实例 C 名 | `58fc779` |
| `__` 规则 | 用户标识符不许以 `__` 开头（生成名保留前缀）| `084303c` |
| H11 | 显式实参诊断推荐了不工作的写法 | `1e84999` |
| H12 | `main` 掉出末尾发 trap（应为隐式 `return 0`）| `43a7b8b` |
| H13 | 无接收者方法经 `dyn` 调用 ⇒ **编译器段错误** | `1071d84` |
| H14 | 显式类型实参**真的喂推断** | `02879cc` |
| H15 | `f()!` 作语句生成裸值（`-Werror=unused-value`）| `9ca5973` |

工具 ✓：`tools/golden.sh`（413 份基准 + 非法 C 检查）· `tools/fuzz.py`（变异/生成式/模块级 + 分诊 +
战役脚本 + 清理）· **`tools/attack.py`（八组定向攻击，155 题）**。

## 0.3 还开着什么（按优先级 ✓，每条都有入口 ✓）

1. ~~**B7 + F4**~~ ✅ **已修（2026-09-28，本轮）** —— 真根是**时机**：`gen(v)` 写在泛型函数体里 ⇒
   调用被延迟 ⇒ 实例在**延迟实例化那一趟**（body 检查之后）才建出来 ⇒ 把协程设置从 `checkFunc`
   提取成 `coroSetup`、在延迟回放之后对实例补跑（+ `coroFrameLay` 填帧字段、帧名用 `instName`、
   统一判据 `coroEmitted` 决定"谁被发射"、handle typedef/池/任务表按需拼接）。**过程中还发现并修掉
   自己引入的一个编译器段错误** ✓：拼接用 `memcpy(out->data, …, nb.len)` 直接写 ✗ 而单元变长了
   ⇒ 写越界 ⇒ `dropUnreferenced` 里的野指针 SIGSEGV ✓ ⇒ 改成**经 `Buf` 重建** ✓。
   **B8 也已修** ✓（第 3 轮）：把 `DeferredUse` 推广成"**记表达式**" —— 复查时
   ① 非"延迟方法"的节点按**自身类型**复查（`ttSubstitute(du->call->type, …)` ✓，不再读
   `u.method.recv` ✗ —— 那正是第 2 轮崩溃的原因 ✓）；② **两边都替换**（`want` 也 `ttSubstitute` ✓ ——
   只替换一边会给语料里每个泛型调用报假错 `` `gen_i64` expects `T`, found `i64` `` ✗，闸门当场 144 份
   差异 ✓）；③ `checkAssignable` 在"值是参数"时也记录并接受 ✓。三形状（1/2/3 次 yield）rc=4/8/12 全对 ✓；
   闸门 413/413 · 非法 C 0（零变化 ✓）、parrun 295/0、instimpl 19/0 ✓。

   **第 1 轮（新 goal）的排查** ✓：第一处嫌疑 `runDeferredUse`（`check_top.c:4519-4520` ✓，只在
   `rt->kind == TY_GENERIC && f->owner == rt->sdef` 时替换 ✓，而协程协议方法的接收者是**帧类型**
   ⇒ 不替换 ✗）——改成"从帧的 `coroOf` 取 `yieldType`"之后**报错照旧** ✗ ⇒ 说明症结不在那里 ✓。
   **第 2 轮（打点 + 两处修改，全部回退 ✗）** ✓：

   - 打点 `runOpCheck`（`EXTC_OP=1` ✓）⇒ **零输出** ✗ ⇒ 报错**不是**延迟复查发的 ✓，而是**模板期**
     的具体算符检查 ✓：`check_expr.c:149` 只判"**左边**是不是参数"（`lt->kind == TY_PARAM` ✓）
     ⇒ `i64 + T`（参数在**右边** ✗）掉进 `:155` 的数值检查 ⇒ 报错 ✓；
   - 于是改成"**任一侧**是参数就 `deferOp`" ✓ ⇒ `+` 的错**消失** ✓，错误前移到**赋值**：
     `assignment expects 'i64', found 'T'` ✗ ⇒ 继续查 `checkAssignable`（`check.c:414` ✓）：
     它只在"**延迟方法调用**"（`EX_METHOD && !func && mentionsParam(recv)` ✓）时记录复查 ✗，
     而这里的值来自 **`EX_BIN`（`+`）** ⇒ 不记录 ⇒ 报错 ✓；
   - 于是我改成"值是参数也记录并接受" ✗ ⇒ **灾难** ✗：`runDeferredUse`（`check_top.c:4511` ✓）
     假设记录下来的节点是 **`EX_METHOD`**（它读 `du->call->u.method.recv` ✓）⇒ 我塞进去的
     `EX_BIN` 让那里读**垃圾指针** ⇒ **几十个文件编译器 SIGSEGV** ✗（闸门 126 份差异 ✓、
     parrun **278/17** ✗）⇒ **两处修改全部回退** ✓（闸门 413/413 ✓、parrun 295/0 ✓、
     instimpl 19/0 ✓）；
   - **第 3 轮：按该路径做完 ⇒ B8 收口** ✓（见下）✓。

   （原记录）**正确的下一步** ✓：把 `DeferredUse` 从"记方法调用"**推广成"记表达式"** ✓ ——
     存 `Expr *node` + `Type *want` ✓，在实例期用 `ttSubstitute` 重算该节点的类型 ✓
     （而不是复用 `u.method.recv` ✗）；这样"值是参数"就能安全地延迟 ✓，`+` 与赋值两处
     都走同一条路 ✓。

   真正的报错来自 **`OpCheck`**（`cannot apply '+' to i64 and T` ✓，`check_top.c:4426-4427` ✓ 用
   `ttSubstitute` 替换两个操作数 ✓）：`+` 的**右操作数类型**在模板期就记成了 `T` ✓ ⇒
   下一轮的打点位置就是 `:4426`（打印 `typeStr(lt)` / `typeStr(rt2)` ✓）与记录处（操作数类型
   从哪来 ✓：`e->u.bin.right->type` ✓＝方法调用表达式的类型 ✓）。**已回退** ✓（闸门 413/413 ✓）。
2. ~~**F1**~~ ✅ **已修（第 9 轮）** ✓ —— 四步同轮落地 ✓（第 25→36 轮地图里的最后一块 ✓）：

   ① **检查器的实现查询**（`check_expr.c:458` ✓）用 `ttEquals(im->target, pt)` ✗ ⇒ `pair<i64>` 与
      `pair<T>` 永不相等 ⇒ 改成"**同一泛型声明即算覆盖**" ✓（`it->sdef == pt->sdef` ✓，与 H10 同手法 ✓）；
   ② **把 impl 的方法标 `used` + 新增 `dynTable`** ✓ —— `codegen.c:7450` 那段 "method definitions
      of the instances" ✓ 只发 `md->used` 的方法 ✓，而**实例方法体没被点名就会被"definitions
      nothing names"删掉** ✓（打点实测：`inst=pair_i64 … md=tag used=1` ✓，生成物里却出现 **0** 次 ✓）；
   ③ **vt 表按实例发表** ✓（`codegen.c:7502` ✓）：泛型 target 时遍历 `g.insts` ✓，在
      `substEnter(&g, inst)` 里发表 ✓ ⇒ 表名 `extc_vt$Tag$pair_i64` ✓、thunk
      `extc_th$Tag$pair_i64$tag` ✓、调用 `pair_i64_tag` ✓ —— 一套名字全对齐 ✓；
   ④ **豁免只给打了 `dynTable` 的方法** ✓ —— 一开始把**所有**实例方法都豁免 ✗ ⇒ **73 份**基准变化 ✗
      （那些是本来该被删的未用实例方法 ✓）⇒ 改成按需豁免后**零变化** ✓（`c497870` ✓）。

   **同族缺口 F1b + D9 也已修** ✓（第 12 轮 ✓，**两件事必须一起做** ✓）：

   打点实测把方向彻底纠正了 ✓：F1b 的生成物里 `p2_i64_tag` 的**原型与定义都在** ✓、thunk 也在 ✓，
   只是**两处都没有 `__attribute__((unused))`** ✗ ⇒ 这一族**不是"尸体被删"** ✓，而是
   **`markUnusedParams` 没处理实例方法** ✓。再打一层点（在那趟里打印条目）⇒
   `p2_i64_tag body=NULL off=0 len=0 proto=yes` ✗ ⇒ 条目**存在**（原型登记过 ✓）但 `off/len` 是 0 ✓
   ⇒ 因为**第一版的 `dyn` 豁免把 `deadFuncBody` 一起跳过了** ✗ ⇒ 条目没有 body ⇒ 那趟跳过它 ✓。
   **修法（两处合用 ✓）**：① 豁免分支里**照常调用 `deadFuncBody`** ✓（条目已有 `proto` ✓ ⇒ 不会崩 ✓，
   它只填 `off/len` ✓）；② `DeadFunc` 加 `dynTable` 标记 ✓，删除那趟**跳过**它 ✓
   （表在删除那趟之后才拼 ✓，那趟看不见 thunk 的提及 ✓）。
   **结果** ✓：generics **49/49** ✓、dyn **16/16** ✓、闸门 **413/413 · 非法 C 0（零变化 ✓）**、
   parrun 295/0 ✓、instimpl 19/0 ✓、quick 41/0 ✓、**`tools/attack.py` 十一组 207 题全 0** ✓✓。

   （历史）**同轮还暴露一个同族缺口** ✓，单列追踪为 **F1b** ✓（`tools/attack.py generics` ✓）：**泛型实例的
   方法体在忽略 `self` 时**没走 `markUnusedParams` 那趟 ✓（它只按 `g.funcs` 找候选 ✓，实例方法不在
   里面 ✓）⇒ 生成物在 `-Werror=unused-parameter` 下编译失败 ✓。**它同时让 dyn 组的 D9 变红** ✓
   （D9 原先在检查器就被拒 ✓ 算 `ok_or_reject` 通过 ✓；现在一路编到 gcc ✓ 栽在同一处 ✓）⇒
   **第 10 轮的第一次尝试：失败并崩掉** ✗（已回退 ✓）—— 我在按实例发方法体的循环里**手工**造了一条
   `DeadFunc`（`name`/`off`/`len` ✓）并塞进 `g.deadFuncs` ✓，想让 `markUnusedParams` 能看见它 ✓
   （那趟只走 `g.deadFuncs` ✓，`codegen.c:6144` ✓）。结果：**F1、F1b、D9 三条全部让编译器崩溃** ✗
   ⇒ 说明那条清单有**不变量**（条目多半还要 `scopeA/scopeB`/`kind` 之类的字段 ✓，或者必须由它的
   **创建函数**登记 ✓，手搓的条目在后面的遍历里被解引用 ⇒ 段错误 ✓）。
   **第 10 轮查清的完整机制** ✓（`DeadFunc` 结构体在 `codegen.c:84-89` ✓）：

   - 条目的不变量是 **`proto` 必须非空** ✓ —— 后面几趟（`:5528`/`:5897`/`:6075` 一带 ✓）都会解引用它 ✓，
     我手搓的条目 `proto = NULL` ✗ ⇒ **段错误** ✓（这就是三条全崩的原因 ✓）；
   - **创建处有两处** ✓：`:7391`（**实例方法的原型** ✓，在 `substEnter` 里登记 ✓ ⇒ `pair_i64_tag` 的条目
     其实**早就存在** ✓）与 `:7423`（普通函数/方法 ✓）；
   - ⇒ 于是我改成"**照常调用 `deadFuncBody`**" ✓（它会按名字认领已有条目 ✓ 并补上 `off/len` ✓）⇒
     **闸门零变化** ✓ 但 **F1 重新变红** ✗：说明"**点名了就不删**"这个预期**不成立** ✓ ——
     即使 thunk（`extc_th$Tag$pair_i64$tag` ✓ → `pair_i64_tag` ✓）已经发出 ✓，
     "definitions nothing names" 那趟**仍然删掉了它** ✓ ⇒ 该趟**看不到 vt 表区域的提及** ✓
     （要么扫描范围不含它 ✓，要么它在扫描之后才拼进去 ✓）⇒ **下一轮**：查那趟的扫描范围
     （`dropUnreferenced` ✓ 用 `countMentionsIn(g->body…)`/`countCodeMentions` ✓）并把 vt 区
     **提前拼入**或**纳入范围** ✓；这同时解决 `markUnusedParams` 看不到实例方法的问题 ✓。

   **第 11 轮：把"谁在什么时候拼"彻底钉死** ✓（含两次回退 ✗）：

   - 删除那趟的判定是 `countGet(name) != 2` ⇒ **只有原型+定义两次 ⇒ 删** ✓（`codegen.c:5906` ✓，
     计数范围是**整个单元** ✓ `:5901` ✓）⇒ 打点实测 `pair_i64_tag`：`body=yes count=2 hit_proto=yes` ✗
     ⇒ **thunk 的调用确实不在当时的文本里** ✓；
   - **第一次尝试（回退 ✗）**：把 vt 区**提前拼进 `out`** ✓ ⇒ **16 份非法 C** ✗、dyn 组 **13/16** ✗
     ⇒ 那两趟的前提被破坏 ✓（不能搬拼接位置 ✓）；
   - **第二次尝试（回退 ✗）**：改成"**只把 vt 区的文本额外计入计数**" ✓（新 `CG.vtText` ✓ + 在函数
     那趟多一次 `countSpan` ✓）⇒ 闸门零变化 ✓ 但 F1 仍红 ✗ ⇒ 因为 **`g.vtText` 是在删除那趟
     **之后**才被填的** ✓（我把它挂在两个拼接点上 ✓ ⇒ 那里计数时还是空的 ✓）；
   - ⇒ **下一轮的确切改法** ✓：把 `vtText` 的填充**搬进 vt 构建函数内部**（两个 Buf 在那儿都已完整 ✓），
     并让**构建（不是拼接 ✓）**在计数那趟**之前**先跑一次 ✓ —— 构建只写 `g.vtDecls`/`vtDefs`/`g.vtText`
     三个 CG 缓冲 ✓ **不碰 `out`** ✓（这正是第一次失败与这次的区别 ✓）。

   **第 11 轮收尾的第三次尝试（同样无效果，已回退 ✗）**：给 `DeadFunc` 加 `dynTable` 标记 ✓、
   `deadFuncBody` 登记时从 `FuncDef` 带过来 ✓、删除那趟**跳过**它 ✓ ⇒ `pair_i64_tag` 保住了 ✓
   （F1 保持绿 ✓、闸门**零变化** ✓），但 **F1b/D9 的失败模式没变** ✗ ⇒ 说明它们卡的不是"尸体被删" ✓
   而是**别处** ✓（都报"生成的 C 不合法" ✓ = gcc 拒收 ✓）⇒ **下一轮先看 F1b 的生成物** ✓：
   确认 `p2_i64_tag` 的定义在不在 ✓、`markUnusedParams` 有没有给它插 `__attribute__((unused))` ✓
   （那趟只走 `g.deadFuncs` ✓，而实例方法的条目来自 `:7391` 的原型登记 ✓ ⇒ **很可能它没有 `body`** ✗
   —— `:7732` 只给 `df->len` 非零的条目填 `body` ✓，而 `deadFuncBody` 是否**匹配**到那条要看
   `cFuncName` 在两种上下文里是否同名 ✓）⇒ 若确认是这条 ✓，修法是让 `markUnusedParams` 也走
   **实例方法**（例如在按实例发方法体的循环里把 `md` 加进一份单独清单 ✓）。

   **第 11 轮末的实测（关键）** ✓：F1b 的生成物里 **`p2_i64_tag` 的原型与定义都在** ✓
   （`static int64_t p2_i64_tag(p2_i64 * self);` ✓ + 定义 ✓ + thunk ✓）—— 只是**两处都没有
   `__attribute__((unused))`** ✗ ⇒ 所以这一族**不是**"尸体被删" ✓，而是 **`markUnusedParams` 没处理
   它** ✓。那趟只走 `g.deadFuncs` ✓，而实例方法的条目由 `:7391` 登记（在 `substEnter` 里 ✓ 名字应为
   `p2_i64_tag` ✓）✓ ⇒ **下一轮的打点**：在那趟里对名字含 `_tag` 的条目打印
   `body/off/len/proto` ✓，确认是"条目没被 `deadFuncBody` 认领（`body` 为空 ✓）"还是
   "认领了但定位失败（`funcDefStart` 找不到 ✓）" ✓ —— 两者都很容易补 ✓。

   （历史）**下一轮的正确做法** ✓：先找 `g.deadFuncs` 的**创建处**（`grep -n "DeadFunc" src/codegen.c` ✓ ——
   我只找到读处与 `deadFuncBody` 的**匹配**处 `:5205` ✓，所以创建必在别处 ✓），用它登记实例方法 ✓，
   而不要手搓条目 ✓。

3. ~~**H15 同族（S11）**~~ ✅ **已修（第 5 轮）** ✓ —— 实测**它与 A4 残留是同一个 bug** ✓：
   `var f = fs::openWrite(p)!` 里 `f` 没人用 ⇒ 死局部变量消除**删掉声明前缀** ✗、按设计**保留**
   了带调用的右值 ✓，而留下的那段是**解包后的载荷读取**（`(fs$openWrite(…)).u.success._0;` ✗）
   ⇒ gcc `unused-value`、`-Werror` 下编译失败 ✓。
   **修法**（`dropUnusedLocals` 里构造"保留文本"的那一处 ✓）：把保留的右值包成 `(void)(…)` ✓
   （去掉尾部 `;`/换行再补 ✓）⇒ 一次修掉 **S11**（fs 组转绿 ✓）与 **A4 残留**（那个形状的生成物
   也合法了 ✓）。**代价**：**37 份**基准变化 ✓（全是"死局部变量 + 右值里有调用"的程序 ✓ = 本 bug
   的受害者 ✓，0 份非法 C ✓）⇒ 已按"由修 bug 引起"重设 ✓。验证：闸门 413/413 · 非法 C 0 ✓、
   fs 组 **12/12** ✓、parrun 295/0 ✓、instimpl 19/0 ✓、quick 41/0 ✓；
4. ~~**A4 残留**~~ ✅ **已修（第 5 轮，与 S11 同一处）** ✓ —— 见上 ✓；
5. **阶段 B**（所有生成名加 `__` 前缀 + 全量重设基准 ✓）—— 主人明确暂缓 ✓。

## 0.5 一段插曲：H14 让一条老反例**过期** ✓（收尾时才逮到）

写这一页时我把 `check.sh quick` 的数字从第 26 轮照抄成 **41/0** ✗（那一轮确实如此 ✓），
而收尾时的实测是 **40/1** ✗ —— 红的正是 `tests/generics/errors/cannot_infer.extc`：

```
fn makeDefault<T>() -> i32 { return 7 }
fn main() { println(makeDefault<i32>()) }      /* 期望报错 ✗ … 现在编过了 ✓ */
```

它的注释写着"要写显式实参 ✗" ✓ —— 而 **H14 做的正是让显式实参真的能喂推断** ✓ ⇒ 这条反例的
**前提过期** ✓（老前提是"显式实参没用" ✗）。改成不带显式实参的形状（`makeDefault()` ✓）后它仍然
推不出来 ✓ ⇒ `check.sh quick` 回到 **41/0** ✓。**两条教训** ✓：① 改了语言行为之后要**回头搜老反例**
（这次是收尾才发现 ✓）；② 页面/提交里的数字**必须当场实测** ✓ —— 我这次恰好自己破了自己的规矩 ✓，
好在被 quick 当场抓住 ✓。

## 0.4 不能破的约定 ✓

- 每一步先跑 `tools/golden.sh`（413/413 逐字节 ✓）；基准变化只有两种允许来源：**由修 bug 引起**
  （提交信息写明范围 ✓）或**阶段 B 那种全局改名**（同样写明 ✓）；
- `make` 零诊断 ✓；不放松 `check_escape.c` / `check_top.c` 的逃逸与级别分析（守卫不得掩盖真逃逸 ✓）；
- 健全性修复（UB / 崩溃 / 生成物非法 / 编译器挂死）**自主落地** ✓；只有大范围语义变化、要改语言规则、
  影响面无法估算的才报备 ✓；
- **改了没效果一律回退** ✓ 并在本文件记档（含试错失败的原因 ✓）—— 本轮里 A4 / F1 / B7+F4 / H15
  都是这样走过来的 ✓；
- 提交信息里的数字**只写实测跑出来的** ✓（这条是被自己两次"提前宣布全绿"换来的 ✓）。

# 加固记录（自主迭代）

来源：主人交代"上强度、构造 hack 数据、跑 fuzzer，遇到问题就修，修不了的记下来"。
工具：[tools/fuzz.py](../tools/fuzz.py)（变异 fuzzer）· [tools/golden.sh](../tools/golden.sh)（413 份生成物逐字节 + 必须合法 C）。

## 一、已修（都有回归判据 + 提交）

| 编号 | 问题 | 触发（最小） | 修法 | 提交 |
|---|---|---|---|---|
| H1 | **SIGSEGV**：`main` 里对**非 option** 用 `?` | `fn main() -> i32 { let x = 5?` | `checkTryInner` 的"main 里失败即 trap"分支加"操作数真的带载荷"守卫；非 option 落到原分支报 `` `?` needs an `option<...>` `` | `724ca7d` |
| H2 | **SIGSEGV**：畸形结构体字面量 + 泛型返回 | `fn make<T>(x: T) -> pair<T> { return { a: make, b: x } impl }` | `levelOfValue2` 里 `identBindOf` 结果为 NULL 时提前返回（与"无发布可跟"同答案；名字没解析出来本身就会报错，不可能被接受） | `724ca7d` |

两条回归进 `tests/errors/`（`// expect:` 注释由 `tools/parrun.py` 自动收集，parrun 285/0）。

**Fuzzer 本身**：变异保留源码行结构（只换 token），否则几乎全成语法错、走不到 codegen；实测 yield 100%
（3 seed × 400 次里 1199 次编译成功）。失败用例带 seed/迭代号，可复现。

### H3（已修）非有限浮点字面量生成 `inf`，C 里没有这个标识符

`tools/fuzz.py` 的变异词表里有 `1e400`，两个程序因此"extC 说成功、gcc 编不过"：
`-1e400` 被 `%g` 打成 `-inf` ⇒ `error: 'inf' undeclared`。

修法：`EX_FLOAT` 落地时先判断 NaN / ±∞（`v != v`、`v * 0.5 == v`），走
`__builtin_nan("")` / `±__builtin_inf()`；**不引入 `<math.h>`**，否则每一份生成物都会变。
实测：`1e400` / `-1e400` / `f32(-1e400)` 都产出合法 C，且 413 份生成物**逐字节不变**
（正常浮点走原路径）。回归：`tests/asan/nonfinite-float.extc`（该套件按目录 glob 收集）。

### H4（已修）给字面量赋值被接受，生成的 C 不是左值

`fail-00075`：变异把标识符换成了字面量 ⇒ 源码 `18446744073709551615 = total + p.val`，
extC 说成功，生成的 C 是 `9223372036854775807 = (total + p->val);`，gcc 报
`lvalue required as left operand of assignment`。**赋值左边从来没被要求是可写位置**。

修法：`case ST_ASSIGN` 开头加校验，判据与 C 一致 —— 只看**最外层形态**
（`EX_IDENT` / `EX_FIELD` / `EX_INDEX` / `EX_SLICE` / `EX_DEREF` / `EX_SIGN`），其余（字面量、
算术/比较表达式、调用）没有存储。两处踩到又修掉的误拒，都靠语料抓出来：

1. 先写成 `isPlace(target)`（那是为**切片**写的判据，只认绑定/字段/下标/切片）⇒ 25 份语料编不过；
2. 补上 `EX_DEREF` 后仍是 5 份 ⇒ 真正原因是"最外层"这个原则：`self.big![self.n] = b`（stdlib）
   与 `(*buf)[k] = (*src)[i]`（bench/oi）的外层是下标/签名，C 允许，而按根递归就误判了。

实测：6 种非法形状全拒（字面量 / 大整数 / 调用结果 / 算术 / 比较 / 复合 `+=` 字面量），
6 种合法形状全通（变量 / 字段 / 下标 / 解引用 / 括号解引用下标 / 取字段再下标）；
**413 份生成物逐字节不变**；parrun 285 → 286/0（回归 `tests/errors/assign_to_literal.extc`）。

## 二、已修：文本后处理 pass 的**越界删除**（2026-09-28 修完，见 §3.2 的收尾）

**症状**：`examples/prelude.extc` 的生成物在某种路径写法下是**非法 C**：

```c
/* 期望 */ (printf("%s", (slice_u8_isEmpty(&(b))) ? "true" : "false"), printf("\n"));
/* 实得 */ (printf("%s", (slice_u8_e" : "false"), printf("\n"));
```

**可复现的判别对**（同一棵树、同一份源码；两次编译各自 10/10 确定性一致）：

| | 从仓库根编译 `examples/prelude.extc` | 拷贝到 `/tmp/*.extc` 后编译 |
|---|---|---|
| 生成物哈希 | `ac9c8d53…` ✗ 非法 C | `f3ac7cfd…` ✓ 合法 |

**更正（2026-09-28，之前写成"路径写法"是不准确的）**：我又量了一轮 —— 在 `/tmp` 下用**相对**
与**绝对**两种写法编译同一个拷贝，输出**完全一致**；把文件从 `examples/` 挪到 `/tmp` 才改变结果。
所以真正的变量是**编译上下文**（同一份源码在仓库根与在别处解析出的编译单元不同），
而不是路径字符串本身。曾经想把它做成 fuzz oracle 的一条"同一输入必须产出同一 C"判据，
因为上面这个反例（合法的上下文差异也会改变输出）会**误报**，已撤掉。

**因果链（已用探针钉住）**：

1. pass 流水线**迭代多轮**：`dropUnreferenced` 被调用三轮（文本 11571 → 4950 → 3557）。
2. 第 1 轮 `dropRuntimeDefs` 之后调用仍在（`len=11548 调用=11400`）；第 2 轮入口就没了
   （`len=4950 调用=-1`）⇒ 是第 1 轮 `dropRuntimeDefs` 的某刀吃掉了它。
3. 逐刀加"是否覆盖这次调用"的探针，命中的是：

   ```
   [rt] extc_arena_alloc  at=3080 span=2030 调用在5105 覆盖=★★是★★
   ```

   `extc_arena_alloc` 的定义被按 **2030 字节**的范围删除 ⇒ 越界切进用户代码，吞掉 `main` 里的调用。
4. 机制：多行定义的 `span` 是"从定义首行扫到**第一个列 0 的 `}`**"。当该定义已被**前面的轮次**
   部分删掉时，这个扫描会越过定义、落到**用户函数**的收尾花括号上。

### 2.1 修法与证据（round 6 收敛）

两处改动，都在 `dropRuntimeDefs`：

1. **定义结束改成按括号深度配对**（原来"扫到第一个列 0 的 `}`"）：定义完整时停在它自己的收尾；
   被部分删过时深度回不到 0 ⇒ `span` 保持 0 ⇒ **不删**；
2. **扫描与删除都不许越过文本末尾**：`for (; p < rp + rl && p < text + len; p++)`，并在删除前加
   `if (ln + span > text + len) span = 0;`。

定位过程（每一步都是实测，不是推理）：

| 观察 | 结论 |
|---|---|
| `EXTC_SKIP_*` 逐个关 pass | 损坏需要 `dropRuntimeDefs` **和** `dropUnreferenced` 都跑 |
| 逐刀"是否覆盖这次调用" | `[rt] extc_arena_alloc at=3080 span=2030 覆盖=★★是★★` |
| 打上改动 1 之后 | 所有 `span` 变成定义大小（161/242/…）✓，**2030 消失** ✓ —— 但输出**逐字节没变** ✗ ⇒ 那一刀是**红鲱鱼** |
| 在每个 pass 后查损坏串 | 它在 `cg-runtime` 里出现 |
| 在 pass 内部逐刀查 | 每刀之后都干净 ✗（我当时只看前 14 行，那是第 1 轮）|
| 进入/离开该 pass 对照 | **第 2 轮**：进入干净（len=10715）→ 离开已坏（len=4950）✗ |

⇒ 综合起来：没有任何一刀"覆盖"那次调用，调用却消失了 —— 只可能是**越界**：`rp + rl` 用的是
**原始**运行期文本长度，第 2 轮里它已经指向缓冲区之外，`span` 在界外量出来之后
`memmove` 的长度 `len - (ln - text) - span + 1` 会下溢成天文数字，一次拷贝毁掉后面整个文件。
改动 2 正是堵住这条（改动 1 单独不够：它只让 span 更合理，越界仍然发生）。
**注**：下溢这一步是从"界外 span + 无覆盖却损坏"推出来的机制，未逐字节验证；已验证的是
两处界加上去之后损坏消失、413 份全部合法。

**验证**：`examples/prelude.extc` 在**仓库根**编译 ⇒ 合法 C ✓；全量 413 份**只有这一份**变化，
且是"非法 C → 合法 C" ✓；`tools/golden-known-bad.txt` 已清空（任何一份产物不合法都会让闸门变红）；
`tools/golden.sh` ⇒ 413/413 逐字节 ✓ 且 **非法 C 0** ✓；parrun 286/0 ✓；asan 9 项 ✓；instimpl 15/15 ✓。

**试过但无效（都已回退，量过）**：① 把扫描窗口 `rl` 提到外层循环外；② 定义长度改成"在当前文本里
再找一次列 0 的 `}`"；③ 用 `g->primText` 里同一行的原始长度给 `span` 加上界（`strstr` 整行查找，
仍没拦住那一刀 —— 说明同一次删除里还有别的路径在改 `span`，或越界发生在另一处 `memmove`）。

## 三、待拍板（需要主人决定，先不阻塞）

### 3.1 文本 pass 的偏移记账要不要重做（对应 §二）

**补记（round 3）：这件事按目标 ② 本来就在"我做"的范围内，不需要拍板 —— 只是要挑一个
不与我自己的构建互相干扰的时间窗做。** 下面是据已有证据写好的落地计划。

已知的最后一环（探针原话）：

```
[rt] extc_arena_alloc  at=3080 span=2030 调用在5105 覆盖=★★是★★
```

`dropRuntimeDefs` 里多行定义的 `span` = "从定义首行扫到第一个列 0 的 `}`"，窗口上界是
`rp + rl`，而 `rl` 每轮外层循环重置为 `rl0 = strlen(g->primText)`（**原始**长度）。流水线跑多轮 ⇒
第二轮起当前文本里已没有整段运行期块，窗口却仍按原始长度延伸 ⇒ 扫描越过块尾、落到**用户函数**的
`}` 上 ⇒ 一刀 2030 字节。三次尝试都**没拦住**，原因分别是：

| 尝试 | 为什么无效 |
|---|---|
| 把 `rl` 提到外层循环外 | 越界发生在**单次**扫描里，跨迭代收缩救不了它 |
| 定义长度改成"在当前文本里重新找列 0 的 `}`" | 找的仍是那个**错的**位置（扫描起点已经越界） |
| 用 `g->primText` 里同一行的原始长度给 `span` 上界 | 行查找一开始写成 `strstr(g->primText, ln)`（`ln` 不是 C 字符串 ⇒ 永远找不到）；改成整行拷贝后仍没拦住 ⇒ 说明那一刀之外还有另一处 `memmove`/另一条路径在改 `span`，**没定位完** |

**round 5 读代码的更正（不必再打点了）**：`dropRuntimeDefs` 里**只有一处** `memmove`
（`codegen.c:5323`），`span` 的唯一来源是 `:5308-5309`「从定义首行扫到**第一个列 0 的 `}`**」。
所以上一轮写的"还有第二处 `memmove` 在改 `span`"是**猜错**（那是没打点的猜测）。真正的机制是：

* `rl0 = strlen(g->primText)` 是**原始**运行期文本长度，而 `size_t rl = rl0` **每次调用**都重置；
* 流水线跑到第二轮时，当前文本里的运行期块已经短了一截 ⇒ 窗口 `rp + rl` 从一开头就偏长
  ⇒ 扫描越过块尾、落到**用户函数**的 `}` 上 ⇒ `span=2030`；
* 因此任何"调用内"的记账（第 ① 次改动把 `rl` 提到外层循环 ✗）都救不了它 —— 偏长发生在
  进入这次调用之前。

**下一次的修法（按括号深度配对，一次只动这一处）**：

把 `:5308` 的扫描从"第一个列 0 的 `}`"改成**深度配对**：从 `ln` 起数 `{`/`}`，定义在
**深度回到 0** 的那个 `}` 结束（`ln` 所在行以 `{` 收尾 ⇒ 初始深度 1；单行定义也自然覆盖）。

* 定义**完整**时：正好停在它自己的收尾 ✓（比列 0 启发式更准）；
* 定义**被部分删过**时：开头那个 `{` 永远配不上 ⇒ 深度回不到 0 ⇒ 窗口内找不到结束
  ⇒ `span = 0` ⇒ **不删**（宁可不删，绝不越删）✓。

成功判据：`examples/prelude.extc` **在仓库根**编译也产出合法 C ⇒ 从
`tools/golden-known-bad.txt` 删掉那一行 ⇒ 提交信息写明"基准只此一份变化、且是非法 C → 合法 C"。

**下面这段是上一轮写的打点计划，保留备查（其中"第二处 memmove"的前提已被上面更正）：**

1. 在第 1 轮 `dropRuntimeDefs` 的**每一次** `memmove` 前后各打一条 `len` + 调用位置（已有探针骨架 ✓），
   找出除 `[rt]` 之外还有谁在缩短文本；
2. 落地"**甲**"：删除前要求"被删区间在当前文本里**首行匹配且以列 0 的 `}` 收尾、且区间内不再出现
   另一个列 0 的 `}`"——不满足就**跳过**（宁可不删，绝不越删）；
3. 若"甲"留下残留（生成的 C 里出现半截定义 ⇒ gcc 会报），再上"**乙**"：删之前把所有区间**收集**完、
   从后往前一次性应用（`markUncalledFunctions` 那边有同类写法可参照，我还没定位到它的具体结构）；
4. 成功判据：`examples/prelude.extc` 在**仓库根**编译也产出合法 C ⇒ 从 `tools/golden-known-bad.txt`
   删掉那一行，并在提交信息里写明"基准只此一份变化、且是非法 C → 合法 C"。

### 3.1′ 原问题（保留备查）：偏移记账要不要重做

越界删除的根子是：**每个 pass 都拿着"生成时捕获"的偏移/文本，而流水线会跑多轮、文本每轮都在变**。
可证安全的做法有两条方向，选哪条是设计决定：

- **甲**：删除一律"先在当前文本里重新定位、并且只删**完整匹配**的定义"（发现不完整就跳过）——
  改动小，但可能出现"该删没删"的残留；
- **乙**：给每个 pass 维护"当前文本里各定义的真实区间"，每刀之后同步更新（或干脆先收集所有区间、
  从后往前一次性应用）—— 更彻底，但要动 pass 的数据结构。

### 3.2 基准重设的范围

`tools/golden-known-bad.txt` 里目前挂着 `examples/prelude.extc`（非法 C）。§二修好之后它应当变成
合法 C，那一天需要把这一份的哈希一起更新（**只此一份**，且是"非法 → 合法"）。

### 3.3 `impl` 方法不读 `self` 触发 `-Werror`（L 债）—— 功能可修，但**按目标判据不落地**（round 7 量过）

**修法与效果**（`impl Tag for box { fn tag(self: ref box) -> i64 { return i64(9) } }`，方法体不读 `self`）：

把"**有调用点**的 `impl` 方法"登记进 `g.deadFuncs`（`markUnusedParams` 只遍历它），
登记点必须在**原型循环**里（放进收集循环会把原型排到 `#include` 之前，整份文件编不过）。
实测该形状从 `-Werror=unused-parameter` 失败变成**编译通过、运行正确** ✓。

**为什么仍然不落地**：量了输出面 —— 413 份里有 **410 份变化**：

| 类别 | 数量 |
|---|---|
| **只删 3 个空行**（外观） | **406** |
| 含代码删除（死代码消除） | 4（`tests/dyn/dyn_call.extc` 21 行、…）|
| 只插 `EXTC_UNUSED` | 0 |

目标 ③ 的判据是"变化不止**属性/死代码**两类 ⇒ 只留档不落地"，而"删空行"是第三类
（外观 ✓ 但确实是字节变化）⇒ 按判据回退，代码停在 `64e3c3b`。

**关于那 406 份"恰好 3 个空行"（round 8 更正，待验证）**：我上一轮把它写成"可独立修的伪影"，
但读代码后有了更可能的解释 —— 生成模板里**块与块之间恰好隔 3 个空行**，而 pass 的删除是**按区间**
的（`memmove` 精确删一段），"删掉这个块"自然连**它后面的分隔空行**一起删掉 ⇒ 于是每份被处理到的
文件都恰好少 3 行。若这个解释成立，它**不是**一个可单独修的 bug，而是"删除即连带分隔"的固有结果
⇒ ③ 的几百份字节抖动就是**激活 pass 的固有代价**，不是先修伪影就能避免的。

**round 9 实测（假设证实到形状，机制未钉死）**：在 worktree 里装上完整的 ③（含原型循环那段），
与主树编同一份 `examples/alloc-in-block.extc` 对比，差异是：

```
delete before[152:155]   删掉的正是 3 行空行
上下文: ['    int64_t len;', '};', '']   ← 上一行是 `struct slice_i32 { … };`
```

⇒ 那 3 个空行是**两个顶层项之间的分隔**（`struct slice_i32 {…};` 与下一个构造之间），被原地吃掉，
不是"某个被删块的尾巴"。但**谁吃的还没定**：现有 5 个 pass 里没有重建顶层项的，而两个 marking
pass 的重写是"整份逐字节拷贝 + 按偏移插入属性"，理论上不丢行 ⇒ 留作待查（它决定 ③ 能否字节无损落地）。

**round 10：这条路我追了四轮，没结论，先收手（记下方法与教训）**。最后两次尝试是：

* 在 pass 边界放"精确标记"探针（盯 `};\n\n\n\nstatic int32_t inner` 这段分隔）⇒ **从来没匹配上**，
  连"生成刚结束"那一刻也是"无" ⇒ 说明**我对形状的假设是错的**：diff 里的"删掉 3 行"很可能不是
  "字面意义上的三行空行被删"，而是某个 pass **重排/重写**了那一带文本（行数与内容同时变，diff 才
  显示成 3 行）—— 这也解释了此前若干次"按形状写守卫却毫无效果"。
* 教训：不要在 pass 边界猜形状，**应当在每个 pass 边界把整个缓冲区落盘**（`out->data`/`out->len`
  直接写文件），跑一遍之后逐对 diff ⇒ 一次就能指出是哪个 pass、改了哪一段。这个做法留作下次。

⇒ 它是**外观项**（不影响生成物正确性，只影响 ③ 能否字节无损落地），而目标的核心是 ①（fuzz 与修
真 bug）⇒ 本项按判据继续"只留档"，把力气留给战役。
若要验证这个解释：装上完整的 ③ 登记（含原型循环那一段），取一份变化的文件，看那 3 个空行是不是
紧跟在某个被删除的运行期块之后。

## 三点五、H5（已落地）：视图存储为空时索引会**往 null 写**

**来源**：战役5（三模式 × 5 种子 × 400 次）第 2 条真发现，变异自 `tests/pool/rt_promote.extc`：

```
/home/alphayang/extc-fuzz/w00086/case.c:819:91: runtime error: store to null pointer of type 'int32_t'
```

**根因**：视图索引原语只查下标范围，不查存储指针（`genViewIndexer`，`codegen.c:653`）：

```c
static inline int32_t *slice_i32_index(slice_i32 v, int64_t i, const char *file, int line) {
    if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);
    return &v.data[i];        /* data == NULL 且 i == 0 时，范围检查通过 ⇒ 直写 null */
}
```

变异后的程序里 `extc_pool_take` 什么都没给（`.data` 是 NULL），而切片仍宣称长度 4 ⇒
`slice_i32_index(s, 0, …)` 返回空指针 ⇒ 赋值写空。按 extC 的规矩这里**应当 trap**（解引用 null
要报错），实际放行成 UB ⇒ 这是健全性缺口，不是程序的错。

**修法**（已在 worktree 里验证，补丁存 `~/extc-work/h5-view-null-check.patch`）：

```c
    if (!v.data) extc_trapMsg(file, line, "the view has no storage");
    if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);
```

用已有的 `extc_trapMsg`，**不新增运行期原语**。实测复现用例从"往 null 写"变成
`case.extc:24: trap: the view has no storage`（退出码 1）✓。

**落地**：主人拍板"**健全性修复直接做，不用报**"（只有很严重的问题才需要报）⇒ 已落地并重设基准。

* 实测该改动让 **258 / 413** 份语料变化（凡是用到视图下标的程序多这一句检查），
  **0 份变成非法 C** ⇒ 全部仍是合法 C，基准已按当前编译器重算；
* 回归：`tests/traps/view_no_storage.extc`（该套件的判据是"必须 trap 且带源码位置"），
  parrun 由 286 增至 **287/0**；
* 补丁留档 `~/extc-work/h5-view-null-check.patch`。

**分工约定（主人 2026-09-28 定）**：**健全性修复（UB / 崩溃 / 生成物非法）自主落地**，
包括为此重设相应基准；只有**很严重**的问题（大范围语义变化、需要改语言规则、影响面无法估算）
才需要事先报备。这条规则写在这里，后续轮次照此执行，不再逐条请示。

## 三点六、H6（已落地）：字符串里的非法转义原样透传给 C 后端

**来源**：战役5 的第 3 条发现（`fail-00021`，原语料 `tests/pool/rt_nest_promote.extc`；战役6
又独立抓到同一条）——变异在字符串里插了 `\u64`。extC 说成功，gcc 拒绝整份文件：
`incomplete universal character name \u64`（C 的 `\u` 必须跟 4 位十六进制）。

**根因**：`lexString` 的注释写着"转义原样透传给 C 后端，parser 与检查器从不看里面" ⇒ 它只做
`lxAdvance` 跳过转义；而**字符字面量那边早就有**一个完整校验器（`lexCharEscape`：`n t r 0 \ ' " a b f v x`，
`\x` 要求至少一位十六进制，其余报错）。

**修法**：把那份唯一的权威改名为 `lexEscape(…, const char *what)`（错误措辞按种类），
`lexString` 复用它 ⇒ **一份列表管两种字面量**。

**踩到并修掉的坑**：`lexEscape` 会消费转义及其参数，我在循环末尾又 `lxAdvance()` 了一次
⇒ 每个转义多跳一个字符 ⇒ **197/413 份生成物的字符串文本被改**（闸门当场抓住）⇒ 改成 `continue`。

**验证**：复现用例报 `error: unknown escape '\u' in a string literal`；合法转义
（`\t \n \" \\ \x41`）照旧编译运行正确；`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0**
（纯加诊断，**零输出变化**）；回归 `tests/errors/bad_escape_in_string.extc`，parrun 287 → 288/0。

**更正上一轮的一条不实说法**：提交 `5659c28` 的信息里写"战役5 因此丢了一个用例"——
不准确。战役5 的 3 片失败**都存下来了**（`fail-00021` / `fail-00034` / `fail-00086`）。
目录名带 mode+seed 仍然是值得的加固（避免将来撞名覆盖），但那次并没有真丢东西。

## 三点七、H7（已落地）：数组与标量比较被放行 ⇒ 描述符比较越界读

**来源**：战役7（三模式 × 6 种子 × 500 次）的变异体（原语料 `examples/generic-free-fn.extc`，
同一种子在 `mutate` 与 `modules` 两个模式各命中一次）：

```
==ERROR: AddressSanitizer: stack-buffer-overflow ... READ of size 8
    #0 ... in extc_eq
```

**形状**：变异把 `a[i] == x` 削成 `[i] == x` ⇒ extC 把 `[i]` 当成 **1 元素数组字面量** ⇒
泛型实例化后是"`[1]i32` vs `i32`"的比较。生成物按**左操作数的描述符**（`array_1_i64`，元素 8 字节）
去读右操作数（`i32`，4 字节）⇒ 越界读。

**两处放行**（都要修，缺一不可）：

1. `check_escape.c` 的数组规则递归问"**元素**能不能和 `rhs` 比" ⇒ `i32` 能比 `i32` ⇒ 通过 ✗；
   改为"数组只能和**同一个数组类型**比，再判元素"，一行。
2. `check_top.c` 的延迟复查（`runOpCheck`）在"两边类型不等且左边不是结构体"时**直接 return** ✗
   ⇒ 那次比较从头到尾没人判。删掉这句提前返回，把判定交给 `typeSupportsOp` —— 它本来就是
   "这个类型能不能和那个类型比"的唯一权威（数值走 `cmpIsNative`、数组判元素与同型、结构体走
   `findOp`）。

**验证**：变异体现在报 `` `indexOf_i32` needs `[1]i64` to define `==` ``；
`examples/generic-free-fn.extc` **本体照旧编译运行正确**（`indexOf(30) = 2 …`，退出码 0）；
`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0**（**零输出变化**，收紧了判据而已）；
parrun ⇒ **288/0**（无任何误拒）；回归 `tests/errors/array_vs_scalar_eq.extc`。

## 三点八、定向攻击（tools/attack.py）：泛型组 47 题，先修 H8（实例化失控 ⇒ 挂死 + OOM）

**换打法**：不再只撒网（fuzz），而是**按实现结构点名攻击**。`tools/attack.py` 每组题都带期望
（ok / reject / trap），再叠三条硬 oracle（不许崩 / 说成功就必须合法 C / 不许 UB），组名可单独跑。

第一组 **泛型 47 题**（实例化身份与命名撞车、替换与签名、延迟检查即 H7 家族、每实例方法集隔离、
递归终止、与 dyn/协程交叉）。首轮命中 9 条，先修最严重的：

### H8（已落地）：自递归实例化让编译器失控（挂死 54 秒后被 OOM 杀掉）

**触发**（三行）：

```extc
struct box<T> { v: T }
fn grow<T>(x: T) -> i64 { var b: box<T>
  b.v = x
  return grow(b) }        // 每实例化一次，实参类型就深一层
```

**性质**：不是"慢" —— 它**同时吃爆内存**：每步的实参类型比上一步深一层（`box<T>` 再嵌一层），
第 N 步的类型有 N 层，而每份拷贝是 O(N) ⇒ 总量 O(N²) ⇒ 实测 54 秒后被 SIGKILL（OOM）。

**修法**：在 `funcInstance` 入口按**创建的类型的嵌套深度**设限（`TYPE_DEPTH_LIMIT 64`），
超限就用仓库既有的那句措辞报错（"This is a limit of the compiler, not something wrong with the
program; please report it …"），并给实例**总数**留一个兜底（`FUNC_INST_LIMIT 4096`）。
深度才是要害：只限个数时，4096 步 × 每步 O(深度) 照样把内存吃光（第一版就是这么失败的）。

**验证**：E3 现在**立即**返回
`error: internal: a generic instance was requested with a type nested 65 levels deep …`（退出码 1）；
十层嵌套（攻击套件 A4）不受影响；`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0**（零输出变化）；
parrun ⇒ 289/0。

### H9（已落地）：泛型函数返回 `T` 构造的实例 ⇒ 零值初始化用了**模板名**（生成的 C 非法）

**触发**（B14，最小的那个）：

```extc
struct box<T> { v: T }
fn wrap<T>(x: T) -> box<T> { var b: box<T>
  b.v = x
  return b }
```

生成物里**声明与初始化用了两个名字**：

```c
box_i64 b = (box_T){ .v = 0 };      /* gcc: `box_T` undeclared ⇒ 整份文件编不过 */
```

**根因**：`zeroValue` 收到的是**模板**类型 `box<T>`（检查器在替换 `T` 之前就把局部变量的类型
记下来了），而它随后"进入实例上下文"用的是 `t->targs` —— 那恰好就是 `[T]` ⇒ **用 T 替换 T**，
原地打转 ✗。声明侧用的是 `cType`，它**第一步就 `subst`** ⇒ 于是两处名字不一致。
（第一次尝试只把 `t->name` 换成 `cType(g, t)`，因为替换上下文是空的，输出**一点没变** ✗ ——
真正缺的是入口那一步 `subst`。）

**修法**：`zeroValue` 入口先 `t = subst(g, t);`（与 `cType` 同步），`TY_GENERIC` 分支用 `cType(g, t)`。

**验证**：B14 与 **A4（十层嵌套）**都产出合法 C 并运行正确（`box=8 pair=3,4`）；
`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0**（零输出变化 ⇒ 语料里没有这种形状，所以它一直是
"能编过就看不见"的暗坑）；回归 `tests/instimpl/t_gen_return.extc`（含 `pair<A,B>` 双参数版本）。

### 泛型组剩余的 7 条（按严重度排队，下一轮继续）

| 题目 | 症状 | 归类 |
|---|---|---|
| F4 | 泛型函数里开协程（`coroutine<T>`）⇒ **生成的 C 非法** | A4/B14 的同族残余 |
| A5 | 用户类型名 `pair_i64` 与实例 C 名撞车 ⇒ `redefinition of 'struct pair_i64'` | 生成非法 C |
| F1 | `dyn Tag` 打在"泛型 trait impl"的实例上 ⇒ 被误拒（`` `pair_i64` does not implement `Tag` ``）| 误拒 |
| B7 | `var c: coroutine<i64> = gen(i64(9))`（`gen<T> -> coroutine<T>`）⇒ 被误拒 | 误拒（待查）|
| B3 / B10 | 显式实参 `zero<i64>()` 不支持 ⇒ 但报的是"cannot infer type parameter" | 诊断措辞误导 |

## 三点九、H10（已落地，兜底）：用户类型名撞实例 C 名 ⇒ 生成非法 C

**来源**：攻击套件 A5（三行）：`struct pair_i64 { a: i64 }` 与 `pair<i64>` 的实例名 `pair_i64`
同时存在 ⇒ 生成的 C 里两份 `struct pair_i64` ⇒ gcc 报 redefinition，extC 却说成功。

**修法**（兜底）：在 `generateC` 入口核对"实例的 C 名 vs 用户声明"，撞了指名报错，并给另一处
交代怎么改。两个坑：① 放在检查器里**从不触发**（最后一批实例是在检查器之后才建的）；
② 第一版把编译器自己造的**方法持有者**（`pair<i64>` 的持有者也叫 `pair_i64`）当用户声明 ✗
⇒ 误报了 `impl pair<i64> { … }` 与泛型 receiver（套件 D3/F2）。判据最终用
"`sd->type->sdef != sd` ⇒ 编译器造的持有者，跳过"。

**验证**：A5 报 `` `pair_i64` is the name the compiler needs for an instance of `pair`, and the
program already defines it ``（退出码 1）；`tools/golden.sh` ⇒ 413/413 · 非法 C 0（零输出变化）；
parrun 290/0；回归 `tests/errors/instance_name_collision.extc`。

**注意这是兜底**：主人提出**根治**方案 —— codegen 生成的一切名字一律以**双下划线**开头，
语言层面禁止用户标识符以 `__` 开头（C 也是这么保留给实现的）。那样整族 mangling 冲突
（本条、`struct extc_arena` 撞运行期、`array_*`、`_index` 辅助函数…）一次性消失，
本条检查退化成"永不触发"的安全网。落地要改所有生成名的拼写 ⇒ 413 份基准全量重设。

## 三点十、F4（已定位到根，修法比预想深一层，未落地）：泛型协程**没有自己的帧**

**症状**（攻击套件 F4，泛型函数里开协程）：

```
invalid application of 'sizeof' to incomplete type 'struct gen_i64$frame'
EXTC_UNUSED static bool gen$step(struct gen$frame *f);      /* 模板的 step 反而"用了没定义" */
```

**根因链**（两次试错逐层剥出来的）：

1. 帧类型由 `checkFunc` 的协程 prologue 建立（`check_top.c:3834` 的 `if (f->isCoro)`），
   并且它把 `f->ret` **改写**成帧类型（`f->ret = cft`）✓；
2. 泛型实例是模板的**浅拷贝**（`funcInstance` 里 `*in = *tmpl`）⇒ 实例继承的是**已经改写过的**
   `ret`（模板的帧类型 ✗）⇒ `isProtoType(ret, "coroutine")` 为假；
3. 于是实例的协程 prologue 被跳过 ⇒ **没人给实例建帧** ⇒ 生成物里 `gen_i64$frame`
   只有使用、没有定义 ✗；
4. 第一次尝试（给"帧单元收集"那趟补上 `g.funcs`）**更糟** ✗：实例的 `coroFrameType` 仍指着模板的帧
   ⇒ `struct gen$frame` 被发两遍（redefinition）✗，而 `gen_i64$frame` 依然不存在；
5. 第二次尝试（记住写下来的 `coroutine<T>`，实例化时替换回来并清空 `coroFrameType`）也失败 ✗：
   实例的 body **不走** `checkFunc` 那个 prologue ⇒ `ret` 停留在原型 `coroutine<i64>` ⇒
   codegen 按"句柄"发成 `extc_coro` ✗，而它期待的其实是帧类型。

### F4 + B7 的共同根：实例**完全不跑**协程 prologue（拼图已完成三块中的两块）

**关键发现（第 10 轮）** ✓：实例根本不走 `checkFunc` —— 检查 body 那一趟写着
`if (fx->tmpl) continue;   /* covered by the per-instance recheck */`（`check_top.c:5204` ✓），
而"per-instance recheck"只是**延迟检查**（算符 ✓ 方法 ✓ 可赋值性 ✓），**从未建帧、也从未替换
协议方法的类型** ✗。B7（`c.value()` 的类型还是 `T` ✗）与 F4（实例没有帧 ⇒ 生成非法 C ✗）
因此**同一个根** ✓。

**三块拼图（都已实测 ✓，还差最后一块 ✗）**：

1. ✓ `ast.h` 记 `coroRetProto`（写下来的 `coroutine<T>` ✓ —— 模板的 `ret` 会被改写成帧 ✓，
   实例是浅拷贝 ⇒ 必须能重建原型 ✓）；`funcInstance` 里重建原型 + 清空继承来的帧 + `isCoro=false` ✓。
   实测：帧名变成**实例自己的** `struct gen_i64$frame` ✓，B7 的调用也变成了
   `extc_coro_value_int64_t` ✓（`T` 替换成功 ✓）；
2. ✓ 让协程实例走 `checkFunc`：把 `if (fx->tmpl) continue;` 放宽成
   `if (fx->tmpl && !fx->tmpl->isCoro) continue;` ✓ —— prologue 就在里面 ✓，那里的注释本来就写着
   "检查泛型实例会把同一个 body 再走一遍" ✓（同一顺序 ⇒ 生成的 C 名不会漂移 ✓）；
3. ✗ **还差**：① 该帧结构没进**单元列表**（"用了没定义" ⇒ 早先那次"给帧收集补一趟 `g.funcs`"的
   尝试 ✗ 当时失败是因为帧还不存在 ✓，现在帧真的存在了 ✓ ⇒ 这两块可以合起来 ✓）；
   ② 协程**运行期文本**（`extc_coro` / `extc_task_*` ✓，来自 `src/coroutine.c` ✓）没被触发 ✓
   （`unknown type name 'extc_coro'` ✓）⇒ 要找到它的触发条件并让"有协程实例"也算数 ✓。

**第 11 轮：四块补丁一起落 ⇒ 只剩两个精确的阻碍** ✓（都已定位 ✓、都已回退 ✓）

- ✗ **F4**：`unknown type name 'extc_coro'` ⇒ 协程的 **typedef 与运行期文本**（`codegen.c:6496` 的
  `typedef struct ExtcCoroS extc_coro;` + `src/coroutine.c` 的 `extc_task_*`）**没被发出** ✓
  ⇒ 触发条件要找到并让"有协程实例"也算数 ✓；
- ✗ **B7**：`redefinition of 'struct gen$frame'` ⇒ 说明实例的 `coroFrameType` **仍然指向模板的帧** ✓
  （否则我的"帧单元也收实例"那趟不会把模板帧登记两遍 ✓）⇒ 也就是**建帧那段对这个实例没跑/没生效** ✓
  ⇒ 下一步应在 prologue 里打点确认：实例进来时 `isCoro` 是否为真 ✓、`f->coroFrameType` 是否真的被
  换成新帧 ✓。

**四块补丁（原型字段 ✓ 实例重建 ✓ 放行实例 ✓ 帧单元收实例 ✓）都已回退 ✓**，闸门保持 413/413 ✓；
两个阻碍各自只剩一处待查 ✓。

### 第 25 轮：六块拼图全部落地并实测 ✓，卡在**最后一块的信号来源**（已回退 ✓）

**这一轮把整条链走通了** ✓（每块都实测 ✓）：

1. ✓ `ast.h` 记 `coroRetProto` + `funcInstance` 重建原型、清继承帧、`isCoro=false`；
2. ✓ 让协程实例走 `checkFunc`（`if (fx->tmpl && !(fx->tmpl->isCoro)) continue;`）；
3. ✓ **打点证实**（`EXTC_CORO_DBG`）：实例确实进了 prologue ✓
   （`[coro] gen isCoro=1 tmpl=gen frame=NULL proto=有` ✓）；
4. ✓ 帧名原来取自 `f->name`（实例沿用的是**模板的 name** ✓）⇒ 模板与实例的帧都叫 `gen$frame`
   ⇒ `redefinition` ✗。改用 **`instName` 优先**（与 codegen 的 `cFuncName` 同规则 ✓）⇒ **该症状消失** ✓；
5. ✓ 帧单元收集补上实例 ⇒ 实例的帧**被定义**了 ✓；
6. ✗ **最后一块**：`extc_coro` 的 typedef 与协程运行期（`extc_task_begin` 等）由一个**函数体发射之前**
   的预扫决定（`codegen.c:6254-6261` ✓ 的 `g.needCoroHandle` ✓）⇒ 而"**这个协程是否被装箱**"的决定
   **只在函数体发射时才做出来** ✗ ⇒ 预扫拿不到 ✓。试了两个现成信号都不对 ✓：
   `coroBoxed` 在这个程序里**连模板都是 0** ✗；`coroNeedsZone` 会把不该要的也算进来 ✗
   （闸门当场变红 2 份：`tests/coro/coro_tasks.extc` 等 ✓）。

**第 27 轮：按需拼接试通了一半，拿到了"该怎么做"的准确形状** ✓（已回退 ✗）

- 拼接机制在仓库里**已有先例** ✓：`primA`（`codegen.c:6519` ✓）捕获"运行期原语块"的偏移 ✓，
  最终装配时用 `bufPutn(&pb, out->data + primA, out->len - primA)` 之类把新片段**插回前面** ✓
  ⇒ 协程 handle typedef 用同一招 ✓；
- 在**早期 typedef 块之后**捕获偏移 ✓，体发射时若 `g->needCoroHandle` 才置位 ✓（装箱点
  `extc_task_begin` 那两处 ✓），最终装配时**按需插入** ✓ —— 机制本身工作 ✓；
- ✗ 第一次试出了 `redefinition of 'struct ExtcCoroS'` ✓：早期块**也**打了这份定义 ✓ ⇒
  插入必须**仅在"早期块没打过"时**做 ✓（**typedef 可以重复，结构体定义不行** ✓）⇒ 形状是：
  早期块之后记 `coroDefPrinted = g.needCoroHandle;` ✓（此时为真就别再插 ✓）；
- ✗ 第二次的脚本在应用补丁时**断言中断**（锚点没匹配上 ✓）⇒ 只落了一部分 ✓，已整体回退 ✓，
  闸门保持 413/413 ✓。

**第 29 轮：找到真正的根 —— 实例的创建时机** ✓✓（决定性 ✓）

逐块手工应用第 1–5 块（`coroRetProto` ✓、记录 ✓、`funcInstance` 重建 ✓、放行实例 ✓、
**帧名 `instName` 优先** ✓）⇒ 构建干净、闸门绿 ✓。但 F4 的生成物里**只有 `gen$frame`** ✗。
打点（`EXTC_FR=1` ✓）得到全部帧创建记录：

```
[frame] f=gen  inst=-      owner=gen    => gen$frame      ← 模板
[frame] f=gen  inst=gen_T  owner=gen_T  => gen_T$frame    ← 一个**临时**实例（targs 就是 T）
```

⇒ **真实例 `gen_i64` 从没进 prologue** ✗。原因是**创建时机** ✓：`gen(i64(9))` 写在**泛型函数**
`run<T>` 的身体里 ✓ ⇒ 这个调用属于**延迟检查** ✗ ⇒ 实例 `gen_i64` 是**延迟实例化那一趟**
（在"检查 body"那一趟**之后** ✓）才建出来的 ✓ ⇒ 第 4 块放行的那个循环（遍历 `m->funcs` ✓）
**那时已经跑完**了 ✓ ⇒ `checkFunc(gen_i64)` 从未被调用 ✓✓ ⇒ 协程 prologue 自然没跑 ✓。

**因此正确的修法（第 4 块要换位置）** ✓：协程实例的"设置"不能只挂在"body 检查那一趟放行实例"上 ✓，
而要在**延迟实例化之后**再补一趟 —— 对 `c.funcInsts` 里每个**协程实例**跑一次协程设置
（建帧 + 协议方法替换 ✓），或者把这套设置从 `checkFunc` 里**提出来**成独立函数 ✓，
在"实例建好之后"调用 ✓。前三块（字段/记录/重建）与第 5 块（帧名 `instName`）都已验证正确 ✓，
可以照用 ✓。**本轮已整体回退** ✓（闸门 413/413 ✓）。

**第 30 轮 Step A（已落地 ✓）：把协程设置整段提取成 `coroSetup(c, f)`**

按第 29 轮的修法方向，先把 `checkFunc` 里那 105 行的协程设置（handle 协议 + 帧类型 + 帧上的协议
方法）**整段提取**成 `static void coroSetup(Checker *c, FuncDef *f)` ✓，`checkFunc` 里只留
`if (f->isCoro) coroSetup(c, f);` ✓。判据是"**纯重构必须零变化**" ✓：
`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0** ✓、parrun **295/0** ✓、`make` 零诊断 ✓
⇒ 行为不变**可证** ✓。**Step B**（下一轮 ✓）：第 1–3 块（`coroRetProto` ✓ 记录 ✓ 重建 ✓）+
第 5 块（帧名 `instName` 优先 ✓）+ 在**延迟实例化之后**对 `c.funcInsts` 里的协程实例补跑
`coroSetup` ✓（第 4 块那种"body 检查放行实例"证明不行 ✓：那时实例还没建 ✓）。

**第 31 轮 Step B：一路推到只剩"拼接点位置"这一件事** ✓（已回退 ✗，但每一步的实测都在）

逐块落地并实测（每块都过闸门 ✓，语料始终零变化 ✓）：

1. ✓ `coroRetProto` 字段 + 记录 ✓；`funcInstance` 重建原型/清继承帧/`isCoro=false` ✓；
2. ✓ 帧名 **`instName` 优先** ✓（`coroSetup` 里 ✓）；
3. ✓ **延迟回放之后补跑 `coroSetup`** ✓（锚点：callChecks 回放循环之后、`#79` 注释之前 ✓）——
   这一块是**第 29 轮真根的正解** ✓（实例是那时才建出来的 ✓）；给实例先替换
   `yieldTime`/`isCoro=true` ✓；
4. ✓ 生成器：`coroDefPrinted`/`coroDefA` + 装箱点（`extc_task_begin` 两处 ✓）置位 ✓ +
   **带守卫的按需插入** ✓。插入的 typedef **生效** ✓（`extc_coro` 未定义的错误消失 ✓！）；
5. ✓ 任务表运行期（`coroutineEmitRuntime` ✓）原来只在"程序自己声明了 `extc_task_*`"时才发 ✗
   ⇒ 一并放进插入 ✓ ⇒ `extc_task_begin` 的错误消失 ✓；
6. ✓ 池运行期（`poolsEmitRuntime` ✓）也要在前面（任务表用到 `extc_arena` ✓）⇒ 再放进插入 ✓
   （并用 `poolDone` 防止最终装配重复发 ✓）⇒ `extc_arena` 的错误消失 ✓；
7. ✗ **只剩拼接点的位置** ✓：必须在"**`#include` 之后 + 死钩子块之后 + 所有函数体之前**" ✓。
   实测走过三个位置：`genCoroHandleDecls` 之前（❌ `int64_t` 未知 ⇒ 落在 include 之前 ✓）、
   早期 typedef 块之后（❌ 池运行期里的 `extc_die` 未知 ⇒ 死钩子在它之后 ✓）、
   ⇒ **正解：死钩子块结束处** ✓ —— `codegen.c:6527` 写 `g.rtDie` ✓、**块结束在 `:6547`** ✓，
   紧接 `:6548` 是下一段输出 ✓ ⇒ 捕获点放这两行之间 ✓。

⇒ 本轮已整体回退 ✓（闸门 413/413 ✓）；下一轮只需把那**一个捕获点**放对 ✓，前面的 1–6 块照用 ✓。

**第 32 轮：整套补丁就绪，只剩"捕获偏移"落点** ✓（已回退 ✗，但每块都实测过 ✓）

十二处编辑全部用精确锚点落地 ✓、构建干净 ✓、语料闸门始终 **413/413 · 非法 C 0** ✓，逐步实测的
错误链条是：

```
unknown type name 'extc_coro'      ⇒ 按需插入 typedef 之后消失 ✓
implicit declaration extc_task_begin ⇒ 把任务表运行期一起插入后消失 ✓
unknown type name 'extc_arena'      ⇒ 需要池运行期（它在最终装配里发，晚于函数体 ✓）
unknown type name 'extc_die'        ⇒ 池运行期调死钩子 ⇒ 拼接点必须在死钩子之后 ✓
```

打点（`EXTC_SP=1`）确认拼接**确实执行**：`need=1 printed=0 A=1204 task=0 pool=0` ✓；
而 `extc_ablock`/`extc_arena` 的 typedef 在 **`generateC` 自己**的一大段 `bufPuts`（`:6651-6680+` ✓，
跨几十行字符串 ✓）里 ⇒ 捕获点必须在这**整段之后** ✓。两次定位失败：

- 用"找 `        );`"⇒ 该语句的收尾**不是**这个形状 ✗（`assert no closing );` ✓）；
- 用 `extc_arena` 那句之后的第一个 `);` ⇒ 落在**字符串内部** ✗（构建报 missing terminating `"` ✓）。

⇒ **下一轮的两条路**（任选其一 ✓）：① 读 `:6700-6780` 找到那条长 `bufPuts` 的**真实收尾** ✓
（按行号 ✓ 不猜文本 ✓）；② 更稳的：把 `extc_ablock`/`extc_arena` 的 typedef **一并放进插入文本** ✓，
并给预奏里那份加 `#ifndef` 守卫 ✗（结构体定义不可重复 ⇒ 需要动预奏 ✓）—— 所以 ① 更干净 ✓。

**第 33 轮：链条推到只剩"顺序"一项** ✓（已回退 ✗，链条完整 ✓）

十三块编辑 + 精确捕获点（**按内容定位**：`extc_arena` 那句 typedef 之后**第一个以 `");` 结尾的行**
= `codegen.c:6699` ✓；后来发现更好的落点是**整个预奏运行期之后**，即 `:6792` 那个带注释的块之前 ✓）。
逐步实测的**完整错误链**（每一步都消掉上一环 ✓）：

```
unknown type name 'extc_coro'              ⇒ 拼接 typedef ✓
implicit declaration extc_task_begin       ⇒ 拼接任务表运行期 ✓
unknown type name 'extc_arena'             ⇒ 拼接池运行期 ✓
invalid application of sizeof … incomplete ⇒ 帧单元收集补上实例（第⑤块 ✓）
has no member named 'pc'                   ⇒ 补调 coroFrameLay（`:6396` ✓，帧字段那一趟）
implicit declaration extc_coro_next        ⇒ 把 genCoroHandleDecls（`:4138` ✓）也渲染进拼接 ✓
'struct gen$frame' declared inside parameter list ⇒ **只剩顺序**：步骤原型出现在帧结构之前 ✗
```

⇒ **只剩一项** ✓：`genCoroHandleDecls` 里那批 `$step` **原型**要排在**帧结构定义之后** ✓
（现在拼接把它们放在帧单元之前 ✓；非泛型协程之所以没事 ✓，是因为它的帧结构由类型通道更早发出 ✓）。
下一轮做法：把 `$step` 原型那一段**单独挪到单元区之后** ✓，或在拼接文本里**连帧结构一起插** ✓。

**第 34 轮：顺序修好之后只剩两条"声明/定义"错配** ✓（已回退 ✗）

按上一轮的判断把 `genCoroHandleDecls` **挪到 `emitDescRegion` 之后**再调一次 ✓（早调那次在
`needCoroHandle` 置位前空转 ✓，用 `coroDeclsDone` 记住 ✓）⇒ "`struct gen$frame` declared inside
parameter list" 消失 ✓。剩下两条，都已定位到行 ✓：

1. **F4**：`gen$step declared 'static' but never defined` ✗ —— `$step` 声明的循环
   （`codegen.c:4250-4252` ✓）写的是 `if (!f->isCoro || f->tmpl) continue;` ✓ ⇒ 它给**模板**声明、
   **跳过实例** ✗，而**定义**来自 `genFunc`（发实例 ✓ 不发模板 ✗）⇒ 泛型模板的声明成了孤声明 ✓。
   修法：该循环跳过**泛型模板**（`f->typeParams.len > 0` ✓）✓，实例的声明本来就由别处发 ✓；
2. **B7**：`extc_coro_value_int64_t` 未声明 ✗ —— 取值助手在 `genCoroHandleDecls` 里按 **yield 类型**
   发（`codegen.c:4190` ✓）✓，我的晚调覆盖它 ✓，但那条"按类型"的列表很可能**以模板为键** ✗
   ⇒ 实例的类型没登记 ✓。修法：登记时用**实例自己的** yield 类型 ✓（或把列表改成按具体类型 ✓）。

⇒ 两条都只差一处 ✓；本轮整体回退 ✓（闸门 413/413 ✓）。

**第 35 轮：把"跳过实例"的三处一起改成"收实例、跳过泛型模板"** ✓（已回退 ✗，剩两条枝节）

改动 ✓：预扫的 `coroKind` 分配（`:4143` 一带 ✓）、`$step`/`$next` 声明（`:4145` ✓）、取值助手的
`switch`（`:4191` ✓）、以及 `$next` 驱动循环（`:4252` ✓）全部改成
`(!cf->tmpl && cf->typeParams.len > 0) ⇒ 跳过`（即**收实例、跳过泛型模板** ✓）——
与前一轮"帧单元收实例"（第⑤块 ✓）同一口径 ✓。结果 ✓：

- ✗ 新枝节：**临时实例** `gen_T`（`targs` 就是 `T` ✓）也被收进来了 ⇒
  `'struct gen_T$frame' declared inside parameter list` ✗ ⇒ 用现成的 **`provisionalInstance(&cf->targs)`**
  跳过即可 ✓（检查器里已有这个 helper ✓，第 6/7 轮的延迟检查就在用 ✓）；
- ✗ B7 的 `extc_coro_value_int64_t` 仍缺 ✓：取值助手循环已收实例 ✓，但那条"按 yield 类型"的路径
  还是没发出实例的类型 ✓ ⇒ 下一轮先打点（`EXTC_CV=1`：打印每个候选的 `cType(yieldType)` 与
  `coroKind` ✓）再看是"没进循环"还是"kind 不匹配" ✓。

**第 36 轮：收手（本轮按 goal 规矩停手，不再追这一条）** ✓

十八处编辑全部落地 ✓（还补了 codegen 侧的 `coroProvisional` 助手 —— 用现成的 `mentionsParam` ✓，
因为检查器的 `provisionalInstance` 是 static ✓），构建干净 ✓、语料闸门 **413/413 · 非法 C 0** ✓，
但 F4/B7 仍各差一处：`gen$step used but never defined`（还有**第三个** `$step` 声明点 ✗）与
`extc_coro_value_int64_t`（按 yield 类型那条路径仍未登记实例 ✗）。

⇒ **结论** ✓：这条链共 **十几处同源点**（"谁被发射"与"谁被声明/登记"两套口径不一致 ✓），每轮都能
消掉一环、也都会冒出下一环 ✓ ⇒ 按 goal 的"修不动就记档、继续做别的" ✓ **停手** ✓，把完整清单与
错误链留在档里 ✓（对下一个接手的人是一张现成的地图 ✓）。**整体回退** ✓，闸门 413/413 ✓。

**下一阶段的入口（照此继续即可 ✓）**：
1. `$step` 声明共 **三处**：`codegen.c:4145`（✓ 已改口径）、`:4252`（✓ 已改）、以及**尚未找到的第三处**
   （第 36 轮实测：即使前两处都收实例、跳过泛型模板与临时实例 ✓，`gen$step` 仍被声明 ✗）
   ⇒ 用 `grep -n '\$step' src/codegen.c` 逐个确认，或给**泛型模板**加"永不发射"的统一判据 ✓；
2. 取值助手：把"按 yield 类型"的登记从**模板**改为**实例**（`codegen.c:4189-4194` ✓ 的
   `cType(ck2->yieldType)` ✓ 口径与 `:4183` 的 `yt` 要对齐 ✓）；
3. 判据：`tools/attack.py generics` 里的 **B7 + F4** 两条 ✓（现在 47 题剩 3 条 ✓）。

**原清单（每一块都验证过或已定位 ✓）**：① `ast.h` 的 `coroRetProto` + 记录 ✓；
② `funcInstance` 重建原型/清帧/`isCoro=false` ✓；③ 放行协程实例走 `checkFunc` ✓；
④ 帧名 **`instName` 优先** ✓（这条让 `redefinition` 消失 ✓，第 25 轮实测 ✓）；
⑤ 帧单元收集收实例 ✓；⑥ 生成器：`coroDefPrinted`/`coroDefA` + 装箱点置位 + **带守卫的按需插入** ✓。

**原记的修法（仍适用）**：要么把 handle typedef 与运行期改成**按需拼接**
（body 发射时发现需要再插到前面 ✓ —— 仓库里"final assembly"那段已经在做类似的事 ✓），
要么在预扫里**重放装箱判定**（与调用点同一套判据 ✓，不能靠新标志 ✓）。六块补丁**全部已回退** ✓，
闸门保持 413/413 ✓；本轮的净产出是**这条完整的根因链**与"哪一块是错的" ✓。

**正确的修法（原记，仍适用，第 1-5 块已验证 ✓）**：帧合成要放进**实例的 body 检查路径**（就是"检查泛型实例会把同一个 body 再走一遍"
那条路径），而不是 `checkFunc` 的 prologue —— 让实例在那里建自己的 `<实例名>$frame`（字段按实参
替换），帧单元收集自然也就对了。两次试错都已回退，代码停在 `084303c`。

## 三点十一′、A4 **结案：是我套件自己的题写错了**（并据此更正前两轮的判断）

**结案实测**：套件里那版 A4（`b.v.v…v = a.v` + 读回 ✓）直接编 + `gcc -Werror` + ASan/UBSan 参数
⇒ gcc 退出码 **0**、运行退出码 1（套件判据里"非零退出"不算失败 ✓）⇒ **A4 通过** ✓。
重跑 `tools/attack.py generics` ⇒ **47 题剩 5 条** ✓（A4 已消失 ✓）。

**我错在哪**：那道题原本声明了 `b` 却**不使用**它 ✓ ⇒ `-Werror=unused-variable` ✓；你早先让我
"把题目改成会用 `b`" ✓，我改的是**套件里**那份 ✓，而我后来三轮一直拿 **`/tmp/a4.extc` 的旧副本**
做实验 ✗ ⇒ 于是看到的是"残骸 ⇒ `unused-value`" ✗ —— 那是**另一个**现象 ✓，与套件的 A4 无关 ✗。

**从旧副本里确实留下一条独立的小问题**（与本条分开、待排期）：当局部变量**真的没人用**时，
`dropUnusedLocals` 把这 415 字节的声明作为一刀删掉 ✓（探针实测区间正确 ✓），可最终产物里仍留下
一句裸的复合字面量 ✗ ⇒ `gcc -Werror` 报 `value computed is not used` ✗。它只在 `-Werror` 下可见 ✓、
只在"未被使用的深嵌套泛型局部变量"这一形状上出现 ✓，属于**质量类**（不是健全性 ✓）——
留档在案，不占主线。

## 三点十一、A4（旧副本的进一步观察，保留备查）：切点算对了，但最终产物仍有半截

**现象**（攻击套件 A4）：十层嵌套泛型的局部变量 `b` ⇒ 生成物里**声明消失、只剩一句裸的复合字面量**
⇒ `gcc -Werror` 报 `value computed is not used`（不带 `-Werror` 时只是警告，所以平时看不见）。

**逐层排除**（每一步都是实测，不是推理）：

| 观察 | 结论 |
|---|---|
| 关掉 `dropUnusedLocals` ⇒ 声明**完整正确**；开着 ⇒ 只剩初始化表达式 | 是本趟 pass 干的 |
| `DeadLocal` 记录：`b` 只有**一条**，`textlen=415`、**1 个换行**（单行）| 不是"登记两次"，也不是多行文本 |
| 切点：`dl` 找到；`[apply] func=main cuts=1` | 确实切了一刀 |
| 那一刀的实际区间：`start=11760 end=12175 len=415`，删掉的正是**完整声明** | 切点**算对了** |
| 但跳过关卡时，`main` 里**只有声明、没有裸表达式** | 残骸**不是** codegen 多发的 |

⇒ 结论：**切点按一份文本算、应用时落在另一份文本上**（偏移/文本不同步 —— 这一族在本轮里已是第三次
出现：② 的 `bl` 过期、H9、A4）。**下一轮的打点**：在应用切点**之后**，打印该区域内实际留下的字节
（以及"实际删掉的字节数" ✓），看它与 415 的差在哪里 ⇒ 然后照 ② 的成熟做法修：**应用前重新定位并
校验完整匹配，失配就跳过**（宁可不删 ✓ 绝不删半截 ✗）。

## 三点十二、F1（两段式，第一段已做通、第二段待做，故整体未落地）：`dyn` 打在泛型 impl 的实例上

**症状**（攻击套件 F1）：`` `pair_i64` does not implement `Tag` `` —— 而 `impl<T> Tag for pair<T>` 明明写了。

**第一段（检查器，已验证可行 ✓）**：`dynTraitOf`（`check_expr.c:435`）用
`ttEquals(im->target, pt)` 找实现，而泛型 impl 记录的 `target` 是**模板** `pair<T>`（`targs` 是块级
类型参数）⇒ 与实例 `pair<i64>` 永不相等。补一条"**同一个 `sdef` + 该 impl 有块级参数** ⇒ 视为覆盖
所有实例"（语义上正是 `impl<T> … for pair<T>` 的意思；每实例的一致性检查已在别处做过）⇒ 检查器
**放行** ✓。

**第二段（codegen，未做 ✗）**：放行之后生成物变成**非法 C** ✗ —— 表和 thunk 是按**模板**发的：

```
extc_vt$Tag$pair_i64   —— 使用处（对的）
extc_vt$Tag$pair_T     —— 定义处（模板名 ✗，于是"未声明"）
extc_th$Tag$pair_T$tag → 里面用 pair_tag((pair_T *)self)   ✗ pair_T 根本不存在
```

⇒ 需要让**泛型 impl 也按实例发表与 thunk**（表名用 `vtKey` 的实例名、thunk 里用实例的 C 类型）。
这与第 6/7 轮统一过的 `methodSetOf`/`vtKey` 是同一套机制 —— 那次统一了"表名从哪来"，这次要统一
"**泛型 impl 的实例也要各发一份**"。

**为什么不落地半段**：只改检查器会把"误拒"变成"生成非法 C" ✗（更糟）⇒ 已回退，两段一起做才算数。

**第二段的两次尝试（都回退，但把范围缩到最后一处）**：

- 尝试 1（已做通一半 ✓）：把 `for (… m->impls …)` 的循环头改成"泛型 impl ⇒ 对模板的**每个实例**
  各发一份表"（`g.insts` 里 `sdef` 相同的实例 ✓），循环体一行不动 ✓。结果：表名对了 ✓
  （`extc_vt$Tag$pair_i64` ✓，不再是 `pair_T` ✓），但 **thunk 体仍调 `pair_tag`** ✗；
- 尝试 2 ✗：把 `tsd = bt->mholder ? bt->mholder : bt->sdef` 换成权威 `methodSetOf(bt)` ⇒ **没变** ✗
  —— 因为 `pair<i64>` 的 `sdef` 本来就是模板 `pair` ✓，实例方法与模板同住一个声明 ✓ ⇒
  `methodSetOf` 返回的还是模板 ✓。

**于是范围缩到一处** ✓：thunk 里要的是**带实例限定的方法 C 名**（`pair_i64_tag` ✓，即第 6 轮
"按实例解析方法"那套命名 ✓），而 thunk 是**在任何函数上下文之外**写的 ⇒ 必须显式用实例上下文
求名（`cFuncName` + 实例替换 ✓），不能靠"当前函数"的隐式状态 ✓。

**尝试 3（找到了正确的着力点 ✓，但脚本改写的插入点算错 ⇒ 已回退 ✗）**：

- **关键发现** ✓：实例限定的方法 C 名由 **`g->ownerPrefix`** 决定 —— `cFuncName`（`codegen.c:569`）里
  就是 `if (g->ownerPrefix) return "<prefix>_<method>"` ✓（注释写着"a generic instance uses its own
  name" ✓）。所以在发 thunk 时把 `ownerPrefix` 临时设成实例的 C 名（`pair_i64` ✓），名字自然就是
  `pair_i64_tag` ✓ —— 一处 `保存/设置/恢复` 即可 ✓，不需要任何新命名逻辑 ✓；
- ✗ 失败原因纯属我自己的脚本：用"从替换段之后找第一个 `{`"来定位循环体末尾 ✓，可外层 `for` 的 `{`
  已经包含在替换段里 ✓ ⇒ 插入点落进了循环体内部 ⇒ 编译错（`expected ')' before ';'` ✓）⇒ 回退 ✓。
  **教训**：这种改写要在**原文**里用精确锚点（例如 `const char *vtKey = cType(&g, bt);` 与循环体末尾
  的 `bufPrintf(&vtDefs, "static const struct extc_vt$%s_t __attribute__…` 之后）手工定位 ✓，
  不要靠脚本猜括号 ✓。

**尝试 4（三步全部手工落地 ✓，差最后一块 ✗）**：用**逐字锚点切片**改写（不再猜括号 ✓）⇒

- ✓ 第①步（检查器放行）与第②步（按实例发表）**都成功**：表名 `extc_vt$Tag$pair_i64` ✓；
- ✓ 第③步（`g->ownerPrefix = vtKey` ✓ 保存/恢复 ✓）**也成功**：thunk 体变成了
  `return pair_i64_tag((pair_i64 *)self);` ✓ —— 名字正是实例限定的那个 ✓；
- ✗ **但 `pair_i64_tag` 从未被生成** ⇒ `implicit declaration of function 'pair_i64_tag'` ✗。

⇒ **最后一块** ✓：泛型 impl 的方法实例（`pair_i64_tag` ✓）只有在**被调用**时才由调用点造出来 ✓，
被 **vt 表引用**时没人造它 ✗ ⇒ 表发射这一趟必须**主动为该实例造/取方法实例**（走第 6 轮那套
"按实例解析方法"的入口 ✓，而不是直接用 `tsd->methods` 里的模板方法 ✓），这样 codegen 才会把它的
函数体也发出来 ✓。

**第 24 轮：第④步的原语仍未锁定（但排除法很有信息量 ✓）**

- 找到**一致性检查**的位置 ✓：`check_top.c:249-295`（`impl<T> Trait for pair<T>` 的签名核对 ✓）——
  它**只校验、不造实例** ✗；
- 打点 `funcInstance` ✓（`EXTC_FI=1` 打印模板名/宿主/参数数 ✓），跑一个**能编能跑的直接调用**
  程序（`p.tag()` ✓，生成物里确实有 `pair_i64_tag` ✓）⇒ **`funcInstance` 一次都没被调用** ✗
  ⇒ 方法实例是**另一个原语**造的 ✓；
- 候选位置：**持有者克隆**那段 ✓（`check_top.c:5081-5095` ✓，为"`impl` 指定一个**实例**"造
  `t->mholder` 并从声明抄一份方法 ✓）与注释里说的 "`resolveSignature` + `ttSubstitute` over
  `tt->instances`" ✓。

⇒ **结论** ✓：F1 缺的正是"**只被 vt 表引用、从没被调用**"时没人造方法实例 ✓ ⇒ 修法应在一致性
检查那一趟里，对**泛型 impl** 的每个目标实例主动物化一次方法实例 ✓（用的是上面那个原语 ✓）。
已经试过 4 次、都回退 ✓；这条是**功能缺口**（dyn × 泛型 impl ✓）不是健全性问题 ✓ ⇒ 按 goal 规矩
**留档、继续做别的** ✓（余下轮次给 B7+F4 ✓，它四块拼图已实测 ✓）。

**注意闸门** ✓（尝试 4 实测：三步落地后闸门**仍是绿的** 413/413 ✓ —— 语料里的 dyn+泛型 impl 组合
要么没走到这条路径、要么本来就正常 ✓；先前两次变红是我改坏了中间状态 ✗）：这条修到底后**可能**会让
`tools/golden.sh` 变红（**5 份产物不同 + 5 份非法 C** ✗，
`tests/dyn/dyn_stored_call.extc` 等 ✓）—— 说明语料里**确有**泛型 impl + dyn 的组合 ✓，所以这条修好
之后要**重设这几份基准** ✓（并在提交信息里写明"新增按实例的 vt 表" ✓）。

## 三点十三、B3/B10（已查清，未落地）：显式类型实参**接受但不喂推断** + 诊断推荐了无效写法

**实测三形状**（第 12 轮）：

| 写法 | 结果 |
|---|---|
| `maxOf<i32>(4, 3)`（注释里的官方例子）| **通过** ✓ —— 但那只是因为**推断本来就成功**（`T` 从实参得到 `i32`）✓ |
| `f<i64>(i64(1))`（`T` 只在返回类型出现）| ✗ `cannot infer type parameter T` |
| `zero<i64>()`（无实参）| ✗ 同上 |

⇒ 路径是有的 ✓：`EX_GENCALL`（`check_expr.c:1448` ✓）建好实例 ✓、把节点改写成 `EX_CALL` ✓，
**但没把写下来的类型实参带过去** ✗ ⇒ `EX_CALL` 从头再推断一次 ✓ ⇒ 显式实参成了**纯装饰** ✗。
而两处诊断偏偏写着 "write it explicitly: `f<i32>(...)`" ✗ —— **推荐的正是那个不工作的写法** ✓
（两处文本完全相同 ⇒ 改的时候要一起改 ✓；`s.count(...)==2` ✓）。

**两次尝试与结论** ✓：

- 在 `Expr` 的 call 成员里加 `targs`/`hasExplicitTargs` 字段 ✗ ⇒ **这个 union 不清零** ⇒
  普通调用节点读到**垃圾长度** ⇒ 误种子 ⇒ 语料 `tests/stl/sort.extc`（`sort::sort(x)`）
  当场报 "cannot infer" ✗ ⇒ 新字段在有初始化遍之前**不可用** ✓（这是本次最有价值的发现 ✓）；
- 改用**已初始化**的 `e->func`（改写时指向实例 ✓，其实例 `targs` 就是显式实参 ✓）读回 ✗ ⇒
  **没生效**（`f<i64>`/`zero<i64>()` 仍失败 ✓）⇒ 说明 `EX_CALL` 路径在种子之前**重新解析了 callee** ✓、
  把 `e->func` 覆盖掉了 ✓。

**下一步（顺序明确）** ✓：① 先让 `exprNew` **清零整个 union**（一处 ✓，受益于所有将来加的字段 ✓）；
② 再加字段并在改写的节点上置位 ✓；③ `EX_CALL` 的推断用它预填 `targs`（`unifyTParams` 对已填槽位
本就跳过 ✓ 见 `vecPush(NULL)` 那段的模式 ✓）；④ 两处诊断文案一起改成实情 ✓（写出来**不参与**推断 ✓，
给出真正能用的做法 ✓）。前三步都不动既有产物 ✓，第四步只改字符串 ✓。

## 三点十四、B3/B10 的**诊断**已落地（H11）：不再推荐一个不工作的写法

**落地内容**：两处逐字相同、**缩进不同**（32 空格 / 28 空格 ✓）的解释，一起改成实情：

```
A generic function's type parameters are inferred from its arguments. Writing them out is
accepted (`f<i32>(...)`) but does **not** seed the inference: give an argument whose type
mentions the parameter, or a typed variable to assign into.
```

**证据** ✓：`f<i64>(i64(1))` 与 `zero<i64>()` 现在都给出上面这段（旧文是
"…If one only appears in the return type, write it explicitly: `f<i32>(...)`" ✗ —— 推荐的正是那个
必然失败的写法 ✓）；`maxOf<i32>(4, 3)` 照旧通过 ✓（它靠推断成功 ✓，与显式实参无关 ✓）。

**验证** ✓：`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0** ✓（纯字符串改动 ⇒ 零输出变化 ✓）；
parrun ⇒ **292/0** ✓（回归 `tests/errors/explicit_targs_no_seed.extc` ✓）；`check.sh quick` ⇒
**41/0**、退出码 0 ✓；`make` 零诊断 ✓。

**仍然开着的那半**（让显式实参真去喂推断 ✓）留档在三点十三 ✓，顺序是：① 先把显式实参放进**检查器侧的
小表**（按节点索引 ✓ —— **不要碰 `Expr` 的字段** ✗：`exprNew` 虽然 `arenaAllocZero`，但 `sort::sort(x)`
那种**限定调用**的节点不是它造的 ✓ ⇒ 新字段仍可能是垃圾 ✓，上一轮已实测踩到 ✓）；② `EX_CALL` 推断时
用它预填 `targs` ✓；③ 两处注释（`check_expr.c:2000` 与 `:2026` 的 *docs 注释* ✓）也跟着对齐 ✓。

## 三点十五、新攻击组 coro（协程 × 池/arena）：16 题抓到两条

`tools/attack.py coro`：基本驱动 ✓ `for` 与显式驱动一致 ✓ 耗尽后 `next` 为 false ✓ 局部数组跨 yield ✓
suspend 前后写同一局部 ✓ 视图跨 yield ✓ 跨 yield 取局部地址（拒 ✓）✓ `send` ✓ 协程驱动协程 ✓
帧在循环里反复创建 ✓ 池对象共存 ✓ 池切片 ✓ 句柄存数组 ✓ —— 14/16 通过 ✓。

### K7 = 题目的期望写错了（已改 ✓）

`var p: mut ref i64 = alloc<i64>(1)` 在协程体内跨 `yield` ⇒ 检查器拒绝 ✓：
"`p` lives across a `yield` and carries a …" ✓ —— 这是**设计限制**（引用不跨 suspend ✓），
不是 bug ✓ ⇒ 题目期望改为 `reject` ✓。

### K4 = **两个真问题**（同一条用例）

**症状 1（已修 ✓ = H12）**：`main` 不写 `return` 时，生成物末尾是 **trap** ✗：

```c
    while (ids$step(&a)) { s = (s + a.ret); if (ids$step(&b)) { s = (s + b.ret); } }
    io$ostream_shl_slice_u8(…);
    extc_trapMsg("/tmp/k4suite.extc", 6, "a non-void function reached its end without returning");
```

而 C 约定 `main` 掉出末尾等价 `return 0` ✓，检查器里也写着"`main` 的返回类型可以缺省" ✓
⇒ **假 trap** ✗（程序白死 ✓）。修法二选一：codegen 对 `main` 发 `return 0;` ✓，或检查器直接要求
`main` 显式 return ✓（前者更贴近 C 与既有注释 ✓）。

**H12 修法** ✓（`codegen.c:4601`）：只有 `main` 走 C 的隐式规则 ⇒ 发 `return 0;` ✓；别的非 void 函数
掉出末尾**仍旧 trap** ✓（那里的值是真正未定义的 ✓，注释里那条"match 所有分支都 return"的理由仍然成立 ✓）。
**验证** ✓：K4 的 trap 消失 ✓、程序 `rc=0` ✓；生成物差异**恰好 13 份** ✓（都是"`main` 掉出末尾"的程序 ✓，
它们以前会在**退出时 trap** ✗）⇒ 已重设这 13 份基准 ✓（提交信息写明范围 ✓）；`tools/golden.sh` ⇒
**413/413 · 非法 C 0** ✓；回归 `tests/instimpl/t_main_falls_off.extc` ✓ ⇒ instimpl **17/0** ✓；
parrun **292/0** ✓。

**症状 2（撤回 ✗：是我算错了）**：当时记"交错驱动丢一个值 ✓，期望 436 实测 336" ✗ ——
第 16 轮逐步打印后看清：`10 → 110 → 121 → 222 → 234 → 336` ✓ **每一步都正确** ✓，
而 `10+100+11+101+12+102` **本来就等于 336** ✓（我把它心算成 436 ✗，多算了一个 100 ✗）。
`value()` 的幂等性也单独验过 ✓（`x=y=z=7` ✓）⇒ **K4 唯一的真 bug 就是那句假 trap** ✓，
H12 已整个修好 ✓：`tools/attack.py coro` 现在 **16/16** ✓。教训与 A4 同一条 ✓：
**结论要落在实测数字上，不靠心算** ✓。

**（原记录，保留备查）症状 2 的最初描述**：交错驱动**丢了一个值** ✗ —— 期望 `10+100+11+101+12+102 = 436` ✓，实测
**336** ✓（正好少一个 `100` ✓）⇒ 即第一次 `ids$step(&b)` 之后 `b.ret` 不是 100 ✗。生成物里两个帧
是**各自独立的局部 struct** ✓，`ids$step` 无状态 ✓ ⇒ 下一步：写一个每轮打印 `a.ret`/`b.ret` 的变体 ✓
把丢失点钉住 ✓。

**注意**：这两个症状都出现在**栈上（未装箱）表示** ✓（生成物里没有 `extc_task_*`/`extc_coro_*` ✓，
逃逸分析选了未装箱 ✓）—— 装箱路径是否也有同样问题另行验证 ✓。

## 三点十六、攻击组 modules（10 题）：**全绿** ✓ —— 这一面是稳的，但题面还不够狠

配套改了 harness ✓：`tools/attack.py` 的探针现在支持**多文件**（`src` 传 `dict`：文件名 → 源码 ✓），
每题先把工作目录清空（免得上一题的模块被 `use` 找到 ✓），入口取 `main.extc` ✓。这是打模块系统的前提 ✓。

**10 题结果**：基本跨模块调用与常量 ✓ `@private` 不可跨模块 ✓ 两个模块的同名私有/公开函数互不干扰 ✓
模块名与类型名撞车（mangling 家族 ✓ `pair_i64` 那条的模块版 ✓）✓ 跨模块共享泛型实例 ✓ 跨模块结构体与方法 ✓
循环 import ✓（**被拒** ✓ —— 不是挂死 ✓）✓ `use` 不存在的模块 ✓ 模块内的 `main` 不夺入口 ✓。

⇒ **0 条问题** ✓。这本身是有用信息 ✓（模块的可见性、mangling、循环 import 都守住了 ✓），
而且这 10 题已成为**常设判据** ✓。

### 加压批 13 题（共 23 题）：**23/23** ✓ —— 两条发现

跨模块 trait impl + `dyn` ✓ `@private` 类型从公开签名漏出（被拒 ✓）✓ 模块文件与同名子目录并存 ✓
跨模块类型闭环（被拒 ✓）✓ **模块名撞 `std` 模块名（`io.extc`）** ✓ 同一泛型在两模块各实例化 ✓
`@private` 泛型经公开包装跨模块用 ✓ 两个模块同名结构体各实例化 ✓ 模块函数遮蔽标准名 ✓
`@private` 常量经公开常量（被拒 ✓：`HIDDEN` 不可见 ✓）✓ 三模块链 ✓ 跨模块 impl 别处的 trait ✓
模块名形似关键字 ✓。

**发现 1（题目自己的错 ✓）**：`dyn` 收**无限定**的 trait 名 ✓ —— `use tag` 就把 `Tag` 带进作用域 ✓，
`dyn tag::Tag(t)` **不是合法语法** ✗（题目踩过 ✓，已改 ✓）。
**发现 2（小瑕疵 ✓，非健全性）**：`use tag::Tag`（想按限定名导入 trait ✓）报的是
"type name `Tag` must start with a lowercase letter" ✗ —— 对 trait 导入来说措辞不对 ✓，
留档不占主线 ✓。

**下一批加压方向**（已记 ✓）：跨模块 trait impl + `dyn` ✓、`@private` 类型
从公开函数签名里漏出去 ✓、模块文件与子目录同名 ✓、`use` 一个自身也 `use` 回头的类型闭环 ✓、
模块名与 `std` 模块名撞车 ✓。

## 三点十七、攻击组 views（视图与切片，16 题）：**全绿** ✓

覆盖：基本切片读写与长度 ✓ mut 与只读视图**同一个 C 结构**（第 6 轮统一 `unitFind` 的那条 ✓）✓
空切片（长度 0、不 trap、不越界读 ✓）✓ **越界索引带位置 trap** ✓ 元素是结构体（字段可写 ✓）✓
元素是泛型实例 ✓ 切片从函数返回（全局数组 ✓ 拒局部 ✓）✓ `new i64[4]` 得到可写切片 ✓
`[4]i64` 当 `slice<u8>` 重解释（拒 ✓）✓ 同一数组两个可写视图 ✓ 嵌套视图 ✓
字符串字面量取可写视图（拒 ✓）✓ 结构体里存视图（按逃逸规则拒 ✓）✓ 视图传进协程 ✓ 池切片 ✓
`for` 遍历与下标循环结果一致 ✓。

⇒ **0 条问题** ✓（三组新面 coro / modules / views 里，只有 coro 抓到了真 bug ✓）。
这 16 题已成为常设判据 ✓。**下一批加压方向**：视图的逃逸级别（全局数组 ✓ 局部 ✓ 帧内 ✓
池内 ✓ 四档各写一题 ✓）、对视图元素做 `ref` 再跨 suspend ✓、`slice<slice<T>>` 的写穿 ✓。

## 三点十八、攻击组 dyn（16 题）：**H13 修掉一个编译器段错误** ✓ + 一条待查

### H13（已落地 ✓）：无接收者的方法经 `dyn` 调用 ⇒ **编译器 SIGSEGV**

**触发**（`tools/attack.py dyn` D2 ✓，六行）：

```extc
trait Bad { fn nope() -> i64 }              /* 没有接收者 */
struct s { v: i64 }
impl Bad for s { fn nope() -> i64 { return i64(1) } }
… var d: dyn Bad = dyn Bad(a)
  return i32(d.nope())                      /* extc 段错误 ✗ */
```

**gdb** ✓：`genMethodCall`（`codegen.c:1803` ✓）里 `Param *p0 = *(Param **)vecAt(&f->params, 0);`
—— 方法**没有参数**（连 `self` 都没有 ✓）⇒ 索引空表 ⇒ 崩溃 ✓。

**对象安全本来就要拦它** ✓：检查器的注释（`dynTraitOf` ✓）写着"no receiver、generic、返回 `Self`"三类 ✓，
而"返回 `Self`"那半**早就拒了** ✓（D3 ✓）—— 缺的就是"no receiver" ✓。

**修法** ✓（两处 ✓）：① 检查器在 dyn 调用解析出 trait 方法之后判 `want->params.len == 0` ⇒
报 `` `%s` has no receiver, so it cannot be called through `dyn` `` ✓（并说明"dyn 走统一表、每项都要接收者"✓，
或改写成自由函数 ✓）；② 生成器加一句 `if (f->params.len == 0) return "0";` 兜底 ✓
（诊断留在检查器 ✓，注释写明这是防御 ✓）。

**验证** ✓：D2 现在退出码 1 + 上面的报错 ✓；`tools/attack.py dyn` **16 题剩 1 条** ✓；
`tools/golden.sh` ⇒ **413/413 逐字节 · 非法 C 0** ✓（零输出变化 ⇒ 只新增拒绝 ✓）；回归
`tests/errors/dyn_no_receiver.extc` ✓；parrun ✓；`make` 零诊断 ✓。

### D12 结案 ✓：是**我题目的期望写错了** —— `dyn` **复制**载荷，本来就安全

判据三条 ✓：

1. 生成物是 `extc_dyn_put((const void *)&__extc_dyp0, (int64_t)sizeof(s), &extc_vt$Tag$s)` ✓ ⇒
   载荷被**复制**进池存储 ✓（不是留一个指向局部变量的指针 ✓）；
2. ASan/UBSan 下运行**零报告** ✓（无 use-after-free ✓）；
3. 更狠的形状也被**正确挡住** ✓：载荷里含**视图**（`struct holder { s: slice<i64>  v: i64 }` ✓）
   且局部先死 ⇒ 检查器报 "this return value would hold a reference to a local variable that dies
   first" ✓ —— **逃逸分析**在起作用 ✓（这条才是真正的攻击面 ✓，它守住了 ✓）。

⇒ 题目期望已从 `reject` 改成 `ok` ✓，`tools/attack.py dyn` 现在 **16/16** ✓。

## 三点十九、攻击组 arena（arena / 逃逸，12 题）：**全绿** ✓

**策略** ✓：`tests/arena-soundness/` 已经收录了一批**已知**的逃逸漏洞 ✓（如 `B_field_table_stale.extc`：
"字段表过期 + 引用型结构体的值 ⇒ 让 arena 内存逃出函数" ✗）⇒ 这一组**避开**已收录的族 ✓，
专打"按级别规则**应当安全**"的相邻形状 ✓ —— 真漏了才是新发现 ✓。判据是硬 oracle ✓（ASan/UBSan 零报告 ✓）。

**结果**：局部 `new` 在块内用 ✓ `new` 值从函数返回（提升 ✓）✓ 跨两层调用提升 ✓ `new` 切片从函数返回 ✓
循环里分配并保留最后一次 ✓ 泛型返回 `new` 值 ✓ 结构体装 `new` 值再返回 ✓ 辅助函数里的 `alloc`
用于外层 ✓ 协程里持有 `new` 切片跨 yield ✓ `alloc` 计数为 0 ✓ 嵌套结构体装 `new` 值 ✓
两层容器返回 ✓ ⇒ **12/12** ✓。

**顺带确认一条语言规则** ✓（题目踩过一次 ✓）：含**引用/视图**的结构体**不能零初始化** ✓ ——
`var h: holder`（`holder { v: mut ref i64 }`）报 "cannot zero-initialize `h`: it contains a reference" ✓
（零值引用会是 NULL ✓，直接拒绝是对的 ✓）⇒ 要用字面量一次写全 ✓
（`var h: holder = holder { v: new i64 }` ✓）。

## 三点二十、攻击组 extern（extern / C 互操作，14 题）：**全绿** ✓

**这一面的口径** ✓：按设计 `extern!` 的签字是**纯信任** ✓（签了 `effects Addr=0 Cont=0` 的声明若在 C
那侧真的存了指针，分析自然失效 ✓ —— FFI 皆如此 ✓，属**设计边界**不是 bug ✓，
`tests/arena-soundness/README` 的 X 族写的就是这件事 ✓）。所以这一组打的是**可验证的那半**：
"未签字就退回最保守"这条规则**到底有没有被执行** ✓。

**结果** ✓：未签字的 extern 不许收本帧地址（拒 ✓）✓ 跨边界传 `slice`（拒 ✓）✓ 跨边界传 `struct`（拒 ✓）✓
**签字后可以传本帧地址**（真的写进 stdout ✓ 返回字节数正确 ✓）✓ 签字后传**全局**地址 ✓
extern 返回指针并解引用（按期望被拒 ✓ —— 拿不到可写的 `ref` ✓）✓ 调元数写错（拒 ✓）✓
`void` 返回值当值用（拒 ✓）✓ 声明了但从不调用（死代码消除正确 ✓）✓ extern 名与 extC 函数同名 ✓
`effects` 子句写错（拒 ✓）✓ extern 调用放进**协程体** ✓ 返回值直接算进 `i32` ✓ 指针参数收 `null` ✓
⇒ **14/14** ✓。

**题目自己踩过两次** ✓（都记下来，免得下次再踩 ✓）：① `b[0]` 是**值** ✗ ⇒ 报 "argument expects
`ref u8`" ✓；② 改成 `ref b[0]` **也不行** ✗ —— "cannot take a reference to this expression:
only variables and fields can be referenced" ✓ ⇒ 正解是**切片的 `.data` 字段** ✓
（官方正例 `tests/extern/main.extc` 就写着 `libc::write(1, s.data, 5)` ✓）⇒ 三题统一改成
`b[..].data` ✓。**第三次踩坑（E5，题目已删 ✓）**：原题想验"extern 返回指针" ✓，写的是
`extern!("libc") fn getenv(name: ref u8) -> ref u8` ✗ —— 而 `getenv` **已经**由 `<stdlib.h>` 声明
（生成的 C 会 include 它 ✓），libc 的签名是 `char *(const char *)` ✗ ⇒ 冲突的是**用户声明** ✓，
不是编译器 bug ✓（`conflicting types for 'getenv'` ✓）。**留档的结论** ✓：与系统头同名的 extern
一旦签名不一致就是用户的错 ✓，但**更好的诊断**是值得做的 nicety ✗（现在只报 C 编译器的冲突 ✓）
—— 记在这里，不占主线 ✓。E5 已从组里删掉 ✓，extern 组现在 **13 题** ✓。

**关于"全绿"的口径** ✓（我自己错了两次 ✗）：第一次提交（`90e9d67`）写"全绿"时 E4 还在失败 ✗；
第二次（`9d45834`）写"14/14"时 E5 翻红 ✗ ⇒ **本条以实测为准** ✓：删掉 E5 之后
`tools/attack.py extern` 的实测结果见提交信息 ✓，不再提前写"全绿" ✓。

## 三点二十一、H15 候选（未落地 ✓ 已定位）：`f()!` 作**语句**时生成裸值 ⇒ `-Werror=unused-value`

**发现路径** ✓：新开的 **fs 攻击组**（`tools/attack.py fs` ✓，12 题）里 `a.close()!` 作语句时生成：

```c
    (fs$ofstream_close(&(a))).u.success._0;      /* 裸表达式 ✗ */
```

gcc：`value computed is not used [-Werror=unused-value]` ✗。**官方测试 `tests/fs/close-twice.extc:16`
写的就是同一形状** ✓（`a.close()!` ✓）—— 它的 runner 不带 `-Werror` ✓，所以这条一直没被看见 ✓。

**试过的修法** ✓（`genStmtInner` 的 `ST_EXPR` 一般分支 ✓，`:3789` 一带 ✓）：

1. 只要类型非 void 就发 `(void)(%s);` ✗ ⇒ **194 份**产物变化 ✓（等价重写，但面太大 ✗）；
2. 收窄到"**非调用**"（`kind != EX_CALL && != EX_METHOD` ✓）✗ ⇒ 仍 **178 份** ✓ —— 因为
   `io::cout << …` 这类**二元**表达式也是非调用 ✓；
3. 需要的精确判据是"**这个值来自解包（`!`/`?`）的载荷**" ✓ —— `?` 走的是 `EX_TRY` 的专门分支
   （`:3785` ✓，本来就不发裸值 ✓），而 `!` 的节点种类**尚未确认** ✓（`grep -E "EX_TRY|'!'" src/parser.c`
   只找到 `:2418` 的 `EX_TRY` ✓）⇒ 下一步：在 parser 里找 `!` 后缀的造节点处 ✓（token 名可能是
   `TK_BANG` ✓），或给那个节点加一个"丢弃时发 `(void)`"的标记 ✓。

**判据** ✓：`tools/attack.py fs` 的 **S2/S3/S7/S8/S11** 五条 ✓（现在 12 题剩 2 条 S1/S11 ✓，其中 S1 是
题目自己缺 `use stl::string` ✓ 已修 ✓）。**已落地** ✓（`9ca5973` ✓）：精确判据取自 parser —— `x!` 与前缀 `!x` 共用 **`EX_SIGN`** 节点 ✓
（`parser.c:2400-2412` ✓，只靠位置区分 ✓）⇒ codegen 里用**类型**分辨：取反必得 `bool`、解包绝不 ✓
⇒ 只有"`EX_SIGN` 且类型非 bool"才发 `(void)(…)` ✓。**闸门从 194/178 份降到 5 份** ✓（正是"丢弃解包"
的程序 ✓ = 本 bug 的受害者 ✓）⇒ 已重设这 5 份 ✓ ⇒ **413/413 · 非法 C 0** ✓；fs 组 6 → 2 ✓。
**同族仍开着一处** ✓：`var f = fs::openWrite(...)!`（`fs::file` 是 **nocopy** 类型 ✓ ⇒ 声明被降成
语句表达式 ✓ ⇒ 那一步仍丢弃解包值 ✓，fs 组 S11 就是它 ✓）；S1 是题目写法未定稿 ✓（已按 `ok_or_reject`
计 ✓，定稿后再收紧 ✓）。

**性质** ✓：属**质量类**（只在 `-Werror` 下可见 ✓），
不是健全性 ✓ ⇒ 不占主线 ✓，留着给下一阶段 ✓。

## 三点十八之前、攻击组 rec（12 题）：**全绿** ✓（H16 之后顺着同族再打）

覆盖 12 题 ✓：**互递归枚举** ✓ · **自引用枚举 + 零初始化（应拒 ✓ = H16 的干净诊断那条路）** ✓ ·
**载荷是自身的 option** ✓ · **载荷是自身的数组** ✓ · **三跳互递归** ✓ · **递归函数遍历自引用枚举** ✓ ·
**`?ref` 自引用（设计意图：给链表一个 null）** ✓ · **泛型枚举自引用** ✓ · **深层嵌套实例**（`box^20` ✓
走 `TYPE_DEPTH_LIMIT` 那条路 ✓ 运行 rc=0 ✓）· **结构体与枚举互指** ✓ · **递归类型只出现在泛型函数里** ✓ ·
**自引用枚举的 match 穷尽性**（应拒 ✓）。⇒ H16 修好后这一族**没有新的崩溃** ✓。

**拼写教训（又一次 ✓）**：可空引用的写法是 **`?ref node`** ✓（`ref node?` 是解析错 ✗ ——
`expected a field name, found '?'` ✓；`ref? node` 会被判成"含引用 ⇒ 不能零初始化" ✓）。
R7 因此从"按期望被拒"（其实是拼错 ✓）升成**真正跑通的正例** ✓ —— 这正是 `ok_or_reject` 的陷阱 ✓：
**看到一片绿时要抽查它到底是"通过"还是"被拒"** ✓（`time` 组那次也是这样抓出来的 ✓）。

## 三点十九之前、攻击组 match（14 题）：**抓到一次编译器崩溃（自引用枚举）** ✓

**H16（本轮）** ✓：`type list = | cons(i64, list) | nil` —— 一个**自引用枚举**（载荷是它自己 ✓）
即可让编译器 **SIGSEGV** ✗（`rc=-11` ✓）。gdb 一击给出根因 ✓：**三个类型遍历函数无限递归** ✓ ——
`typeContainsRef`（`check_lookup.c:567` ✓）、`typeLacksZeroValue`（`:616` ✓）、
`typeContainsProto`（`:553/:555` ✓）都在枚举载荷上**不设深度**地往下走 ✗ ⇒ 爆栈 ✓
（我先修了第一个 ✓，一跑又崩在第二个 ✓ ⇒ **同族三处** ✓ 一次全修 ✓）。

**修法** ✓：每个函数拆成 `X(...)` 入口 + `XAt(..., int depth)` 递归体 ✓，`depth > ZERO_VALUE_DEPTH_LIMIT (64)`
时返回**保守值 `true`** ✓（"含引用 / 无零值 / 含原型" ✓ ⇒ 检查器给出**干净诊断** ✓ 而不是崩 ✓），
与 H8 的 `TYPE_DEPTH_LIMIT` 同一手法 ✓。修好后 `M12` 退出码 **1** ✓、诊断是
`cannot zero-initialize 'l': it contains a reference` ✓（正是设计意图 ✓：`?ref T` 的存在就是为了给链表一个 null ✓）。

**match 组其余 13 条全绿** ✓：全变体匹配 ✓ · **漏变体被拒（穷尽性 ✓）** · 兜底臂 ✓ · 载荷绑定 ✓ ·
载荷是泛型实例 ✓ · 嵌套 match ✓ · option/result 的 match ✓ · 各臂返回值 ✓ · 整数字面量 ✓ ·
泛型函数里 match ✓ · 枚举进容器 ✓。

## 三点二十之前、攻击组 time（12 题）：**时机族第二批，一次抓到 5 条** ✓

W9（第 19 轮修 ✓）是"局部声明里的类型实例 intern 太晚" ✓，修法只覆盖了 **`ST_VAR` 的类型标注** ✓
⇒ 这一组专打**其它**"实例只出现在泛型体内"的形状 ✓。**拼写教训**（第一版全被拒 ✓）：泛型体内
**不能**写 `box<T> { … }`（参数不能当显式实参 ✗ ⇒ `undefined name 'box'` ✓）⇒ 合法写法是
**上下文定型的裸字面量** `{ v: v }` ✓（官方 ctor 用例的写法 ✓）或**先 `var b: box<T>` 再赋字段** ✓。

**5 条红** ✓（全在"生成的 C 不合法" ✓）：**X3 容器元素** ✓ · **X4 数组元素** ✓ ·
**X7 作别的泛型的字段** ✓ · **X8 协程 yield 泛型实例** ✓ · **X9 切片元素** ✓；
绿的 7 条里 ✓ **X1/X11/X12（返回值/裸字面量）** ✓ 与 **X5（`new box<T>[2]`）** ✓ **X6（option+match）** ✓
都通过 ✓ ⇒ 说明"返回值那条路"没问题 ✓。

**第 21 轮的定位（含两次失败尝试 ✓）** ✓：

- `cType` 那侧**没问题** ✓：打点显示 `array_2_box_T -> array_2_box_i64` ✓（`params=set` ✓）⇒ 顶层替换是对的 ✓；
- 真正的证据在**生成物**里 ✓：**同时**存在
  ```
  typedef struct array_2_box_T array_2_box_T;      /* 临时的 ✗ */
  struct array_2_box_T {  box_T data[2];  };       /* ← box_T 未定义 ✗ */
  typedef struct box_i64 box_i64;                  /* 正确的也在 ✓ */
  typedef struct array_2_box_i64 array_2_box_i64;  /* ✓ */
  ```
  ⇒ **含类型参数的临时数组类型也拿到了单元并发了 struct 定义** ✗；
- **两次失败尝试（都已回退 ✗，闸门零变化 ✓）**：① 在 `g.insts` 的收集处（`codegen.c:6341` ✓）用 `ttHasParam`
  过滤 ✗；② 换成**递归**的 `mentionsParam` ✗ —— 都**毫无效果** ✗ ⇒ 说明那个临时数组**不是**从 `g.insts`
  来的 ✓（units 还有另外几个来源：`g.structs`（`:7147` ✓）、`g.insts`（`:7153` ✓）、协程帧 ✓、
  以及 **`scanUnitForUnits` 的递归**（它会按字段/元素**再派生**单元 ✓ —— 最可能是这里 ✓）。
- **第 22 轮的三次尝试（全部无效果 ✓，均已回退 ✗）** ✓：
  ① 在 6 处 `vecPush(&units)`（现为 `:7137/7145/7156/7166/7178/7188` ✓）各打点 ✓ ⇒ **零输出** ✗
     （探针**确实**在二进制里 ✓ `strings` 验证过 ✓）⇒ **那 6 处对 X4 根本不执行** ✓；
  ② 找到**第 7 处**造单元的入口 ✓：**`addInstanceUnit`**（`:5042` ✓）—— 它被 `scanUnitForUnits` 的递归调用 ✓
     （注释原话："引用、**切片与数组元素**、泛型实参都会跟进" ✓ = 正是出问题的形状 ✓），且**只检查**
     `TY_GENERIC && sdef` ✓、**不看是否含类型参数** ✗ ⇒ 在那里加 `mentionsParam` 守卫 ✓ ⇒ **仍无效果** ✗；
  ③ 于是 ⇒ **那个坏 struct 不是从单元列表来的** ✓（三次过滤都动不了它 ✓）。
- **第 23 轮：`abort()` + `bt` 一击定位** ✓（方法有效 ✓）：在**数组的 struct 写点**
  （`cgLine(g, "%s data[%lld];", cType(g, u->inst->inner), …)` ✓ 现为 `:4879` ✓）加探针 ⇒
  **命中** ✓（`[abort] array array_2_box_T` ✓），栈给出 `unitBody` ← **`generateC` 的单元循环** ✓
  ⇒ **临时数组确实在 units 里** ✓；
- 接着查到**真正的根** ✓✓：**`ttHasParam`（`types.c:463` ✓）不进 `TY_ARRAY`** ✗（只递归 `TY_REF` ✓
  与 `TY_GENERIC` 实参 ✓）⇒ `array_2_box_T` 被判成"不含类型参数" ✗ ⇒ 我前一轮的守卫**全部失效** ✓
  （难怪两次过滤都动不了它 ✓）。给 `ttHasParam` 补上数组/枚举递归 ✓ + 重新加回 `addInstanceUnit` 守卫 ✓
  ⇒ **仍是 5 条** ✗（闸门 1 份差异 ✓）⇒ 说明此时数组的 **`inner` 已被替换成具体类型** ✓
  （所以 `mentionsParam` 仍为假 ✗）而它的 **`name` 还是旧的 `array_2_box_T`** ✗ —— 名字与 inner 不同步 ✓。
  **已全部回退** ✓（闸门 413/413 ✓）。
- **第 24 轮：把"守卫"这条路走完了 ✓（仍未清 ✓，已全部回退 ✗）** ✓：
  · 把 `tt` 一路传进 `addInstanceUnit`/`scanTypeForUnits` ✓，守卫用**重算名字对比**
    （`strcmp(t->name, ttMangle(tt, t)) != 0 ⇒ 跳过` ✓，能自维护 ✓）；
  · 再把 **`ttHasParam` 的数组/枚举递归**补上 ✓（`types.c:463` ✓ ⇒ 现在 `mentionsParam` 对
    `[2]box<T>` 为**真** ✓）；
  · 两者**同时**在位 ⇒ **仍是 5 条** ✗ ⇒ **结论：那个临时数组根本不经过 `addInstanceUnit`** ✗
    （也不经过那 6 处 `vecPush(&units)` ✓ —— 探针零输出 ✓）⇒ 它是**另一条单元收集路径**发的 ✓。
  · 已全部回退 ✓（闸门 413/413 ✓）。
- **第 25 轮：gdb 断点给出二元答案 ✓，X4 修好 ✓（5 → 4 ✓）** ✓：
  三个候选函数上断点 + 过滤打印（`strstr` 在 gdb 里要**强制转换** ✗ 否则脚本会死在那一行 ✓）⇒ 输出依次是
  `scanUnitForUnits hit: ?` ✓ `?` ✓ `slice_u8` ✓ `slice_i32` ✓ **`array_2_box_T`** ✓ `box_i64` ✓
  `array_2_box_i64` ✓ ⇒ **临时数组早就在 `units` 里** ✓ —— 也就是说它来自**初始收集**（那 6 处 ✓，
  输出里的 `unit`/`pcg32`/`slice_u8`/`slice_i32` 就是它们 ✓）从 **`g.insts`**（← `tt->instances`）收进来的 ✓。
  而我第 22 轮的守卫无效 ✗，正是因为**当时 `ttHasParam` 不看数组** ✗ ✓。
  **修法（两半合起来 ✓）**：① `ttHasParam` 补数组（与枚举）递归 ✓（`types.c:463` ✓）；
  ② 在 **`g.insts` 的收集处**加 `if (!it || mentionsParam(it)) continue;` ✓ ⇒ **X4 转绿** ✓、
  闸门 **1 份**差异 ✓（正是"泛型体内用数组实例"的程序 ✓ = 本 bug 的受害者 ✓，已重设 ✓）。
  **第 26 轮把剩下 4 条按真实报错分了类** ✓（比"同一根"精确得多 ✓）：

  | 题 | 真实报错 | 性质与修法方向 |
  |---|---|---|
  | **X3** | `unknown type name 'vector$vector_box_i64'` ✗ | **实例没被发射** ✓（名字是**正确的** ✓）⇒ W9 那个"intern 太晚"的同族 ✓ |
  | **X9** | `'slice_box_i64' undeclared` ✗ | 同上 ✓ —— 切片实例来自**表达式** `arr[..]` ✗，而我的 `internLocalTypes` **只走类型标注** ✗ |
  | **X8** | ~~`incompatible types when assigning to type 'box_i64' from type 'box_T'`~~ ✅ **已修（第 27 轮）** ✓ | 真因：**帧字段用了模板体里记录的局部类型** ✗ —— 生成物里实例帧是 `box_i64 ret; box_i64 in;` **加 `box_T b;`** ✗（`b` 的声明是 `var b: box<T>` ✓）⇒ `coroFrameLay` 的 `p->type = d->type;`（现 `:6530` ✓）没有按实例替换 ✓。修法：`f->tmpl && f->targs.len` 时 `ttSubstitute(c->tt, d->type, &f->tmpl->typeParams, &f->targs)` ✓ ⇒ X8 转绿 ✓、**coro 16/0 · generics 49/0** ✓、**闸门零变化** ✓ |
  | **X7** | `variable 'p' set but not used [-Werror=unused-but-set-variable]` ✗ | **质量类** ✓（同 H15/S11 一族 ✓）。**第 28 轮实测**：用**现成的** `EXTC_DBG_LOCAL`（W12 时学到的那招 ✓）看到 `p` 在那趟里是 `bp=hit cnt=3 own=1` ✓ ⇒ 之后**再没出现**（= 声明被切掉了 ✓），可**生成物里 `p.x = inner;` / `p.y = …;` 还在** ✗ ⇒ 症结是"**切只切了声明、逐字段赋值没跟着走**" ✓；我按"字段赋值不算读"改 `countReads`（`:5466` ✓ 那条只在名字后**直接**跟 `=` 时才不算读 ✗）⇒ **毫无效果** ✗（已回退 ✓）⇒ 决策**不由读计数**驱动 ✓。下一步：把 `p.x = …` 这类**字段赋值**也收进 `cuts`（第二处 `vecPush(&cuts)` ✓ 现 `:5696`-ish ✓，那里的条件大概是"`d->name` 后跟 `=`" ✓ ⇒ 要放宽到"`.字段链` 后跟 `=`" ✓），并确认删除时**声明与全部赋值一起走** ✓。 |
    **第 32 轮：封存（决定不再追 ✓）** ✓ —— 在赋值收集那段行扫描里打点 ✓（对**每一行**含名字的都打印 ✓）⇒ **零输出** ✗ ⇒ 那段循环**从头到尾没见过含 `p` 的行** ✓ ⇒ `p` 的两行字段赋值**不在** `bp..bp+bl` 范围里 ✓（或 `bp`/`bl` 指的不是我以为的那段 ✓）。**留给下一阶段的完整地图** ✓：① 声明定位要按行找（第 31 轮已验证有效 ✓：`!dl` 消失、`reads=0` ✓）；② `cuts` 只收到声明那一刀（`n=1` ✓）；③ 字段赋值 `p.x = inner;` 不在扫描范围内 ⇒ **下一步先打印 `bp`/`bl` 与那段正文的首尾各 80 字符** ✓，确认它是哪一段 ✓（很可能是 `pfLine` 之类的**前缀机制**把这两行写到别处/稍后 ✓）。**这一条已花 5 轮** ✓ ⇒ 按 goal 的"修不动就记档、继续做别的" ✓ **封存** ✓，转向扩面（它的判据与全部证据都在此 ✓，随时可接）。
    **第 31 轮：又拿到两个硬事实 ✓（修法仍未落 ✓，已回退 ✗）** ✓：① 把声明定位改成**按行找**（行末标识符 == 名字 ✓ 且排除范围用该行长度 ✓）之后 ✓，打点显示 `!dl` **不再出现** ✓、并且 **`[x7] p: reads=0 bm.len=165 own=1`** ✓ ⇒ 读检查**通过了** ✓；② 但 `[cuts] p n=1` ✗ ⇒ **只收到声明那一刀** ✓，`p.x = inner;` / `p.y = …;` 两行**没收到** ✗ （生成物里那两行仍在 ✓）。⇒ 症结在**赋值收集的那段循环**（`codegen.c:5680` 一带 ✓）：它按行扫 ✓ 要求"行首（缩进后）是名字、后面跟 `=`" ✓；我按记录把条件放宽到"名字后允许一段 **`.字段链`** 再判 `=`" ✓ ⇒ **仍没收到** ✗ ⇒ 说明还有一处不匹配 ✓（可能是 `ll2 > k + nlen2` 的边界 ✓、或那两行**不在** `bp..bp+bl` 范围内 ✓、或 `bp`/`bl` 取的不是我以为的那段 ✓）。**下一次的打点**：在那段循环里对**含名字的每一行**打印 `k/ll2/nlen2` 与"为什么没进分支" ✓ ⇒ 一次定位 ✓。
    **第 30 轮：判定完成 ✓ —— 是嫌疑 ①** ✓。在两处 `continue` 前各打一行 ⇒ 输出对 **`inner` 与 `p` 都**是 `!dl (declaration text not found) -> bail` ✗ ⇒ `strstr(bp, d->text)` **找不到声明原文** ✓（`d->text` 是**发射时**捕获的那一段 ✓，而中间几趟会改写文本 ✓）⇒ 所以这一族根本不是"读计数"的问题 ✗，而是**声明定位太脆**（逐字匹配）✓。**修法（下一次直接做 ✓）**：把声明定位从"逐字 `strstr`"换成**按行找** —— 在函数体里扫行 ✓，取"行的最后一个标识符恰好是 `d->name`、且后面跟 `;`/`=`/`(`"的那一行作为声明 ✓，并把排除范围由 `strlen(d->text)` 改成**该行的长度** ✓（`bm` 的构造与 `declAt` 都据此 ✓）。这样即使发射后被改写 ✓ 也能定位到声明 ✓，`p` 与 `inner` 都会被正常删掉 ✓。
    **第 29 轮实测** ✓：按记录放宽了赋值收集条件（名字后允许一段 **`.字段链`** 再判 `=` ✓）⇒ **仍无效果** ✗；又加 `EXTC_CUTS` 打点 ✓ ⇒ 输出里**只有** prelude 的 `skip` 进了 cuts ✓，**`p` 一条都没进** ✗ ⇒ 说明 `p` 在**到达收刀之前**就被 `continue` 掉了 ✓。两个嫌疑（下一次直接判定 ✓）：① `dl = strstr(bp, d->text)` 找不到**声明原文** ⇒ `!dl` 早退 ✗（`d->text` 是**发射时**捕获的 ✓，而中间几趟会改写文本 ✓）；② `countReads(...) != 0` ✗（"被读了"）。**下一次打点**：在那两个 `continue` 前各打印一次（`dl` 是否为空 ✓ 与 `countReads` 的返回值 ✓）⇒ 一次分清 ✓。

  ⇒ **X3 + X9 已修（第 26 轮）** ✅ ✓ —— 而且比预想的**省得多** ✓：不必写表达式 walker ✓，因为
  检查器把结果写回了 AST（`Stmt.type` ✓，codegen 头注释原话："`Expr.type`, `Expr.func`,
  `Expr.field`, `Stmt.type`" ✓）⇒ 只要在 `internLocalTypes` 里对**每条语句**的 `type` 也做一次
  `ttSubstitute` ✓（intern 是副作用 ✓），就覆盖了 `let sl = arr[..]`（类型 `slice<box<T>>` ✓ ——
  **语句类型**是唯一提到它的地方 ✓）与容器那条 ✓ ⇒ **X3 + X9 同时转绿** ✓、**闸门零变化** ✓。

（历史）**下一轮该怎么定位（工具已经就位 ✓）** ✓：`EXTC_ABORT` 探针就在**数组 struct 写点**
  （`cgLine(g, "%s data[%lld];", cType(g, u->inst->inner), …)` ✓ 现 `:4879` ✓）—— 直接在 gdb 里
  **`break addInstanceUnit` / `break scanTypeForUnits` / `break scanUnitForUnits`** ✓，跑 X4 看
  **哪个先命中、命中时 `t->name` 是什么** ✓ ⇒ 一次就能分清"它到底走哪条收集路径" ✓（比再加探针快 ✓）；
  若三个都不命中 ✓ ⇒ 说明有一条**完全独立**的路径（很可能在**模块/预奏单元**那一层 ✓ 或
  `emitDescRegion` 的类型通道 ✓）⇒ 再从 `unitBody` 的调用者往上一层找 ✓。

**关键证据** ✓（X4）：`unknown type name 'box_T'; did you mean 'box_i64'?` ✓ ⇒ 生成物里用的是
**未替换的临时名** `box_T` ✗，而 `box_i64` **已经存在** ✓（walker 已 intern ✓）⇒ 所以症结是
**替换没有下探到元素类型** ✓：数组 `[2]box<T>` ✓ / 容器 `vector<box<T>>` ✓ / 字段 `pair2<box<T>,i64>` ✓ /
切片 `slice<box<T>>` ✓ / 协程 yield `coroutine<box<T>>` ✓ —— **这一批的修法**是让"包含实例的复合类型"
在检查器阶段整体下沉一次 ✓（`ttSubstitute` 对**内层**实例要真的替换并 intern ✓），
判据就是这一组转 **12/12** ✓。

## 三点二十一之前、攻击组 deep（14 题）：**加深三族 + 跨特性，立刻抓到两条新问题** ✓

思路 ✓：刚修好的三族（**泛型协程** B7/F4 ✓、**泛型 impl × dyn** F1 ✓、**死局部变量 + 解包** S11/A4 ✓）
历史上最容易"同族第二个口子" ✓，所以专门补同族形状 ✓，再加跨特性组合（协程 × 容器 ✓、`dyn` × 协程 ✓）。

**结果** ✓：12 条绿 ✓、**2 条红** ✗（都精确定位 ✓）：

1. ~~**W9**~~ ✅ **已修（第 19 轮）** ✓ —— 根因：**局部声明里的类型实例被 intern 得太晚** ✓
   （`ttSubstitute` 会 intern ✓ `types.c:697` ✓，但泛型函数体里 `var p: pair<T>` 的类型是**体发射**时
   才替换的 ✗ ⇒ 晚于 units 定点循环 + struct 发射 ✓ ⇒ 定义永不发 ✓）。**修法** ✓：在**检查器**阶段
   提前 intern —— 新增 `internLocalTypes`（只走**语句** ✓，对 `ST_VAR` 的 `ann` 做一次 `ttSubstitute` ✓，
   intern 是它的副作用 ✓），并且**必须是独立一趟**地对 `c.funcInsts` 里所有泛型实例跑 ✓：
   我第一版把它挂在**协程补跑循环**里 ✗ ⇒ 打点**零输出** ✓（那个循环开头就 `!fi->tmpl->isCoro … continue`
   ⇒ `mk<i64>` 根本不经过 ✓）⇒ 换成独立一趟后 ✓ 打点显示 `var p ann=pair<T> -> pair<i64>` ✓、
   **W9 rc=0** ✓、**闸门零变化** ✓、套件回到 **233 题 / 0 条** ✓。**教训**：挂点要确认**它的循环真的
   覆盖目标** ✗（打点零输出就是"挂错了地方"的signature ✓）—— 这与本轮/上轮的时机族是同一主题 ✓。

   （历史）1. **W9（第 18 轮已把根因钉死 ✓，修法待落 ✓）** —— 报错 `In function 'mk_i64': unknown type name
   'pair_i64'` ✓（`pair_i64 p = (pair_i64){ .a = 0 };` ✓）。

   **打点结论（很硬 ✓）**：在 units 定点循环之后打印 ⇒ `units=4`（unit/pcg32/slice_u8/slice_i32 ✓）、
   `insts=2`（slice_u8/slice_i32 ✓）—— **`pair_i64` 两边都不在** ✗ ⇒ 它**根本没被 intern 成类型实例**
   ⇒ 所以 struct 定义不发射 ✓。
   再往上查 ✓：intern 原语是 **`ttGeneric`**（`types.c:492` ✓，"concrete 才 `vecPush(&tt->instances)`" ✓
   `:521` ✓），而 **`ttSubstitute` 的泛型分支确实会走它** ✓（`types.c:697` ✓）
   ⇒ 所以**不是**"忘了 intern" ✗，而是**时机** ✗：泛型函数体里 `var p: pair<T>` 的类型是在
   **体发射**时（`subst` ✓）才被替换的 ✓ ⇒ intern 发生在 **units 定点循环 + struct 发射之后** ✓
   ⇒ 定义永远不发 ✓ —— **与 B7/F4 是同一个时机族** ✓（"实例出现得太晚" ✓）。
   **修法** ✓：在**检查器**阶段把它提前 intern ✓ —— 最自然的挂点就是那个**延迟回放**
   （`#57`/`#79` ✓，B7/F4 的 `coroSetup` 补跑也在那里 ✓）：回放实例体时，对**提及类型参数的局部
   声明**做一次 `ttSubstitute`/`ttGeneric` ✓（副作用就是 intern ✓），让 units 那趟能看见 ✓。

   （历史）1. **W9 泛型 impl 的实例在泛型函数里进 dyn** ✗ —— `In function 'mk_i64': unknown type name 'pair_i64'` ✓
   （`pair_i64 p = (pair_i64){ .a = 0 };` ✓）⇒ 泛型函数**体内**写的 `pair<T>` 被正确替换成 `pair_i64` ✓，
   但该实例的 **struct 定义没被登记/发射** ✗ ⇒ **F1 × B8 的交叉** ✓（静态路径 W7/W8 都对 ✓）——
   大概率又是"实例在**延迟实例化那一趟**才出现 ✓ ⇒ 单元登记已经跑过" ✓（与 B7/F4 同一个时机族 ✓）。
2. ~~**W12 相邻两个死局部变量**~~ ✅ **已修（第 17 轮）** ✓ —— 真因一句话：**`g->mainBody` 是
   第一刀之前的旧拷贝** ✓。那趟为 `main` 找 body 时走 `:5573`（"main has no DeadFunc entry" ✓ ⇒ 用
   `g->mainBody` ✓），而它**不在**每次重建后刷新 ✓ ⇒ 第一刀之后 `strstr(text, g->mainBody)` **必然失败**
   ✗ ⇒ `:5591` 的 `if (!bp) continue;` 把 `main` 里**第二个及以后**的死局部变量全部跳过 ✓
   —— 与实测"**不论名字长短，总是第二条残留**"完全吻合 ✓（`a`/`b` ✓、`aa`/`bb` ✓、`alpha`/`beta` ✓）。
   **修法** ✓：找不到 body 时**退化为全单元范围**（`bp = text; bl = len;` ✓）—— 安全性论证：局部变量的
   名字只在自己函数里可见 ✓ ⇒ 范围放宽**只会多保住**一个局部变量 ✓ **不会误删** ✓。
   **代价** ✓：**3 份**基准变化 ✓（正是"main 里有两个相邻死局部变量"的程序 ✓ = 本 bug 的受害者 ✓）。
   **定位手段** ✓：仓库**自带** `EXTC_DBG_LOCAL` ✓（`:5575` ✓ 打印 `body=` ✓、`:5592` 打印 `bp=/cnt=/own=` ✓）
   ⇒ 一跑就看出来 `b` **只打印第一行** ✓（卡在 `bp` 那步 ✓）—— 记下来 ✓：以后查这趟先用它 ✓。

   （历史）2. **W12 相邻两个死局部变量** ✗ —— `unused variable 'b'` ✓ ⇒ **只删掉了一个** ✓。

   **第 15 轮的排查（含一次失败尝试 ✓）**：

   - 现象被**精炼**了 ✓：**不论名字长短** ✓，相邻两条里**第二条**总是残留 ✗（`a`/`b` ✓、`aa`/`bb` ✓、
     `alpha`/`beta` ✓ 三组实测都是第二条 ✓）⇒ **不是**子串命中（一字母名）✓、**也不是**偏移失效 ✗；
   - 那趟的结构 ✓：外层 `for (;;)` ✓ + 每轮只切**一刀** ✓（`cut = true` ⇒ 重建文本 ⇒ 下一轮重搜 ✓，
     `codegen.c:5555-5725` ✓）⇒ 设计上**应该**能连着切 ✓；
   - **失败的尝试（已回退 ✗）**：我猜"第一刀之后 `textIsOriginal = 0` ✓ ⇒ 退路用 `df->body`（指向
     **重建前**的旧文本 ✗）⇒ 第二条的查找落在过期内存上" ✓，于是把退路改成**按原型在新文本里现搜**
     （用现成的 `funcDefStart` ✓）⇒ **毫无效果** ✗（三组名字依旧残留第二条 ✓、闸门零变化 ✓）
     ⇒ 所以症结**不在**这个退路 ✓；
   - **第 16 轮的打点（已做 ✓，结论很硬 ✓）**：在循环头打印
     `i/name/own/text/func` ✓ ⇒ 那趟**确实在反复重跑** ✓（40 行日志 ✓，`i=4 skip text=NULL` 就是
     被切掉的那条 ✓）✓；对 `main` 的两条：
     ```
     i=7 name=a own=1 text=yes  func=main      ⇒ 下一轮 text=NULL（**a 被切掉了** ✓）
     i=8 name=b own=1 text=yes  func=main      ⇒ **每一轮都是 text=yes**（**b 始终没被切** ✗）
     ```
     ⇒ **拒绝发生在循环头之后的某个守卫** ✓：要么 body 没找到 ✓，要么 `countMentionsIn` 认为有人读它 ✓，
     要么 `own` 判定 ✓。**下一轮的打点**（已缩到三行 ✓）：在 body 查找之后打印 `body 找到与否` ✓、
     在提及计数处打印 `count` ✓、在 `own` 判定处打印结果 ✓ —— 定位后一处一行就能修 ✓。

   （历史）**下一轮的打点** ✓：在那趟里对每条 `DeadLocal` 打印
     `name/own/找到 body 与否/是否进入切分支/cuts 条数` ✓（含 `EXTC_LOC` 门控 ✓），
     看第二条是"**根本没进循环**"（登记没进去 ✓）还是"进了但被某个 `continue` 挡下" ✓
     （`:5564` 的 `!d->text || !d->funcName` ✓ / `own` 判定 ✓ / `countMentionsIn` 认为被读了 ✓）。

（历史）## 三点二十二之前、攻击组 events（12 题）：**全绿** ✓

事件层（`epoll` + `AF_UNIX` socket）**没有 stdlib 包装** ✓，只能用 `extern!("extc")` 触达 ✓ ——
而 codegen 置 `needEvent` 靠的正是"**名字前缀**（`extc_epoll_` / `extc_sock_`）+ **被调用**" ✓
（`codegen.c:6318-6324` ✓）⇒ **这条链本身**就是攻击面 ✓。运行期导出与签名 ✓：
`extc_epoll_new()` ✓、`extc_epoll_add(ep, fd, readable)` ✓、`extc_epoll_wait(ep, timeout_ms)` ✓、
`extc_sock_pair(out: mut ref i64)` ✓、`extc_sock_read/write(fd, buf: ref u8, n)` ✓、
`extc_sock_nonblock(fd)` ✓（都在 `src/coroutine.c:106` 的 `eventEmitRuntime` 里 ✓）。

覆盖 12 题 ✓：`epoll_new` 拿到 fd ✓ · 空 epoll 上 `wait(0)` ✓ · **socket 对写一字节再读回** ✓ ·
`epoll_add` 用**负 fd** ✓ · `wait` 用**坏 ep 号** ✓ · `sock_read` 读 **0 字节** ✓ ·
`readable` 传 **2** ✓ · `nonblock` 后读空 ✓ · **端到端就绪**（写 ⇒ epoll 就绪 ✓）✓ ·
**负超时** ✓ · **跨特性**：事件层在**协程体**里 ✓ / 在**泛型函数**里 ✓ ⇒ 全绿 ✓
（其中几条走的是 `extern!` 的类型规则被拒 ✓ —— 也在攻击面内 ✓）。

## 三点二十二、攻击组 nocopy / ctor / ops（17 题）：**全绿** ✓

覆盖：`@noCopy` 被移走后不能再 use（拒 ✓）· 按 `ref` 传 ✓ · 读字段/调方法 ✓ ·
**从局部变量 `return` = 拷贝 ⇒ 拒** ✓ · **直接返回新字面量 ⇒ 允许** ✓ · 赋给另一变量后再用源（拒 ✓）·
在结构体字面量里被复制（拒 ✓）· 放进数组字面量（拒 ✓）· 构造糖 `T(args)` ✓ · 元数写错（拒 ✓）·
`new` 返回别的类型（拒 ✓）· 可失败构造 ✓ · **泛型结构的构造糖** ✓ · `==` 与派生的 `!=` ✓ ·
右操作数类型不对（拒 ✓）· 同一算符的**异构重载** ✓ · 比较算符返回非 bool（拒 ✓）。

**题目自己踩过一次** ✓（值得记 ✓）：我原本把"`fn make() -> c { var a: c = …; return a }`"写成
**允许** ✗ —— 实际被拒 ✓，而且这是**设计一致的** ✓：官方 `tests/nocopy/errors/` 的
`bind_copy`/`assign_copy` 就是把"值位置使用 = 拷贝"一律拒掉 ✓ ⇒ 这个语言**没有"从局部变量移动
出去"** ✓，`return a` 与 `var b = a` 同判 ✓。要返回新值就直接返回**字面量** ✓（官方 ctor 用例的
写法 ✓）⇒ 题目改成两条：`reject` + 一个新的正例（直接返回字面量 ✓）。

## 三点二十三、攻击组 containers（17 题）：**全绿** ✓

覆盖 `vector<T>`（push/len/capacity ✓、**跨容量边界 1000 次后仍 dense** ✓、`get` 越界给 `null` + `??` ✓、
**切片下标越界带位置 trap** ✓、空容器 ✓、元素是结构体/泛型实例 ✓、`@noCopy` 元素被拒 ✓、容器传进
函数 ✓、`pop` 的 option ✓、`clear` 之后再用 ✓、容量 ≥ len ✓）与 `hashMapI64<T>`/`hashSetI64`
（put/get/覆盖/删除/contains/len ✓、装结构体值 ✓、空表 ✓、`@noCopy` 值被拒 ✓）。

**两条题目写法教训** ✓（都是我自己的错 ✓，记下来省下次的时间 ✓）：

1. **vector 本身不可下标** ✗ —— 下标要在 `let dv = v.toSlice()` 得到的切片上 ✓（官方
   `tests/stl/vector.extc` 的写法 ✓）；`pop()` 返回 **option** ✓（`?? 默认值` ✓）。
2. **hashMap 要 `use stl::hashMap`** ✓（大写 M ✓）；且我这轮**两次**在生成题目时踩了转义坑 ✓：
   第一次写 `"\\n"`（heredoc 直传 ⇒ Python 得到**字面反斜杠+n** ✗）⇒ 题目源码里出现 `\` ⇒
   extC 报 `unexpected character` ✓；第二次 `%r` 输出把真换行转义成 `\n` 是对的 ✓，但**源头**必须是
   真换行 ✓ ⇒ 结论：**用逐行列表 + `"\n".join` 构造题目源码** ✓（heredoc 里写**单个** `\n` ✓），
   并**先 dump 出题面看一眼**再跑 ✓。

## 三点二十四、攻击组 io（16 题）：**全绿** ✓

**刻意避开** `tests/io/` 已覆盖的族（EOF ✓、CRLF ✓、超长行 ✓、`cerr`/`cout` 分流 ✓），专打相邻形状 ✓：
读完再读回整数 ✓ · **整数溢出 i64**（`99999999999999999999999`）✓ · 负数 ✓ · 带正号 ✓ · 浮点往返 ✓ ·
**没 `openIn` 就读** ✓ · **写进不存在的目录** ✓ · **读到 EOF 之后的整数** ✓ · 一次读多个（部分读 ✓）·
**零长缓冲** ✓ · **极小缓冲（截断）** ✓ · **关掉输出流之后再写** ✓ · 布尔字面量 ✓ · u8 切片往返 ✓ ·
**同一个文件开两次输出流** ✓。每题**自带输入文件** ✓（先 `openOut` 写、再 `openIn` 读 ✓），不依赖仓库数据 ✓。

⇒ 这一面的实现（`std::fs` 的流与格式化）在这些边界上**全部按设计** ✓。

## 四、事故：fuzz 产物把 /tmp 写满，连带把工具链卡死（2026-09-28，round 9）

**现象**：`/tmp` 写满（`ENOSPC`）⇒ **bash 工具起不来**（它的暂存也在 `/tmp`）⇒ `rm`/`df`/`grep`
一类要起外部进程的工具全部失效 ⇒ **想删东西却需要一个 shell**，先有鸡还是先有蛋。
只读的 `Read` 与写 **home** 的 `Write` 仍然可用（两个文件系统不同：`/tmp` 是 tmpfs，home 是 ext4）。

**原因**：`/proc/mounts` 显示 `/tmp` 是 **tmpfs（内存盘）**（`tmpfs /tmp tmpfs rw,...`，没有
`size=` ⇒ 默认约一半内存）。而 fuzz 每次迭代都要落 `case.extc` / `case.c` / **ASan 可执行文件**
（约 1 MB），一轮战役几百到几千次迭代，三种模式 × 多种子叠加几轮就足以吃掉内存盘。

**已经做的改正**：

1. `tools/fuzz.py` 的 `--out` 默认值由 `/tmp/extc-fuzz` 改为 **`~/extc-fuzz`**
   （`os.path.expanduser`），模块文档里写明"**不要放在 /tmp**"及原因；
2. 新增 **`tools/clean-tmp.sh`**：一键清掉 `/tmp` 下 fuzz/参考/工作树/探针产物并报告释放量，
   `--dry-run` 只看不删；只碰 `/tmp` 下的已知前缀，不进仓库。

**约定**（免得下次再踩）：**fuzz 产物、参考副本、隔离构建的 worktree 一律放 `~/extc-work/`**
（仓库之外的 home 目录）；`/tmp` 只用来放编译器中间文件那种短命对象。

**当时的进展状态**：战役 3（三模式 × 三种子 = 2250 次迭代）已经跑完，结果还**没有读出来**
（日志 `/tmp/fuzz3.log` 与产物 `/tmp/fuzz3` 都在，清 `/tmp` 之前可以先 `tools/fuzz-triage.py
/tmp/fuzz3` 分诊；若要保留就先复制到 `~/extc-work/`）。编译器本体停在与 `64e3c3b`/`a26b8ad`
一致的状态（② 已修、基准 413/413 且 0 份非法 C）。恢复后的第一件事：分诊战役 3，然后按
H 系列套路继续。


# X 批次：把 AST 从"输入 / 分析结果 / 代码生成计划"三合一里解耦

主人 2026-10-02：**先处理第 1 项**（AST 与"分析产物/计划"解耦），goal 模式。

阶段：X0 盘点 → X1 立接缝（`plan` 访问器收口读点）→ X2 逐族搬家 → X3 实例集显式化 →
X4 收尾（AST 只剩语法事实与身份）。**每阶段行为不变**，验收 = 发布与 `EXTC_DBG=1` 两模式
325 测试 + 五道闸门 + 18 份语料 + walker/guards 全绿。

## X0（本轮完成）：盘点

**产物**：[docs/topics/AST-ANNOTATIONS.md](../docs/topics/AST-ANNOTATIONS.md) ——
字段 → 谁写 → 谁读 → 陈旧会怎样的权威表，按四类分（① 语法事实 / ② 分析缓存 / ③ 编译计划 /
④ 身份与重指）。复核脚本：`tools/x0_annotations.py`（`grep` 式计数，口径写在脚本里）。

**盘出来的三件事**（都比开工前的预期更具体）：

1. **③ 计划族 = 20 个字段**，其中被 codegen 读得最多的是 `func`（58 次，调用点重指到实例）、
   `tmpl`(15)、`isCoro`(14)、`cname`(14)、`coroNeedsZone`(12)、`coroProto`(11)、
   `arenaLevel`(9)、`zoneLevel`(8)、`yieldType`(8)、`usesHome`(8)、`makesPool`(8)、`instName`(8)。
2. **耦合是双向的**（这是本轮最重要的发现）：codegen **反过来写**分析状态 ——
   `substParams`/`substArgs` 各 **8** 次（借用检查器的全局替换状态做实例代码生成）、
   `used` 4 次（影响检查器"没被调用就不必复核"的判断）、`name` 15 次（去重改名）、
   `owSites`/`owLocal` 各 3 次、`body` 3 次、`coroBoxed`/`ret` 各 1 次。
   ⇒ "哪个 pass 先跑"因此变成**语义问题**（P0-5 的根因正是"实例物化＝重指节点"这一副作用）。
3. **半语法半计划的字段**存在：`forStep`（parser 也写）、`condAllocs`（检查器写、codegen 读）——
   搬家时要保留"语法那一半"的归属。

**本轮没有改任何代码**（X0 是纯盘点）。

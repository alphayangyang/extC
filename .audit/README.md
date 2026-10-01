# .audit/ —— 2026-09-30 全量审阅的原始证据

本目录是 [`docs/reviews/COMPILER-AUDIT-2026-09-30.md`](../docs/reviews/COMPILER-AUDIT-2026-09-30.md) 的原始材料，
不是编译器的一部分，也不参与构建/测试。

- `REVIEW-PROTOCOL.md`：审阅协议（给每个审阅单元的共同规则：逐行读、注释不可信、每条必须带位置/引用/复现）。
- `findings/*.json`：**全部 31 个审阅单元**的原始 finding 列表（Lead 调度的单元原本写在 /tmp，已一并归档到这里）。格式：

  ```json
  {"findings":[{"title":…,"file":…,"line":…,"kind":…,"severity":…,
                "code_quote":…,"why":…,"trigger":…,"repro":…,"confidence":…}],
   "coverage_notes":"读过的范围 + 已核实为正确的点 + 无法确认的点"}
  ```

- `findings-index.tsv`：全部 173 条 finding 的机器可读索引（级别 / 类型 / 位置 / 来源 / 标题）。
- `design/`：**决策提取**（7 个单元，约 890 条决策记录 + 上百处矛盾，每条带出处）：
  `decisions-part1/2.md`（DECISIONS.md 逐条）、`principles.md`（SPEC/DESIGN 上位原则）、
  `memory-decisions.md`（M-01…M-75）、`types-decisions.md`（D1–D171）、`runtime-decisions.md`（274 条）、
  `manual-contracts.md`（手册契约 161 条 + 48 处冲突）。
  它们支撑 [`docs/DESIGN-RESOLUTIONS.md`](../docs/DESIGN-RESOLUTIONS.md)（18 条设计裁定）。
- `notes.md`：审阅过程中的工作笔记（Lead 亲测条目）。

命令行复现用到的脚本与最小程序在 `/tmp/rev/`（变异 fuzz、ASan/UBSan 全量扫描、`--check-c` 全语料扫描、
汇总脚本），报告附录 A 有索引。若 `/tmp` 已被清理，按各 finding 的 `repro` 字段重建即可
（每条都写了完整命令与最小程序）。

> 这些 JSON 里 `severity` 是**报告者自评**；正文的 P0 全部经 Lead 本机复跑，P1 部分复核。

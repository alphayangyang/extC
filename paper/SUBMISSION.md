# 投稿状态与被审风险清单（Submission notes）

本目录是论文的唯一来源：一份正文（body.tex + chapters/）加三套前置页，由
build.sh 生成四个 PDF。所有数字都来自提交 98bd56d2 的树（见 chapters/artifact.tex 的
「Frozen build」段），本文档只记录**版式、页数、匿名性、投稿去向**这类工程事实，不重复论文结论。

## 1 构建

```bash
cd paper
./build.sh            # 匿名 SIGPLAN 提交版 -> main.pdf
./build.sh journal    # 匿名期刊版（acmsmall） -> main-journal.pdf
./build.sh full       # 具名相机就绪 / arXiv 版 -> main-full.pdf（读 authors.tex）
./build.sh artifact   # ACM Artifact Appendix -> artifact-appendix.pdf
./build.sh all        # 以上四者
./build.sh clean      # 删中间产物，保留入库的 .tex/.tsv/.pdf
```

脚本自己挑 TeX：本机 ~/.local/bin 的 TeX Live 2026 没有 acmart.cls，
/usr/bin 的 TeX Live 2025/Debian 有；pick_tex 找的是**自己的 kpsewhich 能看见
acmart.cls 的那个 pdflatex**，找不到就报错退出而不是用错版本编译。四遍编译（bibtex 一次）。

入库的是：三个入口 main.tex / main-journal.tex / main-full.tex、共享的 preamble.tex、
frontmatter-anon.tex / frontmatter-full.tex / body.tex、chapters/ tables/ data/ figures/、
references.bib、artifact-appendix.tex、authors.tex（占位）以及两个 PDF：
main.pdf（匿名投稿版）与 artifact-appendix.pdf。变体 PDF（main-journal.pdf / main-full.pdf）
由 .gitignore 忽略：它们只是同一份源码换一个 \documentclass，改 authors.tex 即可重建。

当前构建结果（0 warning / 0 error）：

| 目标 | 页数 | 正文 | 参考文献 | 附录 |
|---|---|---|---|---|
| main.pdf（sigplan, anonymous） | 26 | pp.1–21 | p.22 | pp.23–26 |
| main-journal.pdf（acmsmall, anonymous） | 38 | pp.1–32 | p.33 | pp.34–38 |
| main-full.pdf（acmsmall, named） | 38 | 同上 | 同上 | 同上 |
| artifact-appendix.pdf | 3 | — | — | — |

正文分节页（sigplan / journal）：§1 引言 p1/p1；§2 设计 p3/p3；§3 形式化 p5/p7；§4 roofline p9/p13；
§5 实现 p12/p18；§6 评测 p15/p22；§7 相关工作 p19/p28；§8 局限 p20/p30；§9 结论 p21/p32。

## 2 页数账与投稿去向

**事实**：SIGPLAN 版式正文 21 页，参考文献 1 页，附录 4 页。SIGPLAN 会议（OOPSLA/POPL/PLDI）的正文
上限一般是 12 页（不含参考文献与附录），我们超 9 页。这不是排版可以挤掉的量：多出来的是
§3 的完整证明（4 页）、§4 的推导（3 页）、§5 的实现（3 页）、§6 的四张表。

结论：**这份稿子的自然形态是期刊或 arXiv，不是 12 页会议论文**。三条路，按推荐顺序：

1. **期刊（推荐）**：ACM TOPLAS / TACO 没有硬页数，acmsmall 版 38 页正好；审稿人会读证明与
   roofline 推导，那正是本文相对「又一个 GC」的差异所在。main-journal.pdf 是为此准备的。
2. **arXiv 预印本**：main-full.pdf（填 authors.tex）直接挂，占位发表时间戳，会议版再裁。
3. **12 页会议版**：需要一次真正的裁剪，见 §4，估算工作量半天到一天，且会牺牲可复现细节。

不建议现在硬裁：本文的卖点是"两个数都能被证明"，把证明搬空只剩测量，就退化成普通的运行时论文。

## 3 匿名性

当前 main.pdf 与 artifact-appendix.pdf 的匿名检查已过：

```bash
pdftotext main.pdf - | grep -in "alphayang\|extC_Compiler\|github\|zenodo\|The extC Project\|China\|localhost"
```

零命中（main-journal.pdf 同样零命中）。已做的三件事：

- 前置页换成 frontmatter-anon.tex：匿名作者块 + 机构写 "Paper under double-blind review"，
  国家字段必须非空（acmart 会报 "No country present for an affiliation"，填 "Review copy"）；
- 页脚/页眉不带项目名（printacmref=false，nonacm）；
- 论文正文里不出现提交号以外的仓库路径，仓库以 "the artifact" 指代；致谢段留空（相机就绪再填）。

**仍待办（投稿前必须处理）**：

- [ ] 代码托管：现在没有任何匿名仓。要挂 Zenodo 匿名 DOI 或 anonymous.4open.science，
      再把 URL 写进 artifact 附录的 "Publicly available" 一行（该行现写 "yes"，附注 "anonymous mirror pending"）。
- [x] `chapters/appendix.tex` 的小节标题已从 "Corrections to numbers we published earlier" 改成
      "Corrections to earlier internal measurements"（匿名稿不该暗示先前已发表）。
- [ ] 若审稿系统要求 PDF 元数据：pdfinfo 看 Author/Creator 字段，acmart 默认写 "Anonymous Author(s)"，仍值得核一遍。

## 4 12 页会议版的裁剪方案（未执行，需要时按此做）

目标：正文 ≤ 12 页（含图表），参考文献与附录不计。当前 21 页，要砍 9 页。按性价比排序：

| 动作 | 省页 | 代价 |
|---|---|---|
| §5 实现（3 页）压成 §2 的 "Implementation" 半页，细节进附录 | −2.5 | 丢了 instrument 口径讨论，要指针 |
| §3 的形式化（4 页）：定理全留、证明草稿进附录 B | −2 | 审稿人可能要求证明在正文 |
| §4 roofline（3 页）：留 Theorem 14–17 + 图 1，推导进附录 | −1 | 可接受，模型仍可核 |
| §6 的四张表（allarms / ablatepause / bounded / census）移附录 | −2 | 结论段要引用附录号 |
| §2 设计（4 页）：五基质表 + 三档枚举压半页 | −1.5 | 读者要跳附录才看到机制全貌 |
| §7 相关工作 + §8 局限各压 1/3 | −1 | 常规编辑，不丢信息 |

合计约 −10 页，留 1 页余量。裁剪只动 body.tex 与两份 appendix*.tex 的归属，**不动**
data/ 与 tables/ 的内容，保证两版数字一致；裁完把 build.sh 加一个 conf 目标。

## 5 Artifact Appendix（artifact-appendix.tex，3 页）

按 ACM Artifact Review 的模板写，含：check-list 表（Program / Runtime / Proofs / Tests / Data set /
Hardware / RTE / Optional toolchains / Metrics / Output / Experiments）、Description、Creation、
Getting started（四步：make、./build/extc、build.sh、bench）、Evaluation and expected results
（39/16/117/15 ms、RSS 3,420 kB、p99.9 10–58 µs、callgrind Ir 预算 88,029,115）、Methodology
（交错取 min、taskset -c 6、/usr/bin/time -f %M、13% 跨会话漂移、不要并发 make）、
Anonymization status。

**给评审者的最短路径**（也在附录里）：

```bash
make -j && ./build/extc --version          # 编译器 + 运行时
cd proofs && ./verify.sh                  # 需先按 README 按依赖序建 .olean（olean 不入库）
cd tests && ./run.sh                      # driver 216 例 + ASan/UBSan 变体
cd bench/gc/v2 && B=/tmp/b PIN=6 ./run.sh align && REPS=7 B=/tmp/b PIN=6 ./run.sh bench
```

注意 proofs/verify.sh 直接跑会红（脚本顺序固定且依赖 .olean；olean 已在提交 8e7f293b 移出版本库），
建好后步骤 2/3/5/6/7/8 绿，步骤 4 的 Z3 交叉复核只因本机未装 z3 失败——附录里如实写了这一点，
评审者按附录步骤操作才能复现，这是本附录最重要的一段。

## 6 相机就绪 TODO

- [ ] authors.tex 填真名、单位、邮箱、ORCID（现在全是占位）；
- [ ] CCS Concepts 用 ACM CCS 工具重生成（现在四条是手写的 500/300/200/100 权重，需核）；
- [ ] 致谢与基金（现在空）；
- [ ] 若走期刊：main-journal.tex 的 acmsmall 版式要补 \begin{CCSXML} 或确认 ACM 模板版本；
- [ ] 相机就绪版重跑一次四形状（REPS=7）确认数字未漂移，若漂移先改 data/ 再重生成表与图；
- [ ] 冻结口径写死：论文引用 98bd56d2，相机就绪若换 commit，chapters/artifact.tex 的
      "Frozen build" 段（reclaim.c 4,212 行 / md5 7151a431 等）必须同步。

## 7 复现检查脚本（投稿前跑一遍）

```bash
cd paper && ./build.sh all && rm -f main-journal.pdf main-full.pdf   # 0 warning 0 error
pdftotext main.pdf - | grep -in "alphayang\|extC_Compiler\|github\|zenodo\|China" ; echo "anon grep rc=$?"
pdftotext artifact-appendix.pdf - | grep -in "alphayang\|extC_Compiler" ; echo "anon grep rc=$?"
cd .. && ./check.sh docs && python3 tools/check_tree.py --gate && python3 tools/check_changelog.py --gate
```

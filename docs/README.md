# docs 索引

文档分四层。**每篇第一行都写着"状态"**，先看那行再决定要不要读：

| 层 | 目录 | 是什么 |
|---|---|---|
| **手册** | [`usage/`](usage/) | 现在怎么用：语言、标准库、模块与 C ABI、驱动器开关、回收。改语义先改这里。 |
| **设计** | [`design/`](design/) | 现在为什么这么设计：类型模型、内存模型、回收、并行、融合、数组、模块、核心决定。 |
| **计划** | [`plans/`](plans/) | 还没做或做了一半：[`TODO.md`](plans/TODO.md)（唯一待做清单）、体量收敛、库形状、简洁度量尺、元素引用。 |
| **历史** | [`changelog/`](changelog/) · [`archive/`](archive/) | 唯一的开发历史（[`CHANGELOG.md`](CHANGELOG.md) 是索引）。`archive/` 只作考古。 |

## design/ —— 现在为什么这么设计

| 文档 | 一句话 |
|---|---|
| [`CORE.md`](design/CORE.md) | 六条不许破的决定与预算表 —— 动别处之前先看这页 |
| [`TYPE-MODEL.md`](design/TYPE-MODEL.md) | 值、引用、复合，以及它们各自装在什么载体里；拷贝/移动/借出/快照四个词的定义 |
| [`MEMORY-MODEL.md`](design/MEMORY-MODEL.md) | 三种存储（帧 / 板块 / 对象表）与"该用哪个"的判据 |
| [`POOL-LEVELS.md`](design/POOL-LEVELS.md) | 池层级的规则与边界：存储/读回/拒绝各一条 |
| [`ALLOCATORS.md`](design/ALLOCATORS.md) | 分配器分层：arena、板块、对象表各管什么 |
| [`BUF-INVARIANT.md`](design/BUF-INVARIANT.md) | 可增长数组：一个不变量（一片预留）、四个动作、视图何时失效 |
| [`DESTRUCTORS.md`](design/DESTRUCTORS.md) | 析构：`@drop` 的全部时机与顺序 |
| [`RECLAMATION.md`](design/RECLAMATION.md) | 四层回收：按分配点选策略、运行期何时整体删掉 |
| [`PARALLEL.md`](design/PARALLEL.md) | `par` 的证明义务；线程与回收的规则（实现暂缓） |
| [`EFFECTS.md`](design/EFFECTS.md) | `request` / `suspend` / `handle`：等待是类型、驱动器是词法作用域（设计稿 v1，未实现） |
| [`FUSION.md`](design/FUSION.md) | 视图融合：什么形状能合、逐位一致的硬约束 |
| [`ARRAY.md`](design/ARRAY.md) | 数组与视图语义、rank 与形状 |
| [`MODULES.md`](design/MODULES.md) | 模块身份、导入、门面、所有权边界 |

## plans/ —— 还没做的

| 文档 | 一句话 |
|---|---|
| [`TODO.md`](plans/TODO.md) | 全仓库唯一的待做清单（做完就搬进 `usage/` 或删掉） |
| [`TRIM.md`](plans/TRIM.md) | 体量收敛：哪些重复答案先删、按什么顺序 |
| [`LIBRARY-SHAPE.md`](plans/LIBRARY-SHAPE.md) | 标准库该长什么样（45 → 20 模块的四层依赖） |
| [`CONCISENESS.md`](plans/CONCISENESS.md) | 简洁度量尺与糖清单 |
| [`ELEMENT-REFS.md`](plans/ELEMENT-REFS.md) | 元素引用：借容器里的东西而不拷贝 |

## 其它

- 贡献流程见根 [`CONTRIBUTING.md`](../CONTRIBUTING.md)；仓库门面见根 [`README.md`](../README.md)。
- 支持的平台、安装方式、alpha 的承诺边界与版本策略见根 [`RELEASE.md`](../RELEASE.md)。
- 专题页（编辑器、内部实现）在 [`topics/`](topics/)。
- 注释规范见 [`COMMENT-STYLE.md`](COMMENT-STYLE.md)。

## 维护规则

**先记住这一条：写文档是为了以后还能维护它。**

1. **文档不写历史。** 历史只进 [`CHANGELOG.md`](CHANGELOG.md) 与它的月份页（[`changelog/`](changelog/)）。下面这些属于历史：

   ```
   以前是 X、现在改成 Y   ·   定案 88   ·   PLAN #47   ·   当时实测过   ·   回退过一版
   ```

2. **写给人的散文。** 句子优先，读一遍能懂。**不要用编号当引用**——要读者追着编号跑的写法都别用：

   ```
   见 §3.2   ·   见 定案 88   ·   见 P0-12   ·   见 #47
   ```

   要指向别处，就用**超链接**指向真正讲它的那一页。
3. **不引用实现。** 正文里不写源码文件、行号、函数名、内部标识符——实现一变，文档就烂。
   把**行为、用法、概念**说清就够了。命令、构建产物、测试套件名属于用户界面，可以写。
4. **`TODO.md` 的生命周期**：条目**做完 → 搬进 `usage/`（或舍弃）**；**砍掉 → 直接删除**
   （不留"已取消"，不留删除线）。
5. **写的时候要答得出"我怎么知道这是真的"**（跑过，或对着代码读过），但**验证证据不进文档**：
   `file:line`、命令与输出属于**复核报告**，不属于正文。
6. **注释用 Doxygen**：`@brief` / `@param` / `@return` / `@code` —— 说明、参数、返回值、使用例。
   函数体内只留**不显然的不变量**。规范见 [`COMMENT-STYLE.md`](COMMENT-STYLE.md)。
7. **示例要有闸门**：围栏里能独立跑的 extC 示例必须编得过；不能独立跑的片段要显式标注为片段。
   没有闸门的例子会先烂。

## 验收里的文档与注释闸门

`./check.sh quick` 里这些闸门都**会让整体变红**（`tools/` 下同名脚本）：

| 闸门 | 挡什么 |
|---|---|
| 历史词 | 活文档与源码注释里出现历史叙述（中文与英文各一套，词表见下面的围栏）|
| 历史分页 | 月份页与索引对不上、条目跑错月份、索引页混进正文 |
| 文档引用 | `src/` 与 `stdlib/` 的注释里出现 `.md` 路径（文档会改名，链接会死）|
| Doxygen 覆盖 | 函数声明与定义上方没有 `@brief`（**棘轮**：按文件记欠账，只许变短）|
| `@code` 例子 | 注释里的 extC 例子编不过或跑不过；不能独立跑的片段没标 `!skip 原因` |
| 文档里的示例 | 围栏里的完整程序编不过；片段没标 `<!-- fragment -->`；标了"必须编不过"却编过 |
| 驱动开关页 | `docs/usage/flags.md` 与 `extc --help` 不一致 |
| 公开面覆盖 | 驱动开关没写进文档、或标准库公开成员的文档覆盖**退步**（棘轮）|

历史词闸门认的词（要看词表就跑 `python3 tools/check_no_history.py`）：

```
定案 N   ·   PLAN #N   ·   以前 · 曾经 · 回退 · 一开始 · 那次 · 一度
实测过 · 量过 · 测过   ·   audit / 审计批次编号
used to · measured · no longer · earlier version · previously · originally
```
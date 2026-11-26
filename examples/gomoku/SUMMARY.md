# gomoku — 一个能下的五子棋（文本 + 全屏 + bench）

15×15 自由规则五子棋，带一个 Alpha-Beta 搜索的 AI。**一个小项目**（`src/` 下六个文件），
一个二进制四种跑法：人机对局、AI 自对弈、搜索基准、全屏界面。

## 跑

```sh
./build/extc run examples/gomoku                      # 人（黑）对 AI（白），深度 6
./build/extc run examples/gomoku -- --selfplay        # AI 对 AI，深度 4，确定性
./build/extc run examples/gomoku -- --bench --depth 6 # 固定中局重复搜索，报 nps
./build/extc run examples/gomoku -- --tui             # 全屏界面（需要真终端）
./build/extc run examples/gomoku -- --help            # 用法
```

文本模式落子写记谱（`H8`、`h8`、`8h` 都认），`u` 悔棋、`n` 新局、`q` 退出。
全屏模式方向键或 `hjkl` 移动、Enter 落子、`u` 悔棋、`n` 新局、`q` 退出。

## 文件

| 文件 | 作用 |
|---|---|
| `src/main.extc` | 项目入口：解析参数、按模式分发 |
| `src/board.extc` | 棋盘：落子/悔棋、五连判定、候选点生成、记谱解析与格式化 |
| `src/eval.extc` | 局面评估：四个方向的五格窗口打分 |
| `src/search.extc` | 搜索：候选点排序、Negamax + Alpha-Beta、迭代加深、按节点数查时钟 |
| `src/textui.extc` | 文本前端：人机对局、自对弈、bench |
| `src/tuiapp.extc` | 全屏前端：`std::tui` 的屏幕对象 + 按键解码 |

## 它演示了什么

- **多模块项目**：`extc run examples/gomoku` 直接把一个目录当项目跑，`use board` 从
  `src/` 解析；模块之间用 `board::N` / `use board::{N, BLACK, board}` 这种普通导入，
  没有构建文件、没有注册表。
- **结构体方法当对象用**：`board` 的落子/悔棋/胜负判定全在 `struct` 体内声明，
  `place(self: mut ref board, …)` 改棋盘、`lastWon(self: ref board, …)` 只读；
  数组字段 `[N][N]i64` 内联，没有分配。
- **只读参数写借**：`eval(b: ref board, …)`、`pickMoves(b: ref board, …)` 全按引用收，
  搜索里递归传递不复制棋盘。
- **确定性自对弈**：`--selfplay` 不用时钟（`budgetMs = 0` 只看深度），同一份代码两次跑出
  逐字节相同的对局 —— 测试就靠这一条抓搜索里的不确定性。
- **可复现的基准**：`--bench` 在固定中局上重复搜索，报节点数、毫秒与 nps；本机约
  29 万 nps（深度 6 / 3 轮，单线程）。
- **真终端才能测的界面**：全屏模式用 `std::tui` 的 `screen`（`@drop` 恢复终端），
  `tests/gomoku/pty_run.py` 在真 PTY 里喂按键、断言备用屏与退出序列、比对 termios。

## 诚实的边界

- 评估函数是手调的五格窗口分，没有 VCF/VCT 威胁搜索，也没有置换表；深度 6 在开局
  很快，但中后盘会明显变慢。
- 搜索宽度按手数/深度拍死（根 16、浅层 10、其余 8），不是按历史启发动态调整。
- 只做自由规则，不做连珠禁手。
- 全屏界面没有鼠标、没有宽字符、窗口大小变了不重排（重启一次就好）。
- 单线程；并行搜索没做（`par` 只支持循环体，搜索是递归的）。

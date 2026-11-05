# 在 VS Code 里写 extC

两个部件：

| 部件 | 在哪 | 干什么 |
|---|---|---|
| `extc --diagnostics-json --check-only --source-stdin` / `--symbols-json` | WSL 里的编译器 | 把诊断与声明表变成机器可读的一行一个 JSON 对象 |
| `tools/extc-lsp.py` | WSL 里的仓库 | JSON-RPC 语言服务器：诊断、大纲、悬停、跳转定义、补全 |
| `tools/vscode-extc/` | 装到 Windows 侧 VS Code | 客户端 + 语法高亮 + 语言配置，用 `wsl.exe` 起服务器 |

服务器本身不含任何语言规则 —— 它每次都问编译器，所以编辑器里看到的错误与 `extc`
命令行给出的**是同一句话**。

## 一、装扩展

扩展目录已经是普通的"解包扩展"格式，复制到 Windows 的扩展目录即可：

```bash
# 在 WSL 里执行（路径按自己的用户名改）
DEST=/mnt/c/Users/<你>/.vscode/extensions/extc-lsp-0.1.0
mkdir -p "$DEST" && cp -r tools/vscode-extc/* "$DEST"/
```

然后重启 VS Code（或 `Developer: Reload Window`）。打开任意 `.extc` 文件，右下角语言
模式应显示 `extC`。

## 二、两种连法

### A. 目录在 WSL 里，VS Code 在 Windows 上开 UNC 路径（本仓库的默认情形）

打开 `\\wsl.localhost\<发行版>\home\<你>\extC_Compiler`，扩展在 **Windows** 侧运行，客户端按
配置把 Windows 路径翻成 WSL 路径，再通过 `wsl.exe` 起服务器：

```jsonc
// settings.json
{
  "extc.mode": "wsl",              // 显式要求走 wsl.exe；auto 在非远程窗口下也会这么选
  "extc.repoPath": "/home/alphayang/extC_Compiler",
  "extc.wslDistro": "",            // 留空 = 默认发行版
  "extc.python": "python3"
}
```

服务器进程实际是：

```bash
wsl.exe -e bash -lc 'cd /home/alphayang/extC_Compiler && EXTC=…/build/extc exec python3 tools/extc-lsp.py'
```

### B. 用 Remote-WSL 打开（推荐，路径不用翻译）

`Connect to WSL` → 打开 `/home/alphayang/extC_Compiler`。扩展宿主在 Linux 里，`extc.mode`
可以留 `auto`（检测到远程窗口就走 `native`）。语言服务器以 `python3 tools/extc-lsp.py`
直接启动。

## 三、现在能给什么

| 能力 | 行为 | 现状 |
|---|---|---|
| 诊断 | 保存/输入后（去抖 250 ms）整程序检查，坏在哪个文件就发到哪个文件 | ✅ 编译器的**第一个错误** + 全部警告 |
| 大纲 | `fn` / 方法 / `struct` / `type` / 全局 / `trait`，只列当前文件 | ✅ |
| 悬停 | 符号名 → `fn area / 1 parameter(s)`（声明级信息） | ✅ 尚无表达式的精确类型 |
| 跳转定义 | 符号名 → 同文件声明行 | ✅ 跨模块定义未做 |
| 补全 | 本文件符号 + 关键字 + 内建类型 + 文中出现过的词 | ✅ 不含模块公开面 |
| 高亮 | 关键字、类型、`@attribute`、f-string 槽、字符串、数字、注释 | ✅ |

已知短板（有意写在文档里，不假装）：编译器一次只报**第一个**错误（`ctxError` 只保留
首条），所以修完一个才看得到下一个；这不是 LSP 的 bug，但会影响手感。补全与悬停是
"声明级"，因为检查器目前不保留表达式类型表。

## 四、判据

```bash
./tests/lsp/run.sh        # 一次 JSON-RPC 会话：干净→空、坏→诊断、大纲、悬停、跳转、补全、关闭清空
```

`tests/lsp/drive.py` 是客户端脚本（真的起服务器），`tests/lsp/paths.test.js` 只测路径翻译
（`node` 不在时整条跳过）。`tests/lsp/run.sh` 已注册进 `tools/check.py` 的 `nobench`。

## 五、故障排查

- **没有诊断/大纲是空的**：先确认 `build/extc` 存在（`make -j8`）；再在 VS Code 输出面板
  选 `extC`（客户端把服务器 stderr 丢弃了，看 `Developer: Toggle Developer Tools` 的
  Console 更直接）。
- **`wsl.exe` 起不来**：`extc.wslDistro` 填发行版名（`wsl.exe -l -v` 看），或改
  `extc.mode: "native"` 配合 Remote-WSL。
- **编辑器显示两个同名标签**：说明同一缓冲区被两种路径拼写打开了；客户端会把诊断 URI
  映射回你打开时的那个 URI（`tools/vscode-extc/paths.js` 的 `known` 表），若仍出现，报给
  `tests/lsp` 加一条反例。

### 排查：扩展日志

`输出` 面板选 **extC** 通道：客户端会打印它到底执行了哪条命令、服务器 stderr、以及退出码。
第一条日志是 `[client] starting …`，成功时是 `[client] ready: native -> …` 或 `wsl.exe -> …`。
没有这条日志 = 扩展没被激活（见上：Remote-WSL 窗口要从 **远程** 侧安装，`~/.vscode-server/extensions/`）。

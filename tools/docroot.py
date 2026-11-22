#!/usr/bin/env python3
"""文档项目路径解析 —— 文档独立成一个兄弟仓库（默认 $HOME/extc-docs）。

编译器仓库里只剩"随代码一起变"的几页（`docs/README.md`、`docs/flags.md`、
`docs/surface.md` 与两份手册），其余活文档、设计与历史都在文档项目里。闸门因此要能
**跨仓库读文档**，并且在**没有文档项目**时明确跳过（不是失败）：

    EXTC_DOCS   文档项目根；缺省 $HOME/extc-docs
    EXTC_SRC    编译器源码根（文档项目里的符号锚点对着它解析）；缺省 `../extC_Compiler`

跳过协议（与 `tools/check.py` 一致）：打印一行 `  --   跳过：<原因>` 并 exit 0，
runner 记 skip、不计数 —— "没验"绝不能混进"通过"。
"""
import os
import pathlib
import sys

#: 编译器仓库根（本文件在 <repo>/tools/ 下）。
ROOT = pathlib.Path(__file__).resolve().parent.parent

#: 文档项目的默认名字：与编译器仓库兄弟克隆。
DEFAULT_NAME = "extc-docs"
SKIP_PREFIX = "  --   跳过："


def project():
    """文档项目根目录（不检查存在）。"""
    env = os.environ.get("EXTC_DOCS")
    if env:
        return pathlib.Path(env).expanduser().resolve()
    return pathlib.Path.home() / DEFAULT_NAME


def display(path=None):
    """把项目路径打成 `~/...` 的形式（消息里用；不展开用户主目录）。"""
    p = pathlib.Path(path) if path is not None else project()
    home = pathlib.Path.home()
    try:
        rel = p.resolve().relative_to(home)
    except (ValueError, OSError):
        return str(p)
    return str(pathlib.Path("~") / rel)


def doc(rel):
    """文档项目里的一份文件（不检查存在）。`rel` 是项目内相对路径。"""
    return project() / rel


def label(rel):
    """`doc(rel)` 的展示名。"""
    return display(project() / rel)


def src_root():
    """编译器源码根：文档项目里的 `src/...` 锚点对着它解析。"""
    env = os.environ.get("EXTC_SRC")
    if env:
        p = pathlib.Path(env).expanduser()
        if not p.is_absolute():
            p = project() / p
        return p.resolve()
    sibling = project().parent / "extC_Compiler"
    if (sibling / "src").is_dir():
        return sibling.resolve()
    return ROOT


def project_live_docs():
    """文档项目里的**活文档**（顺序稳定）：README/STYLE + guide/reference/design/plans。

    `devlog/`（历史、分析、整理记录）与 `archive/` 是记录，不是现状，不在活文档之列。
    """
    out = []
    for name in ("README.md", "STYLE.md"):
        p = project() / name
        if p.is_file():
            out.append(p)
    for sub in ("guide", "reference", "design", "plans"):
        d = project() / sub
        if d.is_dir():
            out.extend(sorted(d.glob("*.md")))
    return out


#: 兼容/口语别名。
live_docs = project_live_docs


def missing():
    """文档项目不存在时返回 True。"""
    return not project().is_dir()


def skip(reason=None):
    """按跳过协议退出：一行 `  --   跳过：...`，exit 0。"""
    print(SKIP_PREFIX + (reason or ("文档项目 " + display() + " 不存在（文档独立维护）")))
    sys.exit(0)


def require_or_skip():
    """缺文档项目就按跳过协议退出；返回项目根。"""
    if missing():
        skip()
    return project()

"""诊断信息。

设计原则 §11.6：错误信息是语言的一部分。
所以错误不是一个字符串，是 (行, 列, 信息, 注解)，并且渲染时带源码上下文。
"""


class CompileError(Exception):
    def __init__(self, line, col, msg, note=None):
        self.line = line
        self.col = col
        self.msg = msg
        self.note = note
        super().__init__(msg)


def render(err, path, source):
    """把 CompileError 渲染成一段可读的诊断。"""
    lines = source.splitlines()
    out = [f"{path}:{err.line}:{err.col}: error: {err.msg}"]

    if 1 <= err.line <= len(lines):
        text = lines[err.line - 1].rstrip("\n")
        out.append("  " + text)
        caret = " " * max(0, err.col - 1) + "^"
        out.append("  " + caret)

    if err.note:
        out.append(f"  note: {err.note}")

    return "\n".join(out)

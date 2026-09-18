"""extC 词法分析。

语句以换行结束（Go 风格），所以 NEWLINE 是一个**真 token**，
由 parser 决定它在哪些位置有意义（例如 `}` 之后、`,` 之后可以忽略）。

决策点 A：分号。这里实现的是「换行即语句结束」，分号不是必需的。
"""

from dataclasses import dataclass

from .diag import CompileError

KEYWORDS = {
    "fn", "let", "var", "if", "else", "while", "return",
    "break", "continue", "struct", "true", "false", "ref",
}

BUILTIN_TYPES = {
    "i8", "i16", "i32", "i64",
    "u8", "u16", "u32", "u64",
    "f32", "f64",
    "bool", "str", "void",
}

# 长符号必须排在短符号前面，靠最长匹配消歧义
PUNCTS = [
    "->", "==", "!=", "<=", ">=", "&&", "||",
    "+=", "-=", "*=", "/=",
    "+", "-", "*", "/", "%", "=", "<", ">", "!",
    "(", ")", "{", "}", "[", "]", ",", ":", ";", ".",
]


@dataclass(slots=True)
class Token:
    kind: str      # IDENT | INT | FLOAT | STRING | KEYWORD | TYPE | PUNCT | NEWLINE | EOF
    value: str
    line: int
    col: int

    def __repr__(self):
        return f"{self.kind}({self.value!r})@{self.line}:{self.col}"


class Lexer:
    def __init__(self, source, path="<input>"):
        self.src = source
        self.path = path
        self.i = 0
        self.line = 1
        self.col = 1
        self.toks = []

    # ---------- 基础设施 ----------

    def error(self, msg, note=None):
        raise CompileError(self.line, self.col, msg, note)

    def peek(self, k=0):
        j = self.i + k
        return self.src[j] if j < len(self.src) else ""

    def advance(self):
        c = self.src[self.i]
        self.i += 1
        if c == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return c

    def push(self, kind, value, line, col):
        self.toks.append(Token(kind, value, line, col))

    # ---------- 主循环 ----------

    def run(self):
        while self.i < len(self.src):
            c = self.peek()
            line, col = self.line, self.col

            if c in " \t\r":
                self.advance()
            elif c == "\n":
                self.advance()
                if not self.toks or self.toks[-1].kind != "NEWLINE":
                    self.push("NEWLINE", "\n", line, col)
            elif c == "/" and self.peek(1) == "/":
                while self.i < len(self.src) and self.peek() != "\n":
                    self.advance()
            elif c == "/" and self.peek(1) == "*":
                self.advance()
                self.advance()
                while self.i < len(self.src) and not (self.peek() == "*" and self.peek(1) == "/"):
                    self.advance()
                if self.i >= len(self.src):
                    self.error("unterminated block comment")
                self.advance()
                self.advance()
            elif c.isdigit():
                self.lex_number()
            elif c.isalpha() or c == "_":
                self.lex_ident()
            elif c == '"':
                self.lex_string()
            elif c.isspace():
                self.advance()
            else:
                self.lex_punct()

        # 保证文件末尾一定有一个语句终止符
        if not self.toks or self.toks[-1].kind != "NEWLINE":
            self.push("NEWLINE", "\n", self.line, self.col)
        self.push("EOF", "", self.line, self.col)
        return self.toks

    # ---------- 各类 token ----------

    def lex_number(self):
        line, col = self.line, self.col
        start = self.i
        is_float = False

        if self.peek() == "0" and self.peek(1) in ("x", "X"):
            self.advance()
            self.advance()
            while self.peek() and (self.peek().isdigit() or self.peek().lower() in "abcdef"):
                self.advance()
            self.push("INT", str(int(self.src[start:self.i], 16)), line, col)
            return

        while self.peek().isdigit():
            self.advance()

        if self.peek() == "." and self.peek(1).isdigit():
            is_float = True
            self.advance()
            while self.peek().isdigit():
                self.advance()

        if self.peek() in ("e", "E"):
            k = 1
            if self.peek(1) in ("+", "-"):
                k = 2
            if self.peek(k).isdigit():
                is_float = True
                for _ in range(k):
                    self.advance()
                while self.peek().isdigit():
                    self.advance()

        text = self.src[start:self.i]
        self.push("FLOAT" if is_float else "INT", text, line, col)

    def lex_ident(self):
        line, col = self.line, self.col
        start = self.i
        while self.peek() and (self.peek().isalnum() or self.peek() == "_"):
            self.advance()
        text = self.src[start:self.i]

        if text in KEYWORDS:
            kind = "KEYWORD"
        elif text in BUILTIN_TYPES:
            kind = "TYPE"
        else:
            kind = "IDENT"
        self.push(kind, text, line, col)

    def lex_string(self):
        line, col = self.line, self.col
        self.advance()  # 开引号
        start = self.i
        while self.i < len(self.src) and self.peek() != '"':
            if self.peek() == "\n":
                self.error("unterminated string literal")
            if self.peek() == "\\":
                self.advance()
            self.advance()
        if self.i >= len(self.src):
            self.error("unterminated string literal")
        text = self.src[start:self.i]
        self.advance()  # 闭引号
        self.push("STRING", text, line, col)

    def lex_punct(self):
        line, col = self.line, self.col
        for p in PUNCTS:
            if self.src.startswith(p, self.i):
                for _ in range(len(p)):
                    self.advance()
                self.push("PUNCT", p, line, col)
                return
        self.error(f"unexpected character `{self.peek()}`")


def tokenize(source, path="<input>"):
    return Lexer(source, path).run()

"""extC 的 AST。

week-0 只建**完整 AST**（不是 token 流重写）—— 理由见 DESIGN.md §5：
arena / 引用检查 / 单态化 / 好错误信息，四样都要作用域树和符号表。
"不解析整个 C" 是对的；"不建 AST" 是错的。
"""

from dataclasses import dataclass, field
from typing import Optional


# ---------------------------------------------------------------- 类型

@dataclass(slots=True)
class Type:
    kind: str                      # "name" | "ref"
    value: str = ""                # kind == "name" 时是类型名
    inner: Optional["Type"] = None # kind == "ref" 时是被引用的类型

    def __str__(self):
        if self.kind == "ref":
            return f"ref {self.inner}"
        return self.value

    @property
    def is_ref(self):
        return self.kind == "ref"


def name_type(n):
    return Type("name", n)


def ref_type(inner):
    return Type("ref", "", inner)


def strip_ref(t):
    while t is not None and t.kind == "ref":
        t = t.inner
    return t


# ---------------------------------------------------------------- 顶层

@dataclass(slots=True)
class StructDef:
    name: str
    fields: list
    line: int = 0


@dataclass(slots=True)
class Param:
    name: str
    type: Type
    line: int = 0


@dataclass(slots=True)
class FuncDef:
    name: str
    params: list
    ret: Optional[Type]
    body: "Block"
    line: int = 0

    @property
    def is_method(self):
        return bool(self.params) and self.params[0].name == "self"


@dataclass(slots=True)
class Module:
    structs: list = field(default_factory=list)
    funcs: list = field(default_factory=list)


# ---------------------------------------------------------------- 语句

@dataclass(slots=True)
class Block:
    stmts: list
    line: int = 0


@dataclass(slots=True)
class VarDecl:
    name: str
    ann: Optional[Type]
    init: object
    mutable: bool
    line: int = 0


@dataclass(slots=True)
class Assign:
    target: object
    value: object
    line: int = 0


@dataclass(slots=True)
class If:
    cond: object
    then: Block
    els: object = None
    line: int = 0


@dataclass(slots=True)
class While:
    cond: object
    body: Block
    line: int = 0


@dataclass(slots=True)
class Return:
    value: object = None
    line: int = 0


@dataclass(slots=True)
class Break:
    line: int = 0


@dataclass(slots=True)
class Continue:
    line: int = 0


@dataclass(slots=True)
class ExprStmt:
    expr: object
    line: int = 0


# ---------------------------------------------------------------- 表达式

@dataclass(slots=True)
class IntLit:
    value: int
    line: int = 0


@dataclass(slots=True)
class FloatLit:
    value: float
    line: int = 0


@dataclass(slots=True)
class BoolLit:
    value: bool
    line: int = 0


@dataclass(slots=True)
class StrLit:
    value: str          # 未转义的原始内容，直接透传给 C
    line: int = 0


@dataclass(slots=True)
class Ident:
    name: str
    line: int = 0


@dataclass(slots=True)
class Bin:
    op: str
    left: object
    right: object
    line: int = 0


@dataclass(slots=True)
class Un:
    op: str
    operand: object
    line: int = 0


@dataclass(slots=True)
class Call:
    callee: object      # 通常是 Ident
    args: list
    line: int = 0


@dataclass(slots=True)
class MethodCall:
    recv: object
    name: str
    args: list
    line: int = 0


@dataclass(slots=True)
class Field:
    obj: object
    name: str
    line: int = 0


@dataclass(slots=True)
class StructLit:
    name: Optional[str]     # None 表示靠声明类型补全（`{}`）
    inits: list             # [(fieldName, expr)]
    line: int = 0

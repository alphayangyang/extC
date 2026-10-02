#!/usr/bin/env python3
"""C-aware scanning helpers for the repository's own checkers.

Several checker scripts need the same three things: read C without being fooled by
comments or string literals, find struct members, and tell a write from a read. They used
to each carry their own copy; this module is the single one.

The stripping rule is the important part: comments and literals are replaced by spaces
(keeping newlines), so byte offsets and line numbers stay valid. Every earlier version of
these scanners that skipped this step produced false positives -- `e->func` inside a
comment, `a->top->used` inside a generated-C string literal, `d->name` where `d` was
codegen's own descriptor -- and those false positives were reported as real findings.

Note the deliberate difference from `tools/comment_neutral.py`, which answers a different
question ("did this commit change code?"): that one **keeps** literals, because two
revisions with different messages are still the same code only if the literals match. A
scanner that looks for *member accesses* must blank them instead, or a generated-C string
literal like `"a->top->used"` would be read as an access. Same file, two purposes, two
rules -- hence this module rather than a shared function.
"""
import re

WRITE_OPS = r"(?:=(?!=)|\+\+|--|\+=|-=|\|=|&=)"
WRITE = re.compile(r"\s*" + WRITE_OPS)


def strip(text: str) -> str:
    """Return `text` with comments and literals blanked out, offsets preserved."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if x == "\n" else " " for x in text[i:j]))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join("\n" if x == "\n" else " " for x in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def structs(code: str):
    """(name, {members}) for every struct definition in already-stripped `code`.

    Handles both `struct X { ... };` (the tag is the type) and `typedef struct { ... } X;`
    (the name follows the closing brace). The closing brace is found by matching the brace
    that opened the definition -- searching for a character sequence mis-matched it twice
    in an earlier scanner and silently attributed members to the wrong struct.
    """
    for m in re.finditer(r"\bstruct\s+([A-Za-z_]\w*)\s*\{|\btypedef\s+struct\s*(?:\w+\s*)?\{", code):
        open_brace = code.index("{", m.start())
        depth, i, n = 0, open_brace, len(code)
        while i < n:
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if m.group(1):
            name = m.group(1)
        else:
            tail = re.match(r"\s*([A-Za-z_]\w*)\s*;", code[i + 1:])
            if not tail:
                continue
            name = tail.group(1)
        body = code[open_brace + 1:i]
        members = set()
        flat = re.sub(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\([^;{]*?\)", r"* \1", body)
        for chunk in re.split(r"[,;]", flat):
            words = re.findall(r"[A-Za-z_]\w*", chunk)
            if not words:
                continue
            word = words[-1]
            if word in ("struct", "union", "enum", "const", "volatile", "unsigned", "signed"):
                continue
            members.add(word)
        yield name, members


def span_of_struct(code: str, start: int):
    """(open brace, close brace) of the struct definition whose `struct` keyword is at `start`."""
    open_brace = code.index("{", start)
    depth, i, n = 0, open_brace, len(code)
    while i < n:
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return open_brace, i


class Ratchet:
    """A baseline that may only shrink: every violation must be listed, and every listed
    entry that no longer violates is reported as stale and makes the run fail.

    The file format is `<key>\\t<reason>\\t<tracking>`, one per line, `#` for comments --
    the same shape the gate baselines use.
    """

    def __init__(self, path):
        self.path = path
        self.entries = {}
        try:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.rstrip("\n")
                    if not line or line.startswith("#"):
                        continue
                    parts = line.split("\t")
                    self.entries[parts[0]] = parts[1] if len(parts) > 1 else ""
        except FileNotFoundError:
            pass

    def check(self, violations, label):
        """`violations` is {key: detail}. Print the verdict and return an exit code."""
        new = sorted(k for k in violations if k not in self.entries)
        stale = sorted(k for k in self.entries if k not in violations)
        known = len(violations) - len(new)
        print("[%s] 违规 %d（已知 %d / 新增 %d），基线 %d，过期 %d"
              % (label, len(violations), known, len(new), len(self.entries), len(stale)))
        for k in new:
            print("  NEW   %s\n          %s" % (k, violations[k]))
        for k in stale:
            print("  STALE %s  —— 已不再违规，请从基线删除（棘轮只往一个方向转）" % k)
        if new or stale:
            print("[%s] FAIL" % label)
            return 1
        print("[%s] ok（全部违规都在基线里）" % label)
        return 0

    def write(self, violations, reason):
        import os
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as f:
            for k in sorted(violations):
                f.write("%s\t%s\t%s\n" % (k, violations[k], reason))
        print("wrote %s (%d entries)" % (self.path, len(violations)))

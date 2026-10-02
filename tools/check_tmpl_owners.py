#!/usr/bin/env python3
"""Static attribution for a field name that several structs share: `tmpl`.

Why this exists
---------------
A grep for `->tmpl` finds uses, not owners.  A member with that name is declared by
two structs today (`FuncDef` and `CallCheck`), so a rename or a move by field name
alone catches the wrong one -- measured twice in this repository (a rename pass
rewrote a `CallCheck` site into a `FuncDef` one and broke the build; a hand-written
table first named `Type.tmpl`, which does not exist at all).

What it does
------------
1. Reads every struct definition under `src/` and records, per struct, the members it
   declares.  This is the only source of truth for "which structs declare `tmpl`".
2. Finds every use of the field, written `base.tmpl` or `base->tmpl`, in the files
   given on the command line (default: all of `src/`).
3. Finds the enclosing function of each use and decides, textually, which struct the
   base names: a declared local or parameter, an assignment under a `(Struct *)`
   cast, or a call whose return type is that struct.  See `classify()`; anything it
   cannot decide is printed as UNKNOWN rather than guessed.

Two modes:
  report (default)  print the per-use table, the per-owner counts and the coverage
                    line; exit 1 if any use is undecided.
  --verify          the same walk, but only the conclusions that have to hold at all
                    times: no direct field access in code generation (it must go
                    through `planTemplate`), and no undecided use anywhere.  One ok
                    line, for `check.sh`.

Where the field still lives: `FuncDef.tmpl` is the instance -> template back pointer,
written by instance materialization.  It is one of the fields that cannot move to the
plan side table on its own -- moving it is a storage change, while the real work is
making the instance set explicit.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIELD = "tmpl"

# Where each struct is expected to live.  Used only to report, never to decide.
EXPECTED = {"FuncDef": "ast.h", "CallCheck": "check_internal.h", "Checker": "check_internal.h"}

# Code generation reaches the field only through this accessor.
ACCESSOR = "planTemplate"


def strip_comments(text: str) -> str:
    """Remove comments and string/char literals, keeping offsets and line numbers."""
    out = []
    i, n = 0, len(text)
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


def struct_members(text: str):
    """Yield (struct name, {members}, declaration line) for every struct definition.

    A struct's own name is the tag of a definition that is **not** a typedef: the typedef
    form (`typedef struct X { ... } X;`) names the type after the closing brace, and a
    rename of a member never has to worry about it.  The closing brace is found by
    matching the brace that opened the definition, not by searching for a character
    sequence -- two earlier versions of this scan mis-matched the brace and silently
    attributed members to the wrong struct.
    """
    code = strip_comments(text)
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
        tag = m.group(1)
        if tag:
            name = tag                       # `struct X { ... };` -- X is the type
        else:
            tail = re.match(r"\s*([A-Za-z_]\w*)\s*;", code[i + 1:])
            if not tail:
                continue                     # `typedef struct { ... } ...;` without a name
            name = tail.group(1)             # `typedef struct { ... } X;` -- X is the type
        body = code[open_brace + 1:i]
        members = set()
        # Function pointers first: `void (*cb)(void)` -> `void *cb`, then the generic rule:
        # in a declarator the declared name is the last identifier before `,` or `;`.
        flat = re.sub(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\([^;{]*?\)", r"* \1", body)
        for chunk in re.split(r"[,;]", flat):
            names = re.findall(r"[A-Za-z_]\w*", chunk)
            if not names:
                continue
            word = names[-1]
            if word in ("struct", "union", "enum", "const", "volatile", "unsigned", "signed"):
                continue
            members.add(word)
        line = code[:m.start()].count("\n") + 1
        yield name, members, line


def enclosing_function(code: str, pos: int):
    """Return (name, body_start, body_end) of the function containing `pos`, or None."""
    best = None
    for m in re.finditer(r"^([A-Za-z_][\w \t*]*?)\b([A-Za-z_]\w*)\s*\([^;{]*\)[ \t\n]*\{", code, re.M):
        start = m.end() - 1
        depth, i, n = 0, start, len(code)
        while i < n:
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if start <= pos <= i and (best is None or start > best[1]):
            best = (m.group(2), start, i)
    return best


def declaration_spans(code: str):
    """Byte ranges that declare the field rather than use it: struct bodies and prototypes.

    A member declaration in a header is not a use, and neither is a function prototype;
    counting them as uses is how a field's owner gets double-counted by hand.
    """
    spans = []
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
        spans.append((open_brace, i))
    for m in re.finditer(r"^([A-Za-z_]\w*)\s+\**\s*([A-Za-z_]\w*)\s*\([^;{]*\)\s*;", code, re.M):
        if m.group(1) in ("typedef", "return", "sizeof"):
            continue
        spans.append((m.start(), m.end()))
    return spans


def inside(pos: int, spans):
    return any(a <= pos <= b for a, b in spans)


def function_params(code: str, fn_start: int):
    """Text between the parentheses of the definition whose body starts at `fn_start`.

    Only a short window before the brace is searched: searching further back reaches the
    previous statement's call, which silently made every parameter a struct type.
    """
    window_start = max(0, fn_start - 600)
    open_paren = code.rfind("(", window_start, fn_start)
    if open_paren < 0:
        return ""
    depth, i = 0, open_paren
    while i < fn_start:
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return code[open_paren:i]


def local_types(code: str, fn_start: int, fn_end: int, structs):
    """Map every local/parameter name in this function to the struct type it holds.

    Declarations are textual: `Struct *name`, `Struct **name`, or an assignment whose
    right-hand side carries a `(Struct *)` cast.  Scoping is ignored -- a name is taken
    to have one type per function, which is true for this codebase and is asserted by
    the report: a name mapped to two structs is printed as AMBIGUOUS.
    """
    body = code[fn_start:fn_end]
    types = {}
    for sn in structs:
        for m in re.finditer(r"\b%s\s+\*+\s*\**\s*([A-Za-z_]\w*)\b" % re.escape(sn), body):
            types.setdefault(m.group(1), {})[sn] = "declared"
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*=([^;]*?)(?:;|$)", body, re.S):
            name, rhs = m.group(1), m.group(2)
            if re.search(r"\(\s*%s\s*\*+\s*\)" % re.escape(sn), rhs):
                types.setdefault(name, {})[sn] = "cast"
        for m in re.finditer(r"\*\s*([A-Za-z_]\w*)\s*=\s*\*\s*\(\s*%s\s*\*+\s*\*+\s*\)" % re.escape(sn), body):
            types.setdefault(m.group(1), {})[sn] = "element of %s**" % sn
    params = function_params(code, fn_start)
    for sn in structs:
        for m in re.finditer(r"\b%s\s+\*+\s*\**\s*([A-Za-z_]\w*)\b" % re.escape(sn), params):
            types.setdefault(m.group(1), {})[sn] = "parameter"
    return types


def classify(code: str, fn_start: int, fn_end: int, base: str, structs, types):
    """Decide which struct `base` names inside the enclosing function.

    Rules, in order (textual, no guessing):
      R1  the parameter list or a local declaration says `Struct *base`
      R2  the statement that assigns `base` carries a `(Struct *)` cast
      R3  `base = <call>()` where that call's return type is a struct name
      R4  a bare parameter named `base` (`FuncDef *base` in a signature)
    `base` means the identifier a use is written on: `x.tmpl`, `x->tmpl`, or the
    parameter `tmpl` itself.  A use that no rule decides is reported as UNKNOWN --
    the point of this script is to be honest about what it cannot attribute.
    """
    found = types.get(base)
    if found and len(found) == 1:
        sn, how = next(iter(found.items()))
        return sn, "%s (local/parameter type map)" % how
    if found:
        return None, "AMBIGUOUS: %s" % ", ".join(sorted(found))
    if base == FIELD:
        # A bare parameter with the field's own name (`FuncDef *tmpl`).  It counts as a
        # use of that struct's field: the question "which struct does this belong to" is
        # answered by the signature, which is exactly the trap this script exists for.
        params = function_params(code, fn_start)
        owners = [sn for sn in structs
                  if re.search(r"\b%s\s+\*+\s*\**\s*%s\b" % (re.escape(sn), re.escape(base)), params)]
        if len(owners) == 1:
            return owners[0], "bare parameter of this function"
        if len(owners) > 1:
            return None, "AMBIGUOUS: parameter could be %s" % ", ".join(sorted(owners))
    body = code[fn_start:fn_end]
    for m in re.finditer(r"\b%s\s*=\s*([A-Za-z_]\w*)\s*\(" % re.escape(base), body):
        ret = function_return_type(code, m.group(1))
        if ret in structs:
            return ret, "assigned from %s(), which returns %s" % (m.group(1), ret)
    return None, "no declaration or returning call found in this function"


def function_return_type(code: str, fname: str):
    m = re.search(r"^\s*(?:static\s+)?([A-Za-z_]\w*)\s*\**\s*%s\s*\(" % re.escape(fname), code, re.M)
    if m:
        return m.group(1)
    m = re.search(r"^\s*%s\s+([A-Za-z_]\w*)\s*\(" % re.escape(fname), code, re.M)
    return m.group(1) if m else None


def collect(files, structs):
    """Walk `files` and return (uses, unknown, coverage, codegen_direct)."""
    uses, unknown, coverage, codegen_direct = [], [], [], []
    types_cache = {}
    for path in files:
        p = pathlib.Path(path)
        raw = p.read_text(encoding="utf-8")
        code = strip_comments(raw)
        raw_tokens = len(re.findall(r"\b%s\b" % FIELD, raw))
        spans = declaration_spans(code)
        seen = set()
        # One pattern, one token: the identifier the field is read from.  It also covers
        # `x->tmpl->y` (one site for `x`, one for the field used as a base) without a
        # second pattern, which is what made an earlier version report phantom sites.
        pattern = r"([A-Za-z_]\w*)\s*(?:->|\.)\s*%s\b" % FIELD
        for m in re.finditer(pattern, code):
            pos = m.start(1)
            if inside(pos, spans):
                continue      # a member declaration or a prototype, not a use
            if pos in seen:
                continue
            seen.add(pos)
            base = m.group(1)
            line = code[:pos].count("\n") + 1
            if p.name == "codegen.c":
                codegen_direct.append((line, base))
            fn = enclosing_function(code, pos)
            if not fn:
                unknown.append((p.name, line, base, "outside any function"))
                continue
            owner, why = classify(code, fn[1], fn[2], base, structs, types_cache.setdefault(
                (p.name, fn[1]), local_types(code, fn[1], fn[2], structs)))
            tail = code[m.end():m.end() + 40]
            kind = "write" if re.match(r"\s*(?:=(?!=)|\+\+|--|\+=|-=|\|=|&=)", tail) else "read"
            uses.append((p.name, line, fn[0], base, owner or "UNKNOWN", kind, why))
        coverage.append((p.name, raw_tokens, len(seen)))
    return uses, unknown, coverage, codegen_direct


def verify(uses, unknown, codegen_direct) -> int:
    """Print one ok/bad line for `check.sh` and return the exit status."""
    bad = []
    if codegen_direct:
        bad.append("codegen reads `%s` directly at %s (use %s())"
                   % (FIELD, ", ".join("codegen.c:%d" % line for line, _ in codegen_direct), ACCESSOR))
    if unknown:
        bad.append("undecided uses: %s"
                   % ", ".join("%s:%d %s" % u for u in unknown))
    undecided = [u for u in uses if u[4] == "UNKNOWN"]
    if undecided:
        bad.append("unattributed uses: %s"
                   % ", ".join("%s:%d %s" % (u[0], u[1], u[3]) for u in undecided))
    owners = {}
    for _, _, _, _, owner, kind, _ in uses:
        owners.setdefault(owner, [0, 0])[0 if kind == "read" else 1] += 1
    summary = ", ".join("%s %dr/%dw" % (o, v[0], v[1]) for o, v in sorted(owners.items()))
    if bad:
        for b in bad:
            print("[tmpl-owners] bad: %s" % b)
        return 1
    print("[tmpl-owners] ok: every use attributed (%d sites: %s); codegen reads through %s only"
          % (len(uses), summary, ACCESSOR))
    return 0


def main() -> int:
    argv = sys.argv[1:]
    mode_verify = "--verify" in argv
    files = [a for a in argv if not a.startswith("--")] or \
            sorted(str(p) for p in (ROOT / "src").glob("*.c")) + \
            sorted(str(p) for p in (ROOT / "src").glob("*.h"))
    structs = {}
    for p in sorted((ROOT / "src").glob("*.h")):
        text = p.read_text(encoding="utf-8")
        for name, members, line in struct_members(text):
            if FIELD in members:
                structs[name] = (p.name, line) if name not in structs else structs[name]

    uses, unknown, coverage, codegen_direct = collect(files, structs)
    if mode_verify:
        return verify(uses, unknown, codegen_direct)

    print("structs declaring `%s`:" % FIELD)
    for name, (fname, line) in sorted(structs.items()):
        mark = "" if EXPECTED.get(name) == fname else "   <-- not the expected file"
        print("  %-12s %s:%d%s" % (name, fname, line, mark))

    print("\nuses of `%s` (%d sites):" % (FIELD, len(uses)))
    per_owner = {}
    for fname, line, fn, base, owner, kind, why in sorted(uses):
        per_owner.setdefault(owner, []).append((fname, line, fn, base, kind, why))
        flag = "" if owner != "UNKNOWN" else "   <-- UNKNOWN"
        print("  %-16s:%-5d %-28s %-8s %-6s %s%s" % (fname, line, fn, base + "." + FIELD, kind, owner, flag))
    print("\nper owner:")
    for owner in sorted(per_owner):
        kinds = {}
        for _, _, _, _, kind, _ in per_owner[owner]:
            kinds[kind] = kinds.get(kind, 0) + 1
        print("  %-12s %d uses (%s)" % (owner, len(per_owner[owner]),
                                        ", ".join("%s %d" % kv for kv in sorted(kinds.items()))))
    print("\ncoverage (raw `%s` tokens vs classified sites per file):" % FIELD)
    for fname, raw_tokens, sites in coverage:
        print("  %-16s tokens=%-4d sites=%d" % (fname, raw_tokens, sites))
    if unknown:
        print("\nUNKNOWN bases: %d" % len(unknown))
        for fname, line, base, why in unknown:
            print("  %s:%d %s (%s)" % (fname, line, base, why))
    return 1 if unknown else 0


if __name__ == "__main__":
    sys.exit(main())

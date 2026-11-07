#!/usr/bin/env python3
"""Gate: a question about a *call* must go through the shared predicates, not a local kind list.

Why this exists:
  An operator that resolved to a method has the same shape as a method call: `planCallee` points
  at the operator method, the hidden arena/zone arguments live on the same node, and codegen emits
  the same call. So "walk the callee" analyses have to treat `EX_CALL`, `EX_METHOD`, `EX_ASSOC`,
  `EX_BIN` and `EX_UN` alike. Twelve hand-written copies of that list existed, each one written
  independently; every bug of the form "the pool level / effect edge was right for a method and
  wrong for `a + b`" is one copy that forgot the operators.

  The shared predicates are `exprCarriesCallee` (call shapes plus callee-carrying operators and
  `new`) and `exprIsCallKind` (only the three call kinds, for "is the callee unresolved" questions),
  both in `src/check/check_top.c`. This gate refuses a new local list: any non-`case` line that
  names `EX_CALL` together with one of the other call kinds has to use a predicate instead.

  `case` label lists are allowed: a `switch` that also wants a *different body* per kind is not
  asking the call question, and it is checked by `check_walkers.py` from the other side.

Usage:
  tools/check_call_sites.py           # report, exit 1 on a local list
  tools/check_call_sites.py --list    # print every occurrence with its verdict, exit 0
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_walkers   # strip_comments_and_strings: offsets preserved, so line numbers stay right

ROOT = pathlib.Path(__file__).resolve().parent.parent
CALL_KINDS = ("EX_CALL", "EX_METHOD", "EX_ASSOC", "EX_BIN", "EX_UN")
PREDICATES = ("exprCarriesCallee", "exprIsCallKind")

# A local list is a promise that the difference between the kinds matters here. State why; the
# entry is `file.c:function` -> reason, and the gate prints it with the line so the reason stays
# next to the decision.
ALLOW = {
    "check_stmt.c:checkStmt":
        "the home-arena lift distinguishes free/method calls (result may point into this frame) from "
        "associated calls (constructor result stays escapable); lifting for EX_ASSOC rejected "
        "`return buf<T>::withCap(n)` in tests/stl/buf.extc",
}

def predicate_spans(text: str) -> list:
    """Character spans of the two predicate definitions (their bodies may enumerate the kinds)."""
    spans = []
    for name in PREDICATES:
        m = re.search(r"\nbool " + name + r"\s*\([^;{]*\)\s*\{", text)
        if not m:
            continue
        i = m.end() - 1
        depth, j = 0, i
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        spans.append((i, j))
    return spans


def enclosing_function(text: str, pos: int):
    """Name of the function whose body contains `pos`, or None (column-0 definitions only)."""
    name = None
    for m in re.finditer(r"\n(?:static )?(?:EXTC_INLINE )?[A-Za-z_][A-Za-z0-9_ *]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{", text[:pos]):
        i = m.end() - 1
        depth, j = 0, i
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if i <= pos < j:
            name = m.group(1)
    return name


def main() -> int:
    listing = "--list" in sys.argv
    bad = 0
    seen = 0
    for path in sorted((ROOT / "src" / "check").glob("*.c")):
        text = check_walkers.strip_comments_and_strings(path.read_text())
        spans = predicate_spans(text)
        offset = 0
        for no, line in enumerate(text.split("\n"), 1):
            here = offset
            offset += len(line) + 1
            if any(a <= here < b for a, b in spans):
                continue
            hits = [k for k in CALL_KINDS if re.search(r"\b" + k + r"\b", line)]
            if "EX_CALL" not in hits or len(hits) < 2:
                continue
            seen += 1
            stripped = line.strip()
            if stripped.startswith("case "):
                verdict = "case list (allowed)"
            elif any(p in line for p in PREDICATES):
                verdict = "uses a shared predicate"
            elif (path.name + ":" + (enclosing_function(text, here) or "")) in ALLOW:
                verdict = "allowed local list"
            else:
                verdict = "LOCAL LIST"
                bad += 1
            if listing:
                print(f"  {path.name}:{no}  {verdict:24s} {stripped[:78]}")
    if listing:
        print(f"\n  {seen} line(s) name EX_CALL next to another call kind")
        return 0
    if bad:
        print(f"  {bad} local call-kind list(s) -- use `exprCarriesCallee` (call shapes + operators)")
        print("  or `exprIsCallKind` (only the three call kinds), declared in src/check/check_internal.h")
        return 1
    print(f"  every call-kind question goes through a shared predicate ✓ ({seen} line(s) checked)")
    return 0

if __name__ == "__main__":
    sys.exit(main())

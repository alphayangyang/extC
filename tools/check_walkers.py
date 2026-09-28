#!/usr/bin/env python3
"""Mechanical check: a hand-written `switch` that *walks* an AST node must handle every kind.

Why this exists (measured, not theoretical):
  The walkers that ask "what is inside this expression / statement" are hand-written
  `switch` statements that end in `default:`. Because of that `default:`, `-Wswitch` stays
  silent when a new `ExprKind` is added, so the new kind is skipped by **all** of them.
  That is exactly what happened twice:
    * `EX_SLICE`'s two bounds were skipped by eight walkers at once (check_top.c documents it);
    * `EX_DYN` (added with `dyn Trait`) was skipped by ten of them -- a parameter used only
      inside a payload was reported as never used, and calls inside a payload were invisible
      to the needsHome / allocator / effects / rewrite analyses.

How a walker is recognised:
  Structurally: a `switch (<x>->kind)` whose body calls the function it sits in again
  (a recursive walk). Classifiers (`case EX_NEW: case EX_GENCALL: return true;`) are not
  recursive and are deliberately partial, so they are not reported.

Usage:
  tools/check_walkers.py            # report, exit 1 when a walker misses a kind
  tools/check_walkers.py --list     # print every walker with its coverage, exit 0
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Kinds that carry no child expression/statement: a walker may skip them (it has nothing to
# descend into). Keep this list tight -- if a kind gains children, remove it here.
LEAF = {
    "EX_INT", "EX_FLOAT", "EX_BOOL", "EX_STR", "EX_IDENT", "EX_NULL",
    "ST_BREAK", "ST_CONTINUE",      # statements with nothing inside them
}
# Walkers that are known to be deliberately partial, with the reason. Adding an entry here is
# a promise that the omission is intentional -- state why.
ALLOW = {}
# Count of recursive, untagged kind-walkers. Bump this down as they are migrated, never up.
RATCHET = 23       # measured 2026-09-26 after migrating the `needsHome` pair as well.
                   # Every migration lowers this; the gate refuses to let it grow.   # 2026-09-26: the four depth walkers were closed; nothing is allowed any more 


def enum_kinds(header: str, name: str) -> list:
    """Kind names of `typedef enum { ... } <name>;` in the header."""
    # `[^{}]*` matters: with `.*?` the match starts at the *first* `typedef enum {` in the file
    # and spans everything up to `} StmtKind;`, so `StmtKind` came out as both enums (36 kinds)
    # and every statement walker was reported as missing all 26 expression kinds.
    # Strip comments **first**: an enum's comments may contain braces (one of `ExprKind`'s does),
    # and they must not decide where the enum ends.
    bare = re.sub(r"/\*.*?\*/", " ", header, flags=re.S)
    bare = re.sub(r"//[^\n]*", " ", bare)
    m = re.search(r"typedef enum\s*\{([^{}]*)\}\s*" + name + r"\s*;", bare, re.S)
    if not m:
        return []
    body = m.group(1)
    return re.findall(r"\b((?:EX|ST)_[A-Z0-9_]+)\b", body)


def enclosing_function(text: str, pos: int):
    """Name of the function definition whose body contains `pos`, or None.

    A definition is a line starting in column 0 that carries a `(` and does not end in `;`
    (a prototype); the name is the identifier just before that `(`. Scanning for "the last
    identifier followed by (" instead picked up call lines and mis-attributed the switch.
    """
    name = None
    for line in text[:pos].split("\n"):
        if not line or line[0].isspace() or "(" not in line:
            continue
        stripped = line.rstrip()
        if stripped.endswith(";") or stripped.endswith(")") and "{" not in stripped:
            continue
        before = line[:line.index("(")].strip()
        m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)$", before)
        if m:
            name = m.group(1)
    return name


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and string/char literals, keeping every offset intact.

    Brace counting over raw text is wrong the moment a comment or a format string contains a
    brace, and the resulting block then swallowed the next switch (measured: a statement walker
    was reported as missing every expression kind)."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def switch_blocks(text: str):
    """Yield (pos_of_switch, block_text) for every `switch ( ... )` in the file."""
    for m in re.finditer(r"switch\s*\(", text):
        i = text.find("{", m.end())
        if i < 0:
            continue
        depth = 0
        j = i
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        yield m.start(), text[i:j + 1]


def main() -> int:
    listing = "--list" in sys.argv
    header = (ROOT / "src/ast.h").read_text(encoding="utf-8")
    kinds = {
        "EX": [k for k in enum_kinds(header, "ExprKind")],
        "ST": [k for k in enum_kinds(header, "StmtKind")],
    }
    bad = 0
    hand_written = 0        # recursive *and* untagged: the copies still to be migrated
    for path in sorted((ROOT / "src").glob("*.c")):
        raw = path.read_text(encoding="utf-8")
        text = strip_comments_and_strings(raw)
        for pos, block in switch_blocks(text):
            labels = set(re.findall(r"case\s+((?:EX|ST)_[A-Z0-9_]+)", block))
            if len(labels) < 3:
                continue                        # not a full walker
            first = re.search(r"case\s+((?:EX|ST)_[A-Z0-9_]+)", block).group(1)
            prefix = first[:2]                  # the enum this switch dispatches on
            # Offsets are preserved by the stripper, so scan the raw text: the tagged
            # definition starts with a comment, which is blanked out in `text`.
            fn = enclosing_function(raw, pos)
            recursive = bool(fn) and bool(re.search(r"\b" + re.escape(fn) + r"\s*\(", block))
            # A walker is either recursive by text (the hand-written copies) or tagged
            # `/*@@all-kinds*/` -- the authoritative child list in `ast.c` delegates to helpers,
            # so text alone cannot recognise it, and that list must be guarded too.
            # The tag lives in a comment, and `text` has comments blanked out, so look at the
            # raw file for it.
            tagged = "/*@@all-kinds*/" in raw[max(0, pos - 300):pos + 300]
            if not recursive and not tagged:
                continue                        # a classifier, not a walker
            missing = [k for k in kinds[prefix] if k not in labels and k not in LEAF]
            line = text[:pos].count("\n") + 1
            tag = f"{path.name}:{fn}"
            loc = f"{path.name}:{line} {fn}"
            # Only a **wide** walker (one that already covers most kinds) is expected to be
            # complete. A narrow predicate -- `isPlaceExpr`, `exprMayPrint`, ... -- walks a few
            # shapes on purpose, and is reported as information, not as a defect.
            # "Wide" is relative to the enum: a statement walker legitimately has ten cases.
            wide = len(labels) * 10 >= len(kinds[prefix]) * 6
            if missing and not wide:
                if listing:
                    print(f"  narrow   {loc}  ({len(labels)} kinds, skips {len(missing)})")
                continue
            if recursive and not tagged:
                hand_written += 1
            if missing:
                if tag in ALLOW:
                    print(f"  allowed  {loc}  (missing {', '.join(missing)}) -- {ALLOW[tag]}")
                    continue
                print(f"  MISSING  {loc}  -> {', '.join(missing)}")
                bad += 1
            elif listing:
                print(f"  ok       {loc}  ({len(labels)} kinds)")
    print()
    # The ratchet: migrating a copy lowers this number, adding a new copy raises it. New copies are
    # exactly what this refactor exists to prevent, so the gate refuses to let the count grow.
    if hand_written > RATCHET:
        print(f"  {hand_written} hand-written walkers, budget is {RATCHET} -- migrate one instead of"
              f" adding another (docs/topics/AST-WALKERS.md)")
        bad += 1
    else:
        print(f"  hand-written walkers: {hand_written} (ratchet {RATCHET})")
    if bad:
        print(f"  {bad} walker(s) miss at least one kind -- add the case, or record the omission in"
              f" ALLOW with a reason ({pathlib.Path(__file__).name})")
        return 1
    print("  every recursive kind-walker handles every kind ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())

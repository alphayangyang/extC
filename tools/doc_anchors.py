#!/usr/bin/env python3
"""Document anchors: cite a symbol, not a line number.

A line number breaks on reformat and rots on every edit above it. Docs that must stay
accurate cite src/back/reclaim.c:extc_rc_sweepWord instead of src/back/reclaim.c:3637.

Three classes, because the docs are in three different states:
  STRICT   must cite symbols only (checked, no numeric anchors allowed).
  RATCHET  numeric anchors may not grow past tools/anchor-baseline.tsv; every symbol must
           resolve. New text cites symbols; old lines migrate section by section, as the
           section is edited anyway.
  FROZEN   a snapshot pinned to an old tree (the header states the md5 and line count). Its
           numbers are part of the record, so they are left alone.

Why not convert everything at once: a stale numeric anchor maps to whatever symbol happens
to sit at that line today, so a blind rewrite turns a stale number into a wrong symbol, and a
wrong symbol is worse than an honest stale number. Only the STRICT docs (whose anchors were
re-verified against this tree) are rewritten automatically.

  python3 tools/doc_anchors.py --check       STRICT: 0 numeric anchors; STRICT+RATCHET: symbols resolve, ratchet holds
  python3 tools/doc_anchors.py --ratchet     only the ratchet half of --check
  python3 tools/doc_anchors.py --ratchet --write   refresh the baseline (refuses to raise it)
  python3 tools/doc_anchors.py --convert     rewrite numeric anchors in the STRICT docs
  python3 tools/doc_anchors.py --selftest    prove the checker has teeth
  python3 tools/doc_anchors.py --census      freshness census over the living docs
  python3 tools/doc_anchors.py --map reclaim.c:3637    what symbol owns this line today
"""
import argparse
import source_split
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DQ = chr(34)
AP = chr(39)
BQ = chr(96)
NL = chr(10)
BS = chr(92)

# Three classes, because the docs are in three different states (see the module docstring): strict = cite symbols only; ratchet = numeric anchors may not grow and symbols must resolve; frozen = pinned to an old tree, numbers stay.
STRICT = [
    "docs/design/RECLAMATION.md",
    "docs/usage/reclamation.md",
    "docs/usage/modules-cabi.md",
]
RATCHET = [
    "docs/analysis/MEMORY-MODEL-CURRENT.md",
    "docs/analysis/GC-ROOFLINE.md",
    "docs/analysis/ARENA-CONTAINERS.md",
    "docs/analysis/CORPUS-TORTURE.md",
    "docs/analysis/CONVERGED-MODEL-PROTO.md",
    "docs/design/EFFECTS.md",
    "docs/design/POOL-LEVELS.md",
]
FROZEN = [
    "docs/analysis/CALIBRATION.md",
    "docs/analysis/ALLOC-COST.md",
    "docs/analysis/WMARK-REACH.md",
    "docs/analysis/UNLINK-DEFERRAL.md",
    "docs/analysis/WIN-MINGW.md",
    "docs/analysis/T2-CAPACITY-AND-T1-WIDENING.md",
]
LIVING = STRICT + RATCHET + FROZEN

# A cite starts at a word boundary: never swallow the tail of the surrounding C name.
DASHES = "[-/\u2013\u2014、]"
SHORT_SPAN = re.compile(r"^:[0-9]+(?:\s*" + DASHES + r"\s*[0-9]+)*$")
NUM_ANCHOR = re.compile(r"(?<![A-Za-z0-9_.])([A-Za-z0-9_/.-]+" + BS + r".(?:c|h)):([0-9]+(?:\s*" + DASHES + r"\s*[0-9]+)*)")
SYM_ANCHOR = re.compile("([A-Za-z0-9_/.-]+" + BS + ".(?:c|h)):([A-Za-z_][A-Za-z0-9_:-]*(?:" + BS + "." + BS + "." + "[A-Za-z_][A-Za-z0-9_:-]*)?)")
DEFINE = re.compile("^" + BS + "s*#" + BS + "s*define" + BS + "s+([A-Za-z_][A-Za-z0-9_]*)")
STRUCT = re.compile("^(?:typedef" + BS + "s+)?(?:struct|enum|union)" + BS + "s+([A-Za-z_][A-Za-z0-9_]*)")
ANON_AGG = re.compile("^(?:typedef" + BS + "s+)?(?:struct|enum|union)" + BS + "s*" + BS + "{$")
TAIL_NAME = re.compile("^(?:}" + BS + "s*([A-Za-z_][A-Za-z0-9_]*)" + BS + "s*;|([A-Za-z_][A-Za-z0-9_]*)" + BS + "s*;)$")
SKIP = re.compile("skip:[a-z0-9-]+")
DATA = re.compile("^(?:static" + BS + "s+)?(?:const" + BS + "s+)?[^=]*?" + BS + "b([A-Za-z_][A-Za-z0-9_]*)" + BS + "s*" + BS + "[[^]]*" + BS + "]" + BS + "s*=" + BS + "s*" + BS + "{$")
NAME_END = re.compile("([A-Za-z_][A-Za-z0-9_]*)" + BS + "s*$")
STR_LIT = re.compile(DQ + "([^" + DQ + BS + BS + "]|" + BS + BS + ".)*" + DQ)
CONTROL = {"if", "for", "while", "switch", "return", "sizeof", "else", "do", "case", "goto"}

SRC_FILES = []
CFILES = []
INDEX = {}


def code_view(line):
    s = line.strip()
    if s.startswith(DQ):
        s = s[1:]
    if s.endswith(DQ + ","):
        s = s[:-2]
    elif s.endswith(DQ):
        s = s[:-1]
    if s.endswith(BS + "n") or s.endswith(BS + "t"):
        s = s[:-2]
    if s.endswith(BS + BS):
        s = s[:-1]
    s = s.replace(BS + DQ, DQ)
    return s


def brace_counts(code_lines):
    """Brace delta per line, ignoring string literals and both comment forms.

    A // comment whose line ends in a backslash continues onto the next emitted
    line (the emitted text uses backslash continuations), so the flag carries.
    """
    out = []
    in_block = False
    in_line_cont = False
    for line in code_lines:
        s = line
        i = 0
        n = len(s)
        opens = 0
        closes = 0
        start_in_line = in_line_cont
        in_line_cont = False
        if in_block or start_in_line:
            pass
        in_str = False
        while i < n:
            c = s[i]
            if in_block:
                if c == "*" and i + 1 < n and s[i + 1] == "/":
                    in_block = False
                    i += 2
                    continue
                i += 1
                continue
            if start_in_line:
                break
            if in_str:
                if c == BS:
                    i += 2
                    continue
                if c == in_str:
                    in_str = False
                i += 1
                continue
            if c == DQ or c == AP:
                in_str = c
                i += 1
                continue
            if c == "/" and i + 1 < n:
                if s[i + 1] == "*":
                    in_block = True
                    i += 2
                    continue
                if s[i + 1] == "/":
                    if s.rstrip().endswith(BS):
                        in_line_cont = True
                    break
            if c == "{":
                opens += 1
            elif c == "}":
                closes += 1
            i += 1
        out.append((opens, closes))
    return out


def strip_prefix(head):
    s = head.lstrip()
    guard = 0
    while s.startswith("__attribute__") and guard < 4:
        guard += 1
        i = s.find("(")
        if i < 0:
            return s
        d = 0
        j = i
        while j < len(s):
            if s[j] == "(":
                d += 1
            elif s[j] == ")":
                d -= 1
                if d == 0:
                    break
            j += 1
        s = s[j + 1:].lstrip()
    return s


def func_name(code):
    s = code.strip()
    if not s.endswith("{"):
        return None
    head = s[:-1].strip()
    if not head.endswith(")"):
        return None
    if "=" in head:
        return None
    d = 0
    j = len(head) - 1
    while j >= 0:
        if head[j] == ")":
            d += 1
        elif head[j] == "(":
            d -= 1
            if d == 0:
                break
        j -= 1
    if j <= 0:
        return None
    m = NAME_END.search(head[:j])
    if not m:
        return None
    name = m.group(1)
    if name in CONTROL:
        return None
    rest = strip_prefix(head[:m.start()])
    if not rest.strip():
        return None
    return name


class Index:
    def __init__(self, rel):
        self.rel = rel
        source_split.ROOT = ROOT
        self.raw = source_split.read(rel).splitlines()
        self.code = [code_view(l) for l in self.raw]
        self.deltas = brace_counts(self.code)
        self.tokens = set(["file-scope"])
        self.code_names = set()
        starts = {}
        for i, c in enumerate(self.code):
            n = func_name(c)
            if n is None and c.endswith(")") and i + 1 < len(self.code) and self.code[i + 1].strip() == "{":
                n = func_name(c + " {")
            if n is None and c.rstrip().endswith("{"):
                # A signature may wrap: join upwards until the parameter list balances.
                joined = c
                j = i - 1
                while n is None and j >= 0 and i - j <= 8:
                    joined = self.code[j] + " " + joined
                    n = func_name(joined)
                    j -= 1
            if n:
                starts[i] = n
                self.tokens.add(n)
                self.code_names.add(n)
            m = DEFINE.match(c)
            if m:
                self.tokens.add(m.group(1))
            m = STRUCT.match(c)
            if m:
                self.tokens.add(m.group(1))
        for l in self.raw:
            for m in SKIP.finditer(l):
                self.tokens.add(m.group(0))
        # Function spans: a definition owns every line up to the next definition. Closing a
        # body by counting braces does not work on the emitted text, because it carries
        # mutually exclusive #if arms: the raw brace balance of the whole file can be
        # non-zero while every variant compiles.
        self.struct_spans = []
        for i, c in enumerate(self.code):
            if ANON_AGG.match(c) and i not in starts:
                depth = 0
                end = i
                for j in range(i, min(i + 4000, len(self.deltas))):
                    depth += self.deltas[j][0] - self.deltas[j][1]
                    end = j
                    if depth <= 0:
                        break
                nm = None
                if end > i:
                    t = TAIL_NAME.match(self.code[end].strip())
                    if t:
                        nm = t.group(1) or t.group(2)
                if nm:
                    self.tokens.add(nm)
                    self.code_names.add(nm)
                    self.struct_spans.append((i, end, nm))
            m = STRUCT.match(c)
            if not m or i in starts:
                continue
            nm = m.group(1)
            if nm in CONTROL or nm in ("file",):
                continue
            self.tokens.add(nm)
            self.code_names.add(nm)
            depth = 0
            end = i
            for j in range(i, min(i + 400, len(self.deltas))):
                depth += self.deltas[j][0] - self.deltas[j][1]
                end = j
                if depth <= 0:
                    break
            if end > i:
                self.struct_spans.append((i, end, nm))
        for i, c in enumerate(self.code):
            if i in starts:
                continue
            m = DATA.match(c)
            if m and m.group(1) not in CONTROL:
                starts[i] = m.group(1)
                self.tokens.add(m.group(1))
                self.code_names.add(m.group(1))
        order = sorted(starts)
        self.spans = []
        for k, i in enumerate(order):
            end = order[k + 1] - 1 if k + 1 < len(order) else len(self.code) - 1
            self.spans.append((i, end, starts[i]))
        self.starts = order

    def token_at(self, line):
        if line < 1 or line > len(self.raw):
            return None
        i = line - 1
        m = SKIP.search(self.raw[i])
        if m:
            return m.group(0)
        m = DEFINE.match(self.code[i])
        if m:
            return m.group(1)
        sbest = None
        for (a, b, name) in self.struct_spans:
            if a <= i <= b:
                if sbest is None or a > sbest[0]:
                    sbest = (a, b, name)
        if sbest is not None:
            return sbest[2]
        best = None
        for (a, b, name) in self.spans:
            if a <= i <= b:
                if best is None or a > best[0]:
                    best = (a, b, name)
        if best is not None:
            return best[2]
        return "file-scope"

    def spans_line(self, start):
        for (a, b, name) in self.spans:
            if a == start:
                return name
        return "file-scope"


def src_files():
    out = []
    for dirpath, _d, names in os.walk(os.path.join(ROOT, "src")):
        for n in names:
            if n.endswith(".c") or n.endswith(".h"):
                out.append(os.path.relpath(os.path.join(dirpath, n), ROOT))
    return sorted(out)


def all_c_files():
    out = []
    for dirpath, dirnames, names in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in (".git", "node_modules", "paper")
                       and not d.startswith("build")]
        for n in names:
            if n.endswith(".c") or n.endswith(".h"):
                out.append(os.path.relpath(os.path.join(dirpath, n), ROOT))
    return sorted(out)


def resolve(path, doc_rel=None):
    """Docs sit next to their evidence, so a doc-relative path wins over the repo root."""
    p = path.lstrip("./")
    cands = []
    if doc_rel:
        cands.append(os.path.normpath(os.path.join(os.path.dirname(doc_rel), p)))
    cands.append(p)
    for c in cands:
        if os.path.exists(os.path.join(ROOT, c)):
            return c
    base = os.path.basename(p)
    hits = [f for f in CFILES if os.path.basename(f) == base]
    if len(hits) == 1:
        return hits[0]
    return None


def index_of(rel):
    if rel not in INDEX:
        INDEX[rel] = Index(rel)
    return INDEX[rel]


def scan_lines(text):
    """Yield (line, in_fence). Fenced blocks quote commands and compiler output, not anchors."""
    fence = False
    for line in text.split(NL):
        s = line.lstrip()
        if s.startswith("~~~") or s.startswith(BQ * 3):
            fence = not fence
            yield line, True
            continue
        yield line, fence


def conv_nums(idx, group):
    """Map every line number in a compound cite (120-140, 4686/4716) to a symbol."""
    toks = []
    for num in re.split(DASHES, group):
        num = num.strip()
        if not num:
            continue
        t = idx.token_at(int(num))
        if t is None:
            return None
        if t not in toks:
            toks.append(t)
    if not toks:
        return None
    if len(toks) > 2:
        toks = [toks[0], toks[-1]]
    return "..".join(toks)


def named_near(idx, line, pos):
    """The symbol the prose attaches to a cite, i.e. `extc_rc_zeroSmall`（reclaim.c:1503）.

    Only a name that ends within a few characters before the cite counts: that is the
    pattern where the line number has drifted but the prose still names the target.
    A name inside a longer expression, or one further away, is not evidence.
    """
    best = None
    for name in idx.code_names:
        if len(name) < 5:
            continue
        for m in re.finditer(re.escape(name) + r"(?![A-Za-z0-9_])", line):
            gap = pos - m.end()
            if gap < 0 or gap > 40:
                continue
            between = line[m.end():pos]
            # Either the name sits right against the cite, or the cite follows a call
            # shape name(args): both attach the cite to that name. Anything else is prose.
            if gap > 8 and not between.startswith("("):
                continue
            if best is None or gap < best[0]:
                best = (gap, name)
    return None if best is None else best[1]


def convert_text(text, doc_rel=None):
    rep = {"ok": 0, "unresolved": []}

    def shorthand(line, path, idx):
        """Rewrite a bare ":120-140" code span that follows a full cite in the same line."""
        m0 = NUM_ANCHOR.search(line)
        if m0 is None:
            return line
        segs = line.split(BQ)
        start = 0
        for k, span in enumerate(segs):
            here = start
            start += len(span) + 1
            if k % 2 == 0 or here <= m0.end():
                continue
            if not SHORT_SPAN.match(span):
                continue
            named = named_near(idx, line, here)
            if named is not None:
                segs[k] = path + ":" + named
                rep["ok"] += 1
                continue
            toks = []
            bad = False
            for piece in span[1:].split("、"):
                c = conv_nums(idx, piece)
                if c is None:
                    bad = True
                    break
                if c not in toks:
                    toks.append(c)
            if bad or not toks:
                continue
            if len(toks) > 2:
                toks = [toks[0], toks[-1]]
            segs[k] = path + ":" + "..".join(toks)
            rep["ok"] += 1
        return BQ.join(segs)

    out = []
    for line, fenced in scan_lines(text):
        if fenced:
            out.append(line)
            continue
        m0 = NUM_ANCHOR.search(line)
        if m0 is not None:
            path0 = m0.group(1)
            rel0 = resolve(path0, doc_rel)
            if rel0 is not None:
                line = shorthand(line, path0, index_of(rel0))
        parts = []
        pos = 0
        for m in NUM_ANCHOR.finditer(line):
            parts.append(line[pos:m.start()])
            path, group = m.group(1), m.group(2)
            rel = resolve(path, doc_rel)
            idx = index_of(rel) if rel is not None else None
            conv = conv_nums(idx, group) if idx is not None else None
            named = named_near(idx, line, m.start()) if conv is not None else None
            if named is not None and ".." not in conv and named != conv:
                conv = named
            if conv is None:
                rep["unresolved"].append(m.group(0))
                parts.append(m.group(0))
            else:
                rep["ok"] += 1
                parts.append(path + ":" + conv)
            pos = m.end()
        parts.append(line[pos:])
        out.append("".join(parts))
    return NL.join(out), rep



def cmd_census():
    for f in SRC_FILES:
        idx = index_of(f)
        if not idx.spans:
            continue
        long_spans = [(a, b, n) for (a, b, n) in idx.spans if b - a > 4000]
        print("%-40s functions=%-5d spans=%-5d long=%d" % (f, len(idx.starts), len(idx.spans), len(long_spans)))
        for (a, b, n) in long_spans[:5]:
            print("    long span %s  %d..%d  (%d lines)" % (n, a + 1, b + 1, b - a + 1))
    return 0


def cmd_map(arg):
    path, _, line = arg.rpartition(":")
    rel = resolve(path)
    if rel is None:
        print("unresolved file: " + path)
        return 1
    idx = index_of(rel)
    tok = idx.token_at(int(line))
    print("%s:%s -> %s:%s" % (rel, line, path, tok))
    print("  " + idx.raw[int(line) - 1].strip()[:120])
    return 0

BASELINE = os.path.join(ROOT, "tools", "anchor-baseline.tsv")

def counts(rel):
    """(numeric anchors, symbolic anchors) left in one doc, fences excluded."""
    full = os.path.join(ROOT, rel)
    if not os.path.exists(full):
        return None
    with open(full, encoding="utf-8") as f:
        text = f.read()
    num = 0
    for line, fenced in scan_lines(text):
        if fenced:
            continue
        num += len(NUM_ANCHOR.findall(line))
    return (num, len(SYM_ANCHOR.findall(text)))

def load_baseline():
    rows = {}
    if not os.path.exists(BASELINE):
        return rows
    with open(BASELINE, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 2 or parts[0].startswith("#"):
                continue
            try:
                rows[parts[0]] = (int(parts[1]), int(parts[2]) if len(parts) > 2 else 0)
            except ValueError:
                continue
    return rows

def save_baseline(rows):
    out = ["# doc\tnumeric\tsymbolic"]
    for rel in RATCHET:
        num, sym = rows[rel]
        out.append(rel + "\t" + str(num) + "\t" + str(sym))
    with open(BASELINE, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

def cmd_ratchet(write, allow_raise):
    base = load_baseline()
    now = {}
    bad = 0
    for rel in RATCHET:
        c = counts(rel)
        if c is None:
            continue
        now[rel] = c
        if rel not in base:
            if not write:
                print("no baseline row: " + rel + "  (run --ratchet --write)")
                bad += 1
            continue
        limit = base[rel][0]
        if write and c[0] > limit and not allow_raise:
            print("refusing to raise " + rel + ": %d -> %d numeric anchor(s); fix the new cites or pass --allow-raise" % (limit, c[0]))
            bad += 1
            continue
        if c[0] > limit:
            print("numeric anchors grew: " + rel + "  %d -> %d  (cite symbols, not lines)" % (limit, c[0]))
            bad += 1
    if write and not bad and now:
        save_baseline(now)
        print("baseline written: " + os.path.relpath(BASELINE, ROOT))
        return 0
    if bad:
        print("FAIL: %d ratchet doc(s) regressed" % bad)
        return 1
    print("ok: no ratchet doc gained a numeric anchor")
    return 0

def cmd_convert():
    total = 0
    unresolved = []
    for rel in STRICT:
        full = os.path.join(ROOT, rel)
        if not os.path.exists(full):
            continue
        with open(full, encoding="utf-8") as f:
            text = f.read()
        new_text, rep = convert_text(text, rel)
        if new_text != text:
            with open(full, "w", encoding="utf-8") as f:
                f.write(new_text)
        total += rep["ok"]
        unresolved += [(rel, u) for u in rep["unresolved"]]
        print("%4d converted  %s" % (rep["ok"], rel))
    print("total converted: %d" % total)
    if unresolved:
        print("unresolved: %d" % len(unresolved))
        for rel, u in unresolved[:40]:
            print("  %s  %s" % (rel, u))
    print("note: --convert rewrites STRICT docs only. A RATCHET doc migrates section by section as it is")
    print("      edited, because a stale numeric anchor maps to whatever symbol sits at that line today.")
    return 0

def cmd_check():
    bad = 0
    for rel in STRICT + RATCHET:
        full = os.path.join(ROOT, rel)
        if not os.path.exists(full):
            continue
        with open(full, encoding="utf-8") as f:
            text = f.read()
        if rel in STRICT:
            for line, fenced in scan_lines(text):
                if fenced:
                    continue
                for m in NUM_ANCHOR.finditer(line):
                    print("numeric anchor in a symbol-only doc: %s  %s" % (rel, m.group(0)))
                    bad += 1
        for m in SYM_ANCHOR.finditer(text):
            path, toks = m.group(1), m.group(2)
            frel = resolve(path, rel)
            if frel is None:
                print("anchor names a missing file: %s  %s" % (rel, m.group(0)))
                bad += 1
                continue
            idx = index_of(frel)
            for tok in toks.split(".."):
                if tok not in idx.tokens:
                    print("anchor does not resolve: %s  %s  (no %s in %s)" % (rel, m.group(0), tok, frel))
                    bad += 1
    bad += cmd_ratchet(False, False)
    if bad:
        print("FAIL: %d bad anchor(s) across the living docs" % bad)
        return 1
    print("ok: symbol-only docs cite symbols, every symbol resolves, no ratchet doc gained a numeric anchor")
    return 0


def cmd_selftest():
    """Prove the checker has teeth, on this tree, without touching it."""
    bad = 0
    def want(cond, msg):
        nonlocal bad
        if not cond:
            print("FAIL " + msg)
            bad += 1
        else:
            print("ok   " + msg)

    # 1. a numeric cite converts to the symbol that owns that line today
    out, rep = convert_text("x reclaim.c:3637 y", "docs/design/RECLAMATION.md")
    want(rep["ok"] == 1 and "reclaim.c:" in out, "numeric cite converts")
    want(":3637" not in out, "the converted cite carries no line number")
    idx = index_of(resolve("reclaim.c", "docs/design/RECLAMATION.md"))
    want(idx.token_at(3637) in idx.tokens, "the mapped symbol resolves in its own file")

    # 2. an already-symbolic anchor is not re-matched as a numeric one
    want(len(NUM_ANCHOR.findall("reclaim.c:extc_rc_sweepWord")) == 0, "symbolic anchor is not numeric")
    want(len(NUM_ANCHOR.findall("reclaim.c:3637")) == 1, "numeric anchor is numeric")

    # 3. a cite inside a fence is ignored by both counters
    fenced = "~~~" + chr(10) + "reclaim.c:3637" + chr(10) + "~~~" + chr(10)
    n = sum(len(NUM_ANCHOR.findall(line)) for line, f in scan_lines(fenced) if not f)
    want(n == 0, "fenced cites are not counted")

    # 4. the ratchet fails when a doc gains a numeric anchor and passes at baseline
    real = load_baseline()
    global BASELINE
    keep = BASELINE
    try:
        BASELINE = os.path.join(ROOT, ".selftest-anchor-baseline.tsv")
        lowered = dict(real)
        for rel in list(lowered)[:1]:
            lowered[rel] = (0, lowered[rel][1])
        save_baseline(lowered)
        want(cmd_ratchet(False, False) != 0, "ratchet fails when a doc exceeds its baseline")
        want(cmd_ratchet(True, False) != 0, "rewrite refuses to raise a baseline")
        save_baseline(real)
        want(cmd_ratchet(False, False) == 0, "ratchet passes at the recorded baseline")
    finally:
        BASELINE = keep
        if os.path.exists(os.path.join(ROOT, ".selftest-anchor-baseline.tsv")):
            os.remove(os.path.join(ROOT, ".selftest-anchor-baseline.tsv"))

    if bad:
        print("FAIL: %d selftest assertion(s)" % bad)
        return 1
    print("ok: doc_anchors selftest")
    return 0
def main():
    global SRC_FILES, CFILES
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--convert", action="store_true")
    ap.add_argument("--census", action="store_true")
    ap.add_argument("--ratchet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--write", action="store_true", help="with --ratchet: refresh tools/anchor-baseline.tsv")
    ap.add_argument("--allow-raise", action="store_true", help="with --ratchet --write: allow the baseline to go up")
    ap.add_argument("--map")
    args = ap.parse_args()
    SRC_FILES = src_files()
    CFILES = all_c_files()
    if args.map:
        return cmd_map(args.map)
    if args.census:
        return cmd_census()
    if args.convert:
        return cmd_convert()
    if args.selftest:
        return cmd_selftest()
    if args.ratchet:
        return cmd_ratchet(args.write, args.allow_raise)
    return cmd_check()

if __name__ == "__main__":
    sys.exit(main())

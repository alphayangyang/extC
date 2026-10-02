#!/usr/bin/env python3
"""Gate 6 · instance plumbing: every emitted function calls the instance it should.

What it catches
---------------
A call site inside a generic body is **one AST node shared by every instance** of that
body, and the checker resolves it once per instance while walking the instantiation
fixpoint. With one pointer per node, only the last resolution survives (measured
2026-10-02): for two instances, both emitted functions call the *same* callee and the
other instance is never emitted at all. The generated C stays legal C when the two
instances happen to be interchangeable, so neither "does it compile" (gate 1) nor "does
it run clean" (gate 2) nor "same answer as C" (gate 3) can see it. This gate asks the
structural question instead: **which function does each emitted function call?**

Corpus and expectations
-----------------------
`tools/callsite-corpus/*.extc`, each with a `.expect` file next to it:

    # comment
    require <instance>            # this instance must be *emitted* (a definition exists)
    call <instance> <callee>      # this instance's body must call this callee

Instance names are the C names in the generated output (`wrap_i32`, `pick_meter`). The
expectation file is the judgement; it must be written from the intended semantics, not
from today's output -- otherwise the gate would bless the bug.

Ratchet
-------
The expectations are a to-do list: the corpus is red until the checker is fixed. The
baseline `tools/gate-callsite-known-bad.txt` lists the instances that may still be
wrong. A file whose expectations now hold must be removed from the baseline.

Usage
-----
    python3 tools/gate_callsite.py [--update-allowlist] [--verbose]
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatecommon as G

CORPUS = os.path.join(G.ROOT, "tools", "callsite-corpus")
ALLOW = os.path.join(G.ROOT, "tools", "gate-callsite-known-bad.txt")


def parse_expect(path):
    """Read a `.expect` file into (required instances, {caller: [callee]})."""
    require, calls = [], {}
    for raw in open(path, encoding="utf-8"):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == "require" and len(parts) == 2:
            require.append(parts[1])
        elif parts[0] == "call" and len(parts) == 3:
            calls.setdefault(parts[1], []).append(parts[2])
        else:
            raise SystemExit("%s: cannot parse `%s`" % (path, line))
    return require, calls


def function_bodies(text):
    """Map every top-level function definition to its body text.

    A definition is a line at column 0 that ends in `{`; the body runs to the matching
    `}` at column 0. Declarations (ending in `;`) and nested braces are skipped by that
    rule, which is what the emitted shape looks like.
    """
    bodies, lines = {}, text.split("\n")
    i = 0
    while i < len(lines):
        m = re.match(r"^[A-Za-z_][\w \t*]*?\**([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{\s*$", lines[i])
        if not m:
            i += 1
            continue
        name = m.group(1)
        depth, j, body = 1, i + 1, []
        while j < len(lines) and depth:
            depth += lines[j].count("{") - lines[j].count("}")
            if depth:
                body.append(lines[j])
            j += 1
        bodies[name] = "\n".join(body)
        i = j
    return bodies


def check_one(path):
    """Return (name, problems) for one corpus file."""
    name = G.rel(path)
    expect_path = path[:-len(".extc")] + ".expect"
    require, calls = parse_expect(expect_path)
    out_c = os.path.join(G.GATEDIR, "callsite", name.replace("/", "_") + ".c")
    ok, diag = G.gen_c(path, out_c, timeout=90)
    if not ok:
        return (name, ["extC rejected the probe: %s" % diag.strip().splitlines()[-1:]])
    text = open(out_c, encoding="utf-8", errors="replace").read()
    bodies = function_bodies(text)
    problems = []
    for inst in require:
        if inst not in bodies:
            problems.append("instance `%s` was never emitted" % inst)
    for caller, callees in sorted(calls.items()):
        body = bodies.get(caller)
        if body is None:
            problems.append("caller `%s` was never emitted" % caller)
            continue
        # Only real calls count: a name followed by `(`. Compare against the set of
        # known definitions so that `sizeof`-like noise cannot fake a match.
        found = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", body)) & set(bodies)
        for callee in callees:
            if callee not in found:
                wrong = sorted(n for n in found if n.startswith(callee.split("_")[0]))
                problems.append("`%s` does not call `%s`%s" % (
                    caller, callee,
                    " (it calls %s)" % ", ".join("`%s`" % w for w in wrong) if wrong else ""))
    return (name, problems)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-allowlist", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    G.ensure_gatedir()
    os.makedirs(os.path.join(G.GATEDIR, "callsite"), exist_ok=True)
    probes = sorted(p for p in os.listdir(CORPUS) if p.endswith(".extc"))
    if not probes:
        print("[callsite] no probes found in %s" % CORPUS)
        return 1
    failures = {}
    for p in probes:
        name, problems = check_one(os.path.join(CORPUS, p))
        if a.verbose and problems:
            print("  %s:" % name)
            for prob in problems:
                print("      %s" % prob)
        if problems:
            failures[name] = "\n".join(problems)
    allow = G.load_allow(ALLOW)
    if a.update_allowlist:
        G.write_allow(ALLOW, failures)
        return 0
    return G.report("callsite", failures, allow,
                    eligible={G.rel(os.path.join(CORPUS, p)) for p in probes})


if __name__ == "__main__":
    sys.exit(main())

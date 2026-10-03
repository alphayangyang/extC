#!/usr/bin/env python3
"""R3 · the include graph must follow the pipeline, not the other way round.

Why this rule exists (docs/topics/AST-DECOUPLING.md): `src/codegen.c` currently includes
`src/check_internal.h`, the checker's private header. Removing that one line produces 14
compile errors (`isProtoType`, `isOverloadableOp`, `findOperator`, `findMethod`, ...), so
what code generation actually depends on is a handful of **type-layer predicates that live
in the wrong header** -- and, through it, the checker's whole private state is reachable
from a later phase. That is the edge this rule makes visible.

The pipeline these rules encode:

    base < ast < types < plan < check < codegen  (a later file may not include an earlier
    phase's *private* header; only the published views are allowed)

Deliberate exceptions, each with a reason (and a baseline entry):
  - `plan.h` includes `ast.h`: the plan is keyed by nodes, so it needs the node types.
  - `parser.c` includes `plan.h`: the parser creates the `for` statement whose step the
    checker may replace; the plan owns that answer today. **This one is on the list to
    remove** (P2 of the plan: `forStep` becomes parser-owned syntax).
  - `check_dataflow.c` includes `check_internal.h`: it **is** a checker-side helper (its
    header `dataflow.h` sits at phase 4, next to `check_internal.h`), so this is an
    in-layer include, not a cross-phase one. It was called `dataflow.c`, and the name put
    it in the wrong layer, which is why the tool used to report it -- the file was renamed
    rather than the rule bent.

Usage
-----
    python3 tools/check_layering.py [--update-baseline]
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cscan

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "layering-known-bad.txt")

# Pipeline position. A file may include headers of its own layer or lower, plus the
# published views named below; including another phase's *private* header is a violation.
PHASE = {
    "base.h": 0, "ast.h": 1, "types.h": 2, "plan.h": 3,
    "check_internal.h": 4, "check.h": 4, "codegen.h": 5, "parser.h": 2, "lexer.h": 0,
    "modules.h": 5, "dataflow.h": 4, "check_dataflow.h": 4, "coroutine.h": 5, "pools.h": 5, "plate.h": 5,
    "domain.h": 5, "memfind.h": 5, "prelude.h": 5, "dbg.h": 0, "time.h": 0,
}
# Headers a later phase may include even though they sit lower/higher in the table.
PUBLISHED = {"plan.h", "check.h", "codegen.h", "parser.h", "modules.h", "types.h", "ast.h",
             "base.h", "dbg.h", "coroutine.h", "pools.h", "plate.h", "domain.h",
             "memfind.h", "prelude.h", "lexer.h", "time.h", "dataflow.h"}
PRIVATE = {"check_internal.h"}


def layer_of(name, is_c):
    """Phase number of a file: a `.c` sits at the phase of its own header."""
    if not is_c:
        return PHASE.get(name)
    for h, ph in PHASE.items():
        if name[:-2] + ".h" == h:
            return ph
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-baseline", action="store_true")
    a = ap.parse_args()

    violations = {}
    src = os.path.join(ROOT, "src")
    for name in sorted(os.listdir(src)):
        if not name.endswith((".c", ".h")):
            continue
        # `#include "..."` is a directive, not a string literal: read the raw text, not
        # the stripped one (`cscan.strip` blanks what is inside the quotes).
        text = open(os.path.join(src, name), encoding="utf-8").read()
        for m in re.finditer(r'#\s*include\s+"([A-Za-z_]+\.h)"', text):
            inc = m.group(1)
            if inc not in PRIVATE:
                continue
            # A private header may only be included by the files of its own subsystem:
            # `check_*.c` are the checker, and `check_internal.h` is the checker's own
            # private header. Any *other* phase including it is the violation.
            if name.startswith("check_") and inc == "check_internal.h":
                continue
            if name == "check.c" and inc == "check_internal.h":
                continue
            violations["src/%s -> %s" % (name, inc)] = (
                "包了另一个阶段的私有头（去掉它会有编译错误 ⇒ 说明真正依赖的是里面的谓词，"
                "见 docs/topics/AST-DECOUPLING.md P3）")
        # The reverse edge that the plan wants removed: the parser reaching into the plan.
        if name == "parser.c" and re.search(r'#\s*include\s+"plan\.h"', text):
            violations["src/parser.c -> plan.h"] = (
                "parser 反向依赖计划层（`for` 的步进今天记在 plan 上）⇒ P2 把 forStep 归 parser")
    ratchet = cscan.Ratchet(BASELINE)
    if a.update_baseline:
        ratchet.write(violations, "R3 只读边界的施工单")
        return 0
    return ratchet.check(violations, "layering")


if __name__ == "__main__":
    sys.exit(main())

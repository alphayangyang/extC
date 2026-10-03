#!/usr/bin/env python3
"""R1 · the AST is frozen once the parser has built it.

Why this rule exists (docs/topics/AST-DECOUPLING.md): a node the analysis phase can still
write is not a syntax tree but a shared mutable record -- and a node shared by several
instances of a generic body made "the last writer wins" a silent miscompile. Every real
compiler surveyed states the same rule (rustc: "the AST and HIR are immutable once
created"; Clang: a node carries only what exactly one phase writes once).

What counts as a violation
--------------------------
A **write to a member that is declared only on AST node structs**, in a file that is
neither the parser nor the plan's own storage layer. The node structs are named in
`NODE_STRUCTS` below; a member name that also exists on a non-node struct (`Type`, the
parser's `LamCap`/`MatchArm`, codegen's own descriptor structs) is excluded, because a
textual scan cannot tell those bases apart -- and a baseline full of false positives is
worse than no baseline.

Writes are `node->field = ...` and the `op=`/`++`/`--` forms. `planSet*` calls are **not**
counted: they are the sanctioned way to write analysis results (which is what the plan
side table is for), and counting them would make the number grow as the code gets cleaner.

Known limitation (stated, not hidden): the scan is textual, so a write through a base the
scanner cannot type (`s->name` where `s` is a local descriptor) can still appear. The
baseline is therefore a **direction indicator with a measured phase**: P2 of the plan is
where the field set is reduced to zero, and each removal is verified by the compiler (a
deleted field makes every remaining direct use a compile error).

Usage
-----
    python3 tools/check_ast_freeze.py [--update-baseline] [--verbose]
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cscan

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "ast-freeze-known-bad.txt")

# The syntax tree: nodes the parser builds.
NODE_STRUCTS = {"Expr", "Stmt", "FuncDef", "StructDef", "TypeDef", "FieldDef", "Variant",
                "Param", "GlobalDef", "TraitDef", "ImplDef", "UseDecl", "Module"}
# A member name that exists on a node **and** on a non-node struct the scanner cannot type
# (`owner` is on FuncDef *and* on FuncDef again through a pointer field). Names listed here
# are dropped from the rule, because a textual scan cannot tell the bases apart -- and a
# baseline full of false positives is worse than no baseline.
AMBIGUOUS_NAMES = {"owner"}
# Files allowed to write the tree, and why.
BUILDERS = {"parser.c": "builds the tree", "lexer.c": "builds tokens", "ast.c": "tree helpers",
            "plan.c": "the storage layer of the plan side table",
            "types.c": "builds type objects (the type table's own builder)"}


def node_members():
    code = cscan.strip(open(os.path.join(ROOT, "src", "ast.h"), encoding="utf-8").read())
    on_node, elsewhere = set(), set()
    for name, members in cscan.structs(code):
        (on_node if name in NODE_STRUCTS else elsewhere).update(members)
    return on_node - elsewhere


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    members = node_members() - AMBIGUOUS_NAMES
    violations = collections.Counter()
    detail = collections.defaultdict(list)
    for path in cscan.src_files(ROOT, (".c",)):
        name = os.path.basename(path)
        if name in BUILDERS:
            continue
        rel = cscan.rel(ROOT, path)
        code = cscan.strip(open(path, encoding="utf-8").read())
        for m in re.finditer(r"(?:->|\.)\s*([A-Za-z_]\w*)\b", code):
            field = m.group(1)
            if field not in members or not cscan.WRITE.match(code[m.end():m.end() + 4]):
                continue
            line = code[:m.start()].count("\n") + 1
            key = "%s:%s" % (rel, field)
            violations[key] += 1
            detail[key].append("%d" % line)
            if a.verbose:
                print("   %s:%d %s" % (name, line, field))

    fails = {k: "%d 处（行 %s）" % (n, ", ".join(detail[k][:5])) for k, n in violations.items()}
    ratchet = cscan.Ratchet(BASELINE)
    if a.update_baseline:
        ratchet.write(fails, "R1 AST 冻结的施工单：P2 把它降到 0")
        return 0
    if a.verbose:
        print("AST 节点专属成员 %d 个；parser/plan/types 之外的文件 %d 个；直接写 AST 的处数："
              % (len(members), len(files)))
        for k, n in violations.most_common():
            print("   %-40s %d" % (k, n))
    return ratchet.check(fails, "ast-freeze")


if __name__ == "__main__":
    sys.exit(main())

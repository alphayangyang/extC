#!/usr/bin/env python3
"""Check that the reclamation plan is written in the places that own it.

`struct RcPlan` (src/back/codegen.c) is the single value that answers *whether* and *how* a
translation unit reclaims: `reclaim`, `forced`, `memGrowth`, `nShard`, `noCount`,
`sweepFree`, `rootsUnreliable`. Before this check those answers were seven fields on `CG`
written from wherever the emitter happened to be, and one of the two real bugs of the
reclamation work came from exactly that shape: an early return moved above the idle proof
(`rc.forced` above `unitReclaimIdle`) changed the plan for every unit built with the switch
on, with no test able to see it because the switch-on gates do not compile an idle unit.

The check has four teeth:

  1. the plan's field set is exactly the seven names; a silent addition is a red.
  2. only the functions in OWNERS write a decision field. A site that decides for itself --
     e.g. an emitter setting `reclaim` because it saw a `new` -- is a red, whatever it emits.
  3. `rootsUnreliable` may only ever be set to true (`= true`, or the one derived assignment
     from `rcGlobalsUnreliable`): a degradation flag that can be cleared is a plan change
     hidden in a code path, and the direction of the flag is the safety direction.
  4. inside `rcPlanDecide` the idle proof comes first: `unitReclaimIdle(` must appear before
     the `rc.forced` early return, and `memGrowth` must be set between them. This is the v30
     regression made a judgement: the order of the pruning steps is part of the plan.

Two smaller ones: the legacy field names must not come back (`needReclaim`, `rcForced`,
`rcMemGrowth`, `rcNShard`, `rcNoCount`, `rcSweepFree`, `rcRootsUnreliable`), and every
`g->rc.X` occurrence that is not an assignment is a read -- reads are allowed anywhere.

Both the legacy and the write scan run on code with comments and literals blanked out (so a
comment may discuss the history, and a diagnostic string may still print `needReclaim`), while
line numbers are preserved.

  python3 tools/check_rc_plan.py            check this tree
  python3 tools/check_rc_plan.py --selftest  prove the judgement has teeth on synthetic text
"""
import argparse
import os
import re
import sys

import doc_anchors
import source_split

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = "src/back/codegen.c"

FIELDS = {
    "reclaim": "whether a reclamation runtime is emitted at all",
    "forced": "EXTC_RECLAIM=1: the governed-allocation pruning is skipped",
    "memGrowth": "idle proof criterion (d): a loop replaces a frame-level array",
    "nShard": "EXTC_RC_SHARDS, narrowed to 1 when a cycle crosses sites",
    "noCount": "the count entry points are no-ops",
    "sweepFree": "the event tier (1), the sweep tier (0), or swept-but-forced-off (-1)",
    "rootsUnreliable": "the static root table is incomplete (degradation)",
}

# Which functions own which decision. Anything else writing one is a red.
OWNERS = {
    "reclaim": {"scanUnitNeeds", "rcPlanDecide"},
    "forced": {"scanUnitNeeds"},
    "memGrowth": {"rcPlanDecide"},
    "nShard": {"scanUnitNeeds", "rcPlanDecide"},
    "noCount": {"scanUnitNeeds", "rcPlanDecide", "generateC"},
    "sweepFree": {"scanUnitNeeds", "rcPlanDecide", "generateC"},
}

WRITE = re.compile(r"g->rc\.([A-Za-z_][A-Za-z0-9_]*)\s*(=(?!=)|\+=|\|=|&=)")
LEGACY = re.compile(r"\b(needReclaim|rcForced|rcMemGrowth|rcNShard|rcNoCount|rcSweepFree|rcRootsUnreliable)\b")


def code_only(text):
    """Blank out comments and string/char literals, keeping every newline and every offset.

    The scans below ask about code, not about prose: a comment is allowed to explain why the
    old field was called `rcForced`, and a diagnostic string is allowed to print it.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != chr(10):
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append(chr(10) if text[i] == chr(10) else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        if c == chr(34) or c == chr(39):
            quote = c
            out.append(" ")
            i += 1
            while i < n:
                if text[i] == chr(92) and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                if text[i] == quote:
                    out.append(" ")
                    i += 1
                    break
                out.append(chr(10) if text[i] == chr(10) else " ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def plan_fields(code):
    m = re.search(r"typedef struct RcPlan \{(.*?)\} RcPlan;", code, re.S)
    if not m:
        return None
    return set(re.findall(r"^\s*(?:bool|int)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;", m.group(1), re.M))


def rc_plan_body(code):
    start = code.find("static void rcPlanDecide(")
    if start < 0:
        return ""
    depth = 0
    i = code.find("{", start)
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return code[start:j + 1]
    return code[start:]


def check_text(text, label="tree"):
    bad = []
    code = code_only(text)
    fields = plan_fields(code)
    if fields is None:
        return [f"{label}: struct RcPlan is missing"]
    if fields != set(FIELDS):
        extra = sorted(fields - set(FIELDS))
        gone = sorted(set(FIELDS) - fields)
        bad.append(f"{label}: RcPlan fields are not the plan (extra={extra}, missing={gone})")
    m = LEGACY.search(code)
    if m:
        line = code.count(chr(10), 0, m.start()) + 1
        bad.append(f"{label}:{line}: legacy plan field {m.group(1)} is back")
    idx = doc_anchors.index_of(SRC)
    for i, line in enumerate(code.split(chr(10))):
        for w in WRITE.finditer(line):
            field = w.group(1)
            owner = idx.token_at(i + 1)
            if field == "rootsUnreliable":
                rhs = line[w.end():].strip()
                if not (rhs.startswith("true") or rhs.startswith("g->rc.reclaim && g->rcGlobalsUnreliable")):
                    bad.append(f"{label}:{i + 1}: rootsUnreliable may only degrade to true, wrote '{rhs[:40]}' in {owner}")
                continue
            if field not in FIELDS:
                bad.append(f"{label}:{i + 1}: write to unknown plan field {field} in {owner}")
                continue
            if owner not in OWNERS[field]:
                bad.append(f"{label}:{i + 1}: {field} written in {owner}, but only {sorted(OWNERS[field])} own it")
    body = rc_plan_body(code)
    if not body:
        bad.append(f"{label}: rcPlanDecide is missing")
    else:
        idle = body.find("unitReclaimIdle(")
        forced = body.find("if (g->rc.forced)")
        growth = body.find("g->rc.memGrowth = true")
        if idle < 0:
            bad.append(f"{label}: rcPlanDecide does not run the idle proof")
        if forced < 0:
            bad.append(f"{label}: rcPlanDecide does not honour the forced switch")
        if idle >= 0 and forced >= 0 and forced < idle:
            bad.append(f"{label}: rcPlanDecide returns on the forced switch before the idle proof (the v30 regression)")
        if growth < 0:
            bad.append(f"{label}: rcPlanDecide does not set memGrowth for criterion (d)")
        elif idle >= 0 and forced >= 0 and not (idle < growth < forced):
            bad.append(f"{label}: memGrowth is set outside the idle-proof branch")
    return bad


def selftest():
    text = source_split.read(SRC)
    if check_text(text):
        print("selftest: the current tree is already red; run without --selftest first", file=sys.stderr)
        return 1
    cases = []
    # 1. a site that decides for itself
    cases.append(("stray write", text.replace("static void emitPreamble(", "static void emitPreamble(" + chr(10) + "    g->rc.reclaim = true;", 1)))
    # 2. a degradation flag that can be cleared
    cases.append(("clear degradation", text.replace("g->rc.rootsUnreliable = true;", "g->rc.rootsUnreliable = false;", 1)))
    # 3. the forced switch back above the idle proof
    body_start = text.find("static void rcPlanDecide(")
    idle_at = text.find("    if (unitReclaimIdle(g, m)) {", body_start)
    forced_at = text.find("    if (g->rc.forced) return;", body_start)
    if idle_at > 0 and forced_at > idle_at:
        moved = text[:forced_at] + text[forced_at:].replace("    if (g->rc.forced) return;" + chr(10), "", 1)
        moved = moved[:idle_at] + "    if (g->rc.forced) return;" + chr(10) + moved[idle_at:]
        cases.append(("forced before idle", moved))
    # 4. a new plan field nobody asked for
    cases.append(("extra field", text.replace("} RcPlan;", "    bool extra;" + chr(10) + "} RcPlan;", 1)))
    # 5. the legacy name coming back in code
    cases.append(("legacy name", text.replace("g->rc.reclaim = false;", "g->needReclaim = false;", 1)))
    bad_cases = 0
    for name, mutated in cases:
        if mutated == text:
            print(f"  selftest {name}: mutation did not apply", file=sys.stderr)
            bad_cases += 1
            continue
        errs = check_text(mutated, label=name)
        if errs:
            print(f"  ok red: {name}")
        else:
            print(f"  selftest FAILED: {name} stayed green", file=sys.stderr)
            bad_cases += 1
    return 1 if bad_cases else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    text = source_split.read(SRC)
    bad = check_text(text)
    if bad:
        for b in bad:
            print("FAIL " + b, file=sys.stderr)
        return 1
    n = len(WRITE.findall(code_only(text)))
    print("ok rc plan: %d fields, %d writes, all in the functions that own them (idle proof before the forced return, degradation flags only degrade)" % (len(FIELDS), n))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

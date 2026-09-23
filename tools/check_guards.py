#!/usr/bin/env python3
"""Check that every header closes its include guard at the end of the file.

A declaration placed after the `#endif` of a header's include guard is compiled
once per textual inclusion.  With three include paths into the same header the
same prototype is then declared three times, which is a redundant declaration at
best and a type conflict at worst.  This happened for real: a documentation block
was appended to the end of `types.h`, after the `#endif`, leaving nine lines
outside the guard.

The check is deliberately mechanical:

  * every `src/*.h` must open a guard (`#ifndef X` / `#define X` as the first
    two preprocessor lines), and
  * its last non-empty line must be `#endif` (optionally with a trailing comment
    naming the guard), and
  * the guard named there, when present, must be the macro the file defined.

Usage: tools/check_guards.py [directory]        (default: src)
Exit status: 0 when every header is well formed, 1 otherwise.
"""
import glob
import os
import re
import sys

OPEN = re.compile(r'^\s*#\s*ifndef\s+(\w+)\s*$')
DEFINE = re.compile(r'^\s*#\s*define\s+(\w+)\s*$')
ENDIF = re.compile(r'^\s*#\s*endif\s*(?:/\*\s*(\w+)\s*\*/)?\s*$')


def check(path):
    """Return a list of problems, empty when the header is well formed."""
    with open(path, encoding='utf-8', errors='replace') as fh:
        lines = fh.read().split('\n')
    body = [(i + 1, l) for i, l in enumerate(lines) if l.strip()]
    if not body:
        return ['file is empty']

    problems = []
    guard = None
    # The guard must be the first preprocessor directive in the file.
    for ln, text in body:
        if text.lstrip().startswith('#'):
            m = OPEN.match(text)
            if not m:
                problems.append(f'line {ln}: first preprocessor line is not `#ifndef <GUARD>`')
            else:
                guard = m.group(1)
                nxt = body[body.index((ln, text)) + 1] if body.index((ln, text)) + 1 < len(body) else None
                if nxt is None or not DEFINE.match(nxt[1]) or DEFINE.match(nxt[1]).group(1) != guard:
                    problems.append(f'line {ln}: `#ifndef {guard}` is not followed by `#define {guard}`')
            break

    last_ln, last = body[-1]
    m = ENDIF.match(last)
    if not m:
        problems.append(f'line {last_ln}: the last non-empty line is not `#endif`: {last.strip()[:60]!r}')
    else:
        named = m.group(1)
        if guard and named and named != guard:
            problems.append(f'line {last_ln}: `#endif` names `{named}` but the guard is `{guard}`')

    # Anything between the guard's `#endif` and the end of the file is outside it.
    for ln, text in body[:-1]:
        if ENDIF.match(text):
            problems.append(f'line {ln}: an `#endif` appears before the end of the file, so '
                            f'{body[-1][0] - ln} line(s) after it are outside the guard')
            break
    return problems


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'src'
    files = sorted(glob.glob(os.path.join(root, '*.h')))
    if not files:
        print(f'  no headers found under {root}')
        return 1
    bad = 0
    for f in files:
        problems = check(f)
        if problems:
            bad += 1
            print(f'  FAIL {f}')
            for p in problems:
                print(f'       {p}')
    print(f'include guards: {len(files) - bad} ok / {bad} broken')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

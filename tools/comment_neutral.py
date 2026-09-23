#!/usr/bin/env python3
"""Decide whether a change touched *only comments*.

Usage:
    tools/comment_neutral.py <base-revision> [file ...]

For every file, the code is extracted from both revisions by deleting comments
(block and line) while leaving string and character literals alone.  The two
code-only texts must then be byte-identical.  That is the criterion for
"this change cannot alter behaviour".

Why not "the object files are identical": translating a comment changes how many
lines it occupies, which shifts the debug line table, so .o files necessarily
differ (verified: inserting a single comment line changes check_escape.o).

Exit status: 0 = every file is comment-only, 1 = some file changed code.
"""
import subprocess
import sys


def strip_comments(src: str) -> list[str]:
    """Return the source with comments removed, one entry per non-blank line."""
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == '\\' and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            end = src.find('*/', i + 2)
            i = end + 2 if end >= 0 else n
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            end = src.find('\n', i)
            i = end if end >= 0 else n
            continue
        out.append(c)
        i += 1
    lines = [ln.rstrip() for ln in ''.join(out).split('\n')]
    return [ln for ln in lines if ln.strip()]


def code_of(rev: str, path: str) -> list[str] | None:
    r = subprocess.run(['git', 'show', f'{rev}:{path}'],
                       capture_output=True, text=True)
    return None if r.returncode else strip_comments(r.stdout)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    base = sys.argv[1]
    files = sys.argv[2:]
    if not files:
        r = subprocess.run(['git', 'diff', '--name-only', base, '--', 'src'],
                           capture_output=True, text=True)
        files = [f for f in r.stdout.split() if f.endswith(('.c', '.h'))]

    bad = 0
    for path in files:
        before = code_of(base, path)
        if before is None:
            print(f'  new      {path}  (not in {base})')
            continue
        with open(path, encoding='utf-8', errors='replace') as fh:
            after = strip_comments(fh.read())
        if before == after:
            print(f'  ok       {path}  (comments only)')
        else:
            print(f'  CHANGED  {path}  <- code differs, not a comment-only change')
            import difflib
            for line in list(difflib.unified_diff(before, after, lineterm=''))[2:14]:
                print('           ' + line)
            bad += 1
    print(f'comment-only check: {len(files) - bad} ok / {bad} changed')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

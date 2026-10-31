#!/usr/bin/env python3
"""Check that a working-tree file differs from its committed version only in comments.

This is the safety net for comment rewrites. A comment pass may change comment
text, add or remove comment blocks, and reflow comments, but it must not change
code: not tokens, not line structure inside code.

    tools/comment_only_check.py src/base/base.c            # one file
    tools/comment_only_check.py --all src                  # every tracked C file under src

Comparison rule: both versions are stripped of comments (string and character
literals are respected, so `"/*"` inside a literal is not a comment), every line
keeps its non-comment text with trailing whitespace removed, and lines that
become empty are dropped. The stripped token lines must match exactly and in
order. Exit status 0 means comment-only, 1 means code changed or a file is
missing from HEAD.

The tool reads the committed version with `git show HEAD:<path>`; new files with
no committed version are skipped with a note.
"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def strip_comments(text):
    """Return the code text with comments removed, preserving string literals."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            # Line comment: skip to end of line (keep the newline itself).
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(i + 2, n)
        elif c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i])
                    out.append(text[i + 1])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def code_lines(text):
    """Comment-free code lines, trailing whitespace stripped, blanks dropped."""
    return [ln.rstrip() for ln in strip_comments(text).split("\n") if ln.strip()]


def committed(path):
    """Return the HEAD text of path, or None when the file is not committed yet."""
    rel = path.relative_to(ROOT).as_posix()
    r = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return r.stdout


def check(path):
    """Compare one file; return True when the code is unchanged."""
    now = path.read_text(encoding="utf-8", errors="replace")
    old = committed(path)
    if old is None:
        print(f"SKIP {path}: not in HEAD")
        return True
    a, b = code_lines(old), code_lines(now)
    if a == b:
        print(f"ok   {path} ({len(a)} code lines)")
        return True
    import difflib
    diff = list(difflib.unified_diff(a, b, "HEAD", "worktree", lineterm="", n=2))
    print(f"FAIL {path}: code changed")
    print("\n".join(diff[:200]))
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="*", help="files or directories to check")
    ap.add_argument("--all", action="store_true", help="check every tracked C file under the given roots")
    args = ap.parse_args()
    targets = []
    for arg in args.paths or ["src"]:
        p = (ROOT / arg).resolve() if not pathlib.Path(arg).is_absolute() else pathlib.Path(arg)
        if p.is_dir():
            targets.extend(sorted(p.rglob("*.c")) + sorted(p.rglob("*.h")))
        else:
            targets.append(p)
    if not args.paths:
        args.all = True
    if not args.all and not args.paths:
        targets = sorted((ROOT / "src").rglob("*.c"))
    bad = [t for t in targets if not check(t)]
    print(f"\n{len(targets) - len(bad)}/{len(targets)} files comment-only")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

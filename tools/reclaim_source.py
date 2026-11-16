#!/usr/bin/env python3
"""Render src/back/reclaim.c with its section includes expanded.

The emitted runtime text lives under src/back/rt/, one file per emitted section, in
emission order. reclaim.c keeps the RECLAIM_RT array header, the #include lines, and the
rest of the file. The C compiler sees the expansion, so the source-text gates (which
assert on the shape of the emitted text: skip markers, entry points, contract constants)
read it through here as well: text() is the same bytes, once, in order.

Two things are dropped on the way through, both of them this file's own bookkeeping: the
lines between the rt-split:begin / rt-split:end markers (they explain the split to the
reader; they are not array entries), and the #include lines themselves (their files are
expanded in place). Everything else comes out byte for byte.

  python3 tools/reclaim_source.py            print the file list and the rendered line count

Only reclaim.c has sections; every other path falls through to a plain read.
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = "src/back/reclaim.c"
RT_DIR = os.path.join(ROOT, "src", "back", "rt")
INCLUDE = re.compile(r'^#include\s+"(rt/[A-Za-z0-9._-]+)"\s*$')
SYNC_BEGIN = "rt-split:begin"
SYNC_END = "rt-split:end"


def _root():
    """Tools flip ROOT to a copy tree while they run; honour that."""
    return ROOT


def files():
    """Every file that contributes to the compiled reclaim.c, in preprocessor order."""
    root = _root()
    out = [SRC]
    base = os.path.dirname(os.path.join(root, SRC))
    with open(os.path.join(root, SRC), encoding="utf-8") as f:
        for line in f:
            m = INCLUDE.match(line.strip())
            if m:
                out.append(os.path.relpath(os.path.join(base, m.group(1)), root))
    return out


def _rows():
    """[(rendered line, plain file, index in that file)] for the compiled reclaim.c."""
    root = _root()
    with open(os.path.join(root, SRC), encoding="utf-8") as f:
        own = f.read()
    base = os.path.dirname(os.path.join(root, SRC))
    rows = []
    skip = False
    for i, line in enumerate(own.split(chr(10))):
        if not skip and SYNC_BEGIN in line:
            skip = True
        if skip:
            if SYNC_END in line:
                skip = False
            continue
        m = INCLUDE.match(line.strip())
        if not m:
            rows.append((line, SRC, i))
            continue
        rel = os.path.relpath(os.path.join(base, m.group(1)), root)
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            body = f.read()
        if body.endswith(chr(10)):
            body = body[:-1]
        for k, sub in enumerate(body.split(chr(10))):
            rows.append((sub, rel, k))
    return rows


def read(rel):
    """Text of one file; for reclaim.c, the text with the section includes expanded."""
    rel = rel.replace(os.sep, "/")
    if rel != SRC:
        with open(os.path.join(_root(), rel), encoding="utf-8") as f:
            return f.read()
    return chr(10).join(row[0] for row in _rows())


def text():
    return read(SRC)


def lines_with_owner():
    return list(_rows())


def _atomic_write(path, text):
    """Write beside the target and rename: a hard-linked copy tree (tools/inject.py
    --copy) must not be truncated in place, or the write lands in the original tree."""
    tmp = path + ".reclaim-tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def write(rel, new_text):
    """Write one file. reclaim.c is written back by diffing the rendered text and mapping
    the changed span to the section file that owns it (the span must stay inside one file)."""
    rel = rel.replace(os.sep, "/")
    root = _root()
    if rel != SRC:
        _atomic_write(os.path.join(root, rel), new_text)
        return
    old = text()
    p = 0
    limit = min(len(old), len(new_text))
    while p < limit and old[p] == new_text[p]:
        p += 1
    q = 0
    while q < limit - p and old[len(old) - 1 - q] == new_text[len(new_text) - 1 - q]:
        q += 1
    old_end = len(old) - q
    if p > old_end:
        p = old_end
    rows = _rows()
    pos = 0
    owner = None
    for line, f, i in rows:
        start = pos
        end = pos + len(line)
        if start <= p <= end:
            owner = (f, i, start, line)
            break
        pos = end + 1
    if owner is None:
        raise SystemExit("reclaim_source.write: cannot map offset %d" % p)
    f, i, start, line = owner
    head = p - start
    tail = old_end - start
    if not (0 <= head <= len(line) and head <= tail <= len(line) + 1):
        raise SystemExit("reclaim_source.write: change spans more than one line; patch one anchor at a time")
    path = os.path.join(root, f)
    with open(path, encoding="utf-8") as fh:
        own = fh.read()
    lines = own.split(chr(10))
    base = sum(len(x) + 1 for x in lines[:i])
    piece = new_text[p:len(new_text) - q]
    own = own[:base + head] + piece + own[base + tail:]
    _atomic_write(path, own)


def main():
    for rel in files():
        print(rel)
    print("rendered lines: %d" % len(text().split(chr(10))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

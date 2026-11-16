#!/usr/bin/env python3
"""Render a split source file back into the single stream the compiler reads.

Two headers of this tree keep their bodies in per-section files, because each of them is
one very long function-by-function body whose order matters and whose sections are edited
one at a time:

  src/back/reclaim.c   the emitted reclamation runtime, src/back/rt/*.inc
  src/back/codegen.c   the code generator,            src/back/cg/*.inc

The header keeps its own top comment, its #include list and the #include lines that pull in
the sections. The C compiler sees the expansion, so everything that reasons about the text
(skip markers, entry points, contract constants, doc anchors) reads it through here as
well: read() is the same bytes, once, in order.

Two things are dropped on the way through, both of them bookkeeping: the lines between the
"<tag>-split:begin" / "<tag>-split:end" markers (they explain the split to the reader; they
are not code), and the #include lines themselves (their files are expanded in place).
Everything else comes out byte for byte; tools/emitted_identity.py proves the compiled
result is unchanged.

  python3 tools/source_split.py                 print the file list and rendered line count
  python3 tools/source_split.py --check         assert each split root renders byte-identically
                                                to what the sections hold (round-trip check)

Only the two split roots have sections; every other path falls through to a plain read.
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# split root -> (section directory, marker tag)
SPLITS = {
    "src/back/reclaim.c": ("rt", "rt-split"),
    "src/back/codegen.c": ("cg", "cg-split"),
}
INCLUDE = re.compile(r'^#include\s+"((?:rt|cg)/[A-Za-z0-9._-]+)"\s*$')


def _root():
    """Tools flip ROOT to a copy tree while they run; honour that."""
    return ROOT


def _rel(rel):
    return rel.replace(os.sep, "/")


def split_tag(rel):
    """(section dir, marker tag) for a split root, else None."""
    spec = SPLITS.get(_rel(rel))
    return spec


def files(rel="src/back/reclaim.c"):
    """Every file that contributes to the compiled version of rel, in preprocessor order."""
    rel = _rel(rel)
    root = _root()
    out = [rel]
    if not split_tag(rel):
        return out
    base = os.path.dirname(os.path.join(root, rel))
    with open(os.path.join(root, rel), encoding="utf-8") as f:
        for line in f:
            m = INCLUDE.match(line.strip())
            if m:
                out.append(os.path.relpath(os.path.join(base, m.group(1)), root))
    return out


def _rows(rel="src/back/reclaim.c"):
    """[(rendered line, plain file, index in that file)] for the compiled rel."""
    rel = _rel(rel)
    root = _root()
    with open(os.path.join(root, rel), encoding="utf-8") as f:
        own = f.read()
    if not split_tag(rel):
        return [(line, rel, i) for i, line in enumerate(own.split(chr(10)))]
    _, tag = split_tag(rel)
    begin, end = tag + ":begin", tag + ":end"
    base = os.path.dirname(os.path.join(root, rel))
    rows = []
    skip = False
    for i, line in enumerate(own.split(chr(10))):
        if not skip and begin in line:
            skip = True
        if skip:
            if end in line:
                skip = False
            continue
        m = INCLUDE.match(line.strip())
        if not m:
            rows.append((line, rel, i))
            continue
        sub_rel = os.path.relpath(os.path.join(base, m.group(1)), root)
        with open(os.path.join(root, sub_rel), encoding="utf-8") as f:
            body = f.read()
        if body.endswith(chr(10)):
            body = body[:-1]
        for k, sub in enumerate(body.split(chr(10))):
            rows.append((sub, sub_rel, k))
    return rows


def read_path(path):
    """Text of a path a tool was handed: split roots render, everything else is read as is.
    Selftests hand this function a temporary file outside the tree; that is a plain read."""
    p = str(path)
    root = _root()
    ap, ar = os.path.abspath(p), os.path.abspath(root)
    if ap.startswith(ar + os.sep):
        rel = os.path.relpath(ap, ar).replace(os.sep, "/")
        if split_tag(rel):
            return read(rel)
    with open(p, encoding="utf-8") as f:
        return f.read()


def read(rel):
    """Text of one file; for a split root, the text with its section includes expanded."""
    rel = _rel(rel)
    if not split_tag(rel):
        with open(os.path.join(_root(), rel), encoding="utf-8") as f:
            return f.read()
    return chr(10).join(row[0] for row in _rows(rel))


def text(rel="src/back/reclaim.c"):
    return read(rel)


def lines_with_owner(rel="src/back/reclaim.c"):
    return list(_rows(rel))


def _atomic_write(path, text):
    """Write beside the target and rename: a hard-linked copy tree (tools/inject.py
    --tree copy) must not be truncated in place, or the write lands in the original tree."""
    tmp = path + ".split-tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def write(rel, new_text):
    """Write one file. For a split root, the rendered text is diffed and the changed span is
    mapped back to the section file that owns those lines; one edit, one file."""
    rel = _rel(rel)
    root = _root()
    if not split_tag(rel):
        _atomic_write(os.path.join(root, rel), new_text)
        return
    old = read(rel)
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
    rows = _rows(rel)
    off, owner, tail_owner = 0, None, None
    for line, f, i in rows:
        start = off
        end = off + len(line)
        if owner is None and start <= p <= end:
            owner = (f, i, start, line)
        if start <= old_end <= end + 1:
            tail_owner = (f, i, start)
        off = end + 1
    if owner is None or tail_owner is None:
        raise SystemExit("source_split.write: cannot map the changed span")
    f, i, start, line = owner
    if tail_owner[0] != f:
        raise SystemExit("source_split.write: change spans more than one section file; patch one section at a time")
    head = p - start
    tail = old_end - owner[2]
    if not (0 <= head <= len(line)):
        raise SystemExit("source_split.write: changed span starts outside the owning line")
    path = os.path.join(root, f)
    with open(path, encoding="utf-8") as fh:
        own = fh.read()
    lines = own.split(chr(10))
    base = sum(len(x) + 1 for x in lines[:i])
    piece = new_text[p:len(new_text) - q]
    own = own[:base + head] + piece + own[base + tail:]
    _atomic_write(path, own)


def check_split(rel):
    """The invariants the split has to keep, none of which the compiler would catch:
    every #include names a file that exists, no section is empty, each section ends with
    exactly one newline (two would inject a blank line into the stream), the markers are
    ordered, and nothing is left unexpanded in the rendered text."""
    root = _root()
    _, tag = split_tag(rel)
    with open(os.path.join(root, rel), encoding="utf-8") as f:
        own = f.read()
    bad = []
    if own.count(tag + ":begin") != 1 or own.count(tag + ":end") != 1:
        bad.append("markers: expected one %s:begin and one %s:end" % (tag, tag))
    elif own.index(tag + ":begin") > own.index(tag + ":end"):
        bad.append("markers: %s:end comes before %s:begin" % (tag, tag))
    for f in files(rel)[1:]:
        path = os.path.join(root, f)
        if not os.path.exists(path):
            bad.append("missing section file: " + f)
            continue
        with open(path, encoding="utf-8") as fh:
            body = fh.read()
        if not body.strip():
            bad.append("empty section file: " + f)
        if not body.endswith(chr(10)):
            bad.append("section file does not end with a newline (it would fuse with the "
                       "next section): " + f)
        if chr(13) in body:
            bad.append("section file has CR line endings: " + f)
    rendered = read(rel)
    if INCLUDE.search(rendered) or [l for l in rendered.split(chr(10)) if INCLUDE.match(l.strip())]:
        bad.append("rendered text still holds an unexpanded section #include")
    if not rendered.endswith(chr(10)):
        bad.append("rendered text does not end with a newline")
    return bad, rendered


def main(argv):
    rc = 0
    if "--check" in argv:
        for rel in SPLITS:
            bad, rendered = check_split(rel)
            n = len(files(rel)) - 1
            if bad:
                for b in bad:
                    print("%s: %s" % (rel, b))
                rc = 1
            else:
                print("%-24s ok  %2d sections, %d rendered lines, %d chars"
                      % (rel, n, len(rendered.split(chr(10))) - 1, len(rendered)))
        if rc == 0:
            print("ok: every split root expands to one stream and every section is present")
        return rc
    for rel in SPLITS:
        print("== %s ==" % rel)
        for f in files(rel):
            print("  " + f)
        print("  rendered lines: %d" % len(text(rel).split(chr(10))))
    return rc


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

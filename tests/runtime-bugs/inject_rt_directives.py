#!/usr/bin/env python3
"""Inject malformed control lines into a copy of RECLAIM_RT, for the bug-1 regression.

The reclaim runtime generator walks RECLAIM_RT one entry at a time and uses `#if` /
`#else` / `#endif` entries as tier switches. If one of those is unbalanced or nests deeper
than the generator's fixed table, the compiler must report a generator bug instead of
emitting broken or silently mis-filtered C text.

Usage: inject_rt_directives.py <src/back/reclaim.c> <mode> <out.c>

Modes:
  deep          put 17 nested `#if 1` entries in front of the table (1 more than the
                16-layer table), and their matching `#endif` entries at the end
  stray_endif   a single unmatched `#endif` entry
  stray_else    a single unmatched `#else` entry
  unterminated  a single `#if 1` entry with no matching `#endif`

The injected entries are written with the same escaping as the real ones ("...\\n"), so the
copy is still valid C and still produces one runtime line per entry.
"""
import sys

MARKER = 'static const char *const RECLAIM_RT[] = {\n'

MODES = {
    'deep': ('    "#if 1\\n",\n' * 17, '    "#endif\\n",\n' * 17),
    'stray_endif': ('    "#endif\\n",\n', ''),
    'stray_else': ('    "#else\\n",\n', ''),
    'unterminated': ('    "#if 1\\n",\n', ''),
}


def main(argv):
    if len(argv) != 4:
        sys.exit('usage: inject_rt_directives.py <reclaim.c> <mode> <out.c>')
    src_path, mode, out_path = argv[1:]
    if mode not in MODES:
        sys.exit('unknown mode: ' + mode)
    head, tail = MODES[mode]
    with open(src_path, encoding='utf-8') as f:
        src = f.read()
    start = src.index(MARKER) + len(MARKER)
    end = src.index('\n};', start) + 1          # start of the line that closes the array
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(src[:start] + head + src[start:end] + tail + src[end:])


if __name__ == '__main__':
    main(sys.argv)

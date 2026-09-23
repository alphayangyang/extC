#!/usr/bin/env python3
"""Report non-ASCII characters that live in *comments* (not in string literals).

Usage: tools/scan_cjk.py [file ...]      (default: src/*.c src/*.h)

Uses the same literal-aware scanner as tools/comment_neutral.py, so a Chinese
character inside a string literal is reported separately from one in a comment:
  - comments must be ASCII-only (COMMENT-STYLE.md rule 1)
  - string literals are compiler output and are reported as "output" instead
"""
import glob
import sys

HERE = __import__('os').path.dirname(__file__)


def scan(src):
    comments, output, i, n = [], [], 0, len(src)
    line = 1
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        if c in ('"', "'"):
            quote, start, val = c, line, []
            i += 1
            while i < n:
                if src[i] == '\\' and i + 1 < n:
                    val.append(src[i:i + 2]); i += 2; continue
                if src[i] == quote:
                    i += 1
                    break
                val.append(src[i]); i += 1
            text = ''.join(val)
            if any(ord(ch) > 127 for ch in text):
                output.append((start, text[:60]))
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            start = line
            end = src.find('*/', i + 2)
            body = src[i:end + 2] if end >= 0 else src[i:]
            if any(ord(ch) > 127 for ch in body):
                comments.append((start, body.replace('\n', ' ')[:60]))
            line += body.count('\n')
            i = end + 2 if end >= 0 else n
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            end = src.find('\n', i)
            body = src[i:end] if end >= 0 else src[i:]
            if any(ord(ch) > 127 for ch in body):
                comments.append((line, body[:60]))
            i = end if end >= 0 else n
            continue
        i += 1
    return comments, output


def main():
    files = sys.argv[1:] or sorted(glob.glob('src/*.c') + glob.glob('src/*.h'))
    total_c = total_o = 0
    for f in files:
        with open(f, encoding='utf-8', errors='replace') as fh:
            comments, output = scan(fh.read())
        total_c += len(comments)
        total_o += len(output)
        if comments:
            print(f'{f}: {len(comments)} comment(s) with non-ASCII  <- must be fixed')
            for ln, txt in comments[:3]:
                print(f'    line {ln}: {txt}')
        elif output and '-v' in sys.argv:
            print(f'{f}: comments clean; {len(output)} output string(s) non-ASCII')
    print(f'comments with non-ASCII: {total_c}   output strings with non-ASCII: {total_o}')
    return 1 if total_c else 0


if __name__ == '__main__':
    sys.exit(main())

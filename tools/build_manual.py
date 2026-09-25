#!/usr/bin/env python3
"""Render docs/manual/*.md into a self-contained HTML site (docs/manual/html/).

Single source of truth: the Markdown. The HTML is a **build product**, committed so the site
opens with no toolchain, and `--check` (run by check.sh) fails when it goes stale.
"""
import re, sys, os, glob, html, hashlib

SRC, OUT = 'docs/manual', 'docs/manual/html'
GROUPS = [('上路', ['00-quickstart', '01-safety']),
          ('语言', ['02-program', '03-lexical', '04-types', '05-vars', '06-expr', '07-stmt']),
          ('结构与内存', ['08-struct', '09-ref', '10-nullref', '11-alloc', '12-driver', '13-vararray']),
          ('标准库与工具', ['14-builtins', '15-errors', '18-modules', '21-flags']),
          ('状态与示例', ['16-unimplemented', '17-undecided', '19-examples']),
          ('标准库参考', ['22-stl-string', '23-stl-stringio']),
          ('透明清单', ['20-internals'])]
KW = {'r': 'i64 u8 ref mut', 'keywords': 'fn let var struct enum impl trait match while'}
KW_EXT = ('fn let var if else while for match return struct enum impl trait ref mut use new break '
          'continue extern as true false').split()
TY_EXT = ('i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool string void unit slice array option result '
          'varArray pool vector map hashMap').split()

def inline(t):
    t = html.escape(t, quote=False)
    t = re.sub(r'`([^`]+)`', r'<code>\1</code>', t)
    t = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', t)
    t = re.sub(r'(?<![\w*])\*([^*\n]+)\*(?![\w*])', r'<em>\1</em>', t)
    t = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', lambda m: '<a href="%s">%s</a>' % (
        re.sub(r'\.md(#|$)', r'.html\1', m.group(2)), m.group(1)), t)
    return t

def highlight(code):
    out, i = [], 0
    pat = re.compile(r'(//[^\n]*|/\*.*?\*/|"(?:[^"\\]|\\.)*")', re.S)
    for m in pat.finditer(code):
        out.append(hl_plain(code[i:m.start()])); out.append('<span class="c">%s</span>' % m.group(0))
        i = m.end()
    out.append(hl_plain(code[i:]))
    return ''.join(out)

def hl_plain(t):
    t = html.escape(t, quote=False)
    t = re.sub(r'\b(' + '|'.join(KW_EXT) + r')\b', r'<span class="k">\1</span>', t)
    t = re.sub(r'\b(' + '|'.join(TY_EXT) + r')\b', r'<span class="t">\1</span>', t)
    t = re.sub(r'\b(\d+)\b', r'<span class="n">\1</span>', t)
    t = re.sub(r'\b([a-z_]\w*)(?=\()', r'<span class="f">\1</span>', t)
    return t

def md_to_html(md):
    lines, out, i = md.split('\n'), [], 0
    while i < len(lines):
        l = lines[i]
        if l.startswith('<!--'):                      # 作者注释不渲染
            while i < len(lines) and '-->' not in lines[i]: i += 1
            i += 1; continue
        if l.startswith('```'):
            lang = l[3:].strip(); i += 1; buf = []
            while i < len(lines) and not lines[i].startswith('```'): buf.append(lines[i]); i += 1
            i += 1
            code = '\n'.join(buf)
            out.append('<pre class="%s"><code>%s</code></pre>' % (
                'lang-extc' if lang in ('extc', 'c', '') else 'lang-other',
                highlight(code) if lang in ('extc', 'c', '') else html.escape(code, quote=False)))
            continue
        m = re.match(r'^(#{1,4})\s+(.*)$', l)
        if m:
            n = len(m.group(1)); out.append('<h%d id="%s">%s</h%d>' % (
                n, slug(m.group(2)), inline(m.group(2)), n)); i += 1; continue
        if l.strip() == '---': out.append('<hr>'); i += 1; continue
        if l.startswith('>'):
            buf = []
            while i < len(lines) and lines[i].startswith('>'): buf.append(lines[i][1:].strip()); i += 1
            out.append('<blockquote>%s</blockquote>' % inline(' '.join(buf))); continue
        if l.startswith('|') and i + 1 < len(lines) and re.match(r'^\|[\s:|-]+\|$', lines[i+1]):
            head = [c.strip() for c in l.strip('|').split('|')]
            i += 2; rows = []
            while i < len(lines) and lines[i].startswith('|'):
                rows.append([c.strip() for c in lines[i].strip('|').split('|')]); i += 1
            out.append('<table><thead><tr>%s</tr></thead><tbody>%s</tbody></table>' % (
                ''.join('<th>%s</th>' % inline(c) for c in head),
                ''.join('<tr>%s</tr>' % ''.join('<td>%s</td>' % inline(c) for c in r) for r in rows)))
            continue
        if re.match(r'^\s*([-*]|\d+\.)\s+', l):
            ordered = bool(re.match(r'^\s*\d+\.', l)); items = []
            while i < len(lines) and re.match(r'^\s*([-*]|\d+\.)\s+', lines[i]):
                items.append(re.sub(r'^\s*([-*]|\d+\.)\s+', '', lines[i])); i += 1
                while i < len(lines) and lines[i].startswith(('  ', '\t')) and lines[i].strip():
                    items[-1] += ' ' + lines[i].strip(); i += 1
            tag = 'ol' if ordered else 'ul'
            out.append('<%s>%s</%s>' % (tag, ''.join('<li>%s</li>' % inline(x) for x in items), tag))
            continue
        if not l.strip(): i += 1; continue
        buf = []
        while i < len(lines) and lines[i].strip() and not re.match(r'^(#{1,4}\s|```|\||>|---)', lines[i]):
            buf.append(lines[i]); i += 1
        out.append('<p>%s</p>' % inline(' '.join(x.strip() for x in buf)))
    return '\n'.join(out)

def slug(s): return re.sub(r'[^\w\u4e00-\u9fff]+', '-', s).strip('-').lower()

CSS = open('tools/manual.css', encoding='utf-8').read() if os.path.exists('tools/manual.css') else ''
JS = open('tools/manual.js', encoding='utf-8').read() if os.path.exists('tools/manual.js') else ''

def page(title, body, sidebar, current, prev, nxt):
    return ('<!doctype html>\n<html lang="zh-CN"><head><meta charset="utf-8">\n'
            '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
            '<title>%s · extC 手册</title>\n<style>%s</style></head><body>\n'
            '<nav class="side"><div class="brand"><a href="index.html">extC 手册</a></div>\n'
            '<input id="q" type="search" placeholder="搜索（/ 聚焦）" autocomplete="off">\n'
            '%s</nav>\n<main><div class="crumb">%s</div>\n%s\n'
            '<div class="pn">%s %s</div></main>\n<script>%s</script></body></html>\n' % (
                title, CSS, sidebar, ' · '.join(current), body, prev, nxt, JS))

def build():
    files = sorted(glob.glob(SRC + '/*.md'))
    pages = [f for f in files if os.path.basename(f) != 'README.md']
    titles = {}
    for f in pages:
        first = open(f, encoding='utf-8').read().split('\n')
        titles[os.path.basename(f)[:-3]] = next(l[2:].strip() for l in first if l.startswith('# '))
    names = [os.path.basename(f)[:-3] for f in pages]
    side = []
    for g, members in GROUPS:
        side.append('<div class="g">%s</div><ul>' % g)
        for n in members:
            if n not in titles: continue
            cls = ' class="cur"' if n == current_name else ''
            side.append('<li%s><a href="%s.html">%s</a></li>' % (cls, n, titles[n]))
        side.append('</ul>')
    return pages, titles, names, side

def render_all():
    pages = sorted(f for f in glob.glob(SRC + '/*.md') if os.path.basename(f) != 'README.md')
    titles, names = {}, []
    for f in pages:
        n = os.path.basename(f)[:-3]; names.append(n)
        titles[n] = next(l[2:].strip() for l in open(f, encoding='utf-8') if l.startswith('# '))
    result = {}
    for k, f in enumerate(pages):
        n = names[k]
        side = []
        for g, members in GROUPS:
            side.append('<div class="g">%s</div><ul>' % g)
            for mm in members:
                if mm in titles:
                    side.append('<li%s><a href="%s.html">%s</a></li>' % (
                        ' class="cur"' if mm == n else '', mm, titles[mm]))
            side.append('</ul>')
        md = open(f, encoding='utf-8').read()
        body = md_to_html(md)
        prev = '<a href="%s.html">← %s</a>' % (names[k-1], titles[names[k-1]]) if k else '<span></span>'
        nxt = '<a href="%s.html">%s →</a>' % (names[k+1], titles[names[k+1]]) if k+1 < len(names) else '<span></span>'
        result[n + '.html'] = page(titles[n], body, '\n'.join(side),
                                   ['extC 手册', '§' + titles[n].split()[0]], prev, nxt)
    idx = md_to_html(open(SRC + '/README.md', encoding='utf-8').read())
    result['index.html'] = page('目录', idx, side_of(names[0], titles, None), ['extC 手册'], '<span></span>',
                                '<a href="%s.html">%s →</a>' % (names[0], titles[names[0]]))
    return result

def side_of(cur, titles, _):
    out = []
    for g, members in GROUPS:
        out.append('<div class="g">%s</div><ul>' % g)
        for mm in members:
            if mm in titles:
                out.append('<li%s><a href="%s.html">%s</a></li>' % (' class="cur"' if mm == cur else '', mm, titles[mm]))
        out.append('</ul>')
    return '\n'.join(out)

if __name__ == '__main__':
    res = render_all()
    if '--check' in sys.argv:
        stale = []
        for name, text in res.items():
            p = os.path.join(OUT, name)
            if not os.path.exists(p) or open(p, encoding='utf-8').read() != text: stale.append(name)
        extra = [f for f in os.listdir(OUT) if f not in res] if os.path.isdir(OUT) else []
        if stale or extra:
            print('docs/manual/html 已过期：%s%s -- 跑 tools/build_manual.py' % (
                ', '.join(sorted(stale)[:5]), ' 多余文件:%s' % extra if extra else '')); sys.exit(1)
        print('ok  HTML 与 Markdown 一致（%d 页）' % len(res)); sys.exit(0)
    os.makedirs(OUT, exist_ok=True)
    for name, text in res.items():
        open(os.path.join(OUT, name), 'w', encoding='utf-8').write(text)
    print('已生成 %d 个 HTML 页面 -> %s/' % (len(res), OUT))

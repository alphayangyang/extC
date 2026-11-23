#!/usr/bin/env python3
"""A language server for extC: diagnostics, outline, hover, go-to-definition, completion.

The compiler is the source of truth for every question here; this file is the JSON-RPC
plumbing between an editor and three command-line modes of `extc`:

    extc --diagnostics-json --check-only --source-stdin <path>   (diagnostics)
    extc --symbols-json      --source-stdin <path>               (declarations)
    extc --check-only        --source-stdin <path>               (fast syntax/type probe)

`--source-stdin` is what makes this work on an **unsaved** buffer: the editor sends its text
on stdin, and the compiler keeps `<path>` as the file's identity (module names, `use`
resolution and diagnostic positions all come from the path). Nothing is written to disk, so
the server never races the editor's own save.

Scope: one file at a time. A diagnostic that the compiler reports for an **imported** module
is published against that module's URI, which is correct, and the editor will then show it
when that file is opened.
"""

import json
import os
import re
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Symbol kinds (LSP's SymbolKind numbers).
K_FUNCTION, K_METHOD, K_STRUCT, K_ENUM, K_VARIABLE, K_INTERFACE = 12, 6, 23, 10, 13, 11

KEYWORDS = """fn let var if else while for in match return break continue struct type trait impl
use pub module trap yield drop par new dyn any ref mut const true false global self as""".split()

BUILTIN_TYPES = """i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool str void
slice buf vector vector hashMap map option result coroutine""".split()


def find_extc():
    env = os.environ.get("EXTC")
    if env:
        return env
    for cand in ("build/extc", "build-an/extc"):
        p = os.path.join(ROOT, cand)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return "extc"


EXTC = find_extc()


def uri_to_path(uri):
    if uri.startswith("file://"):
        from urllib.parse import unquote, urlparse
        u = urlparse(uri)
        return unquote(u.path)
    return uri


def path_to_uri(path):
    from urllib.parse import quote
    return "file://" + quote(os.path.abspath(path))


def run_compiler(text, path, flags):
    """Run one compiler query over `text` (the buffer) and return its stdout lines."""
    try:
        proc = subprocess.run(
            [EXTC] + flags + [path],
            input=text.encode("utf-8"),
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    out = []
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def diagnostics_for(text, path):
    """All diagnostics the compiler reports, grouped by file path."""
    diags = run_compiler(text, path, ["--diagnostics-json", "--check-only", "--source-stdin"])
    return diags


def symbols_for(text, path):
    """The declaration table of `path`; empty when the file does not check."""
    got = run_compiler(text, path, ["--symbols-json", "--source-stdin"])
    if not got:
        return []
    blob = got[0]
    return blob.get("symbols", []) if isinstance(blob, dict) else []


WORD_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def word_at(text, line, char):
    lines = text.split("\n")
    if line >= len(lines):
        return None
    ln = lines[line]
    for m in WORD_RE.finditer(ln):
        if m.start() <= char <= m.end():
            return m.group(0)
    return None


class Server:
    def __init__(self, stdin, stdout):
        self.stdin, self.stdout = stdin, stdout
        self.docs = {}          # uri -> text
        self.shutdown = False

    # ---- transport -----------------------------------------------------
    def send(self, msg):
        body = json.dumps(msg).encode("utf-8")
        self.stdout.write(b"Content-Length: %d\r\n\r\n" % len(body))
        self.stdout.write(body)
        self.stdout.flush()

    def reply(self, ident, result=None, error=None):
        msg = {"jsonrpc": "2.0", "id": ident}
        if error is not None:
            msg["error"] = error
        else:
            msg["result"] = result
        self.send(msg)

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def read_message(self):
        headers = {}
        while True:
            line = self.stdin.readline()
            if not line:
                return None
            line = line.decode("ascii", "replace").strip()
            if not line:
                break
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        try:
            length = int(headers.get("content-length", "0"))
        except ValueError:
            return None
        if length <= 0:
            return None
        data = self.stdin.read(length)
        try:
            return json.loads(data.decode("utf-8", "replace"))
        except json.JSONDecodeError:
            return None

    # ---- diagnostics ---------------------------------------------------
    def publish(self, uri):
        text = self.docs.get(uri, "")
        path = uri_to_path(uri)
        by_file = {}
        for d in diagnostics_for(text, path):
            f = d.get("file") or path
            by_file.setdefault(f, []).append(d)
        for f, ds in by_file.items():
            out = []
            for d in ds:
                line = max(1, int(d.get("line") or 1)) - 1
                col = max(1, int(d.get("col") or 1)) - 1
                msg = d.get("message") or ""
                note = d.get("note") or ""
                if note:
                    msg = msg + "\n" + note
                out.append({
                    "range": {"start": {"line": line, "character": col},
                              "end": {"line": line, "character": col + 1}},
                    "severity": 1 if d.get("severity") == "error" else 2,
                    "source": "extc",
                    "message": msg,
                })
            self.notify("textDocument/publishDiagnostics",
                        {"uri": path_to_uri(f), "diagnostics": out})
        if path not in by_file and os.path.abspath(path) not in [
                os.path.abspath(k) for k in by_file]:
            self.notify("textDocument/publishDiagnostics", {"uri": uri, "diagnostics": []})

    # ---- requests ------------------------------------------------------
    def on_initialize(self, ident, params):
        self.reply(ident, {
            "capabilities": {
                "textDocumentSync": 1,      # full text on every change
                "documentSymbolProvider": True,
                "hoverProvider": True,
                "definitionProvider": True,
                "completionProvider": {"triggerCharacters": [".", ":"]},
            },
            "serverInfo": {"name": "extc-lsp", "version": "0.1"},
        })

    def on_document_symbol(self, ident, params):
        uri = params["textDocument"]["uri"]
        text = self.docs.get(uri, "")
        syms = symbols_for(text, uri_to_path(uri))
        out = []
        for s in syms:
            kind = {"fn": K_FUNCTION, "method": K_METHOD, "struct": K_STRUCT,
                    "type": K_ENUM, "global": K_VARIABLE, "trait": K_INTERFACE}.get(
                        s.get("kind"), K_VARIABLE)
            line = max(1, int(s.get("line") or 1)) - 1
            name = s.get("name") or "?"
            detail = ""
            if s.get("kind") == "fn":
                detail = "fn / %s params" % s.get("params", 0)
            elif s.get("kind") == "struct":
                detail = "struct / %s fields" % s.get("fields", 0)
            elif s.get("kind") == "type":
                detail = "type / %s variants" % s.get("variants", 0)
            out.append({
                "name": name,
                "kind": kind,
                "detail": detail,
                "range": {"start": {"line": line, "character": 0},
                          "end": {"line": line, "character": 0}},
                "selectionRange": {"start": {"line": line, "character": 0},
                                   "end": {"line": line, "character": 0}},
            })
        self.reply(ident, out)

    def on_hover(self, ident, params):
        uri = params["textDocument"]["uri"]
        pos = params["position"]
        text = self.docs.get(uri, "")
        word = word_at(text, pos["line"], pos["character"])
        if not word:
            self.reply(ident, None)
            return
        for s in symbols_for(text, uri_to_path(uri)):
            if s.get("name") == word:
                kind = s.get("kind")
                if kind == "fn":
                    body = "fn %s / %s parameter(s)" % (word, s.get("params", 0))
                elif kind == "struct":
                    body = "struct %s / %s field(s)" % (word, s.get("fields", 0))
                elif kind == "type":
                    body = "type %s / %s variant(s)" % (word, s.get("variants", 0))
                elif kind == "method":
                    body = "method %s on `%s`" % (word, s.get("container", "?"))
                elif kind == "trait":
                    body = "trait %s" % word
                else:
                    body = "global %s" % word
                self.reply(ident, {"contents": {"kind": "markdown",
                                                "value": "```extc\n%s\n```" % body}})
                return
        if word in KEYWORDS:
            self.reply(ident, {"contents": {"kind": "plaintext", "value": "extC keyword"}})
        elif word in BUILTIN_TYPES:
            self.reply(ident, {"contents": {"kind": "plaintext", "value": "extC type"}})
        else:
            self.reply(ident, None)

    def on_definition(self, ident, params):
        uri = params["textDocument"]["uri"]
        pos = params["position"]
        text = self.docs.get(uri, "")
        word = word_at(text, pos["line"], pos["character"])
        if not word:
            self.reply(ident, None)
            return
        for s in symbols_for(text, uri_to_path(uri)):
            if s.get("name") == word:
                line = max(1, int(s.get("line") or 1)) - 1
                self.reply(ident, {"uri": uri, "range": {
                    "start": {"line": line, "character": 0},
                    "end": {"line": line, "character": 0}}})
                return
        self.reply(ident, None)

    def on_completion(self, ident, params):
        uri = params["textDocument"]["uri"]
        text = self.docs.get(uri, "")
        items = []
        names = set()
        for s in symbols_for(text, uri_to_path(uri)):
            n = s.get("name")
            if n and n not in names:
                names.add(n)
                items.append({"label": n, "kind": 3})
        for w in KEYWORDS + BUILTIN_TYPES:
            if w not in names:
                names.add(w)
                items.append({"label": w, "kind": 14})
        for m in WORD_RE.finditer(text):
            w = m.group(0)
            if len(w) >= 3 and w not in names:
                names.add(w)
                items.append({"label": w, "kind": 1})
        self.reply(ident, items)

    # ---- dispatch ------------------------------------------------------
    def handle(self, msg):
        method = msg.get("method")
        ident = msg.get("id")
        params = msg.get("params") or {}
        if method == "initialize":
            self.on_initialize(ident, params)
        elif method == "initialized":
            pass
        elif method == "shutdown":
            self.reply(ident, None)
            self.shutdown = True
        elif method == "exit":
            return False
        elif method == "textDocument/didOpen":
            doc = params["textDocument"]
            self.docs[doc["uri"]] = doc.get("text", "")
            self.publish(doc["uri"])
        elif method == "textDocument/didChange":
            uri = params["textDocument"]["uri"]
            for ch in params.get("contentChanges", []):
                if "text" in ch:
                    self.docs[uri] = ch["text"]
            self.publish(uri)
        elif method == "textDocument/didClose":
            uri = params["textDocument"]["uri"]
            self.docs.pop(uri, None)
            self.notify("textDocument/publishDiagnostics", {"uri": uri, "diagnostics": []})
        elif method == "textDocument/documentSymbol":
            self.on_document_symbol(ident, params)
        elif method == "textDocument/hover":
            self.on_hover(ident, params)
        elif method == "textDocument/definition":
            self.on_definition(ident, params)
        elif method == "textDocument/completion":
            self.on_completion(ident, params)
        elif ident is not None:
            self.reply(ident, error={"code": -32601, "message": "not implemented: %s" % method})
        return True

    def run(self):
        while True:
            msg = self.read_message()
            if msg is None:
                return 0
            if not self.handle(msg):
                return 0


def main():
    stdin = os.fdopen(sys.stdin.fileno(), "rb", buffering=0)
    stdout = os.fdopen(sys.stdout.fileno(), "wb", buffering=0)
    return Server(stdin, stdout).run()


if __name__ == "__main__":
    sys.exit(main())

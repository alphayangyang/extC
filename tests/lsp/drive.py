#!/usr/bin/env python3
"""Drive one JSON-RPC session against `tools/extc-lsp.py` and print the observable results.

The server has no state of its own beyond the buffers the editor sent, so a single session
that opens a good file, breaks it, hovers, asks for the outline and closes it exercises every
capability it claims. Prints a JSON object on stdout; `run.sh` evaluates the assertions, so
this file stays a transport and nothing else.

Each request waits for its matching reply, and each notification (there is only one kind,
`publishDiagnostics`) is read from the stream when the scenario expects it. `documentSymbol`,
`hover`, `definition` and `completion` are requests, so their replies cannot be confused with
a diagnostic: the `id` is checked.
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXTC = os.environ.get("EXTC") or os.path.join(ROOT, "build", "extc")

GOOD = """use std::io
use std::text::string

struct point {
    x: i64
    y: i64
}

fn area(p: point) -> i64 {
    return p.x * p.y
}

fn main() -> i32 {
    let p: point = { x: 3, y: 4 }
    io::println(string::fromI64(area(p)))
    return 0
}
"""


class Client:
    def __init__(self):
        self.proc = subprocess.Popen(
            [sys.executable, os.path.join(ROOT, "tools", "extc-lsp.py")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            env=dict(os.environ, EXTC=EXTC),
        )
        self.next_id = 1

    def _send(self, msg):
        body = json.dumps(msg).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def _read(self):
        headers = {}
        while True:
            line = self.proc.stdout.readline()
            if not line:
                return None
            line = line.decode("utf-8", "replace").strip()
            if not line:
                break
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
        n = int(headers.get("content-length", "0"))
        if n <= 0:
            return None
        return json.loads(self.proc.stdout.read(n).decode("utf-8", "replace"))

    def notify(self, method, params):
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method, params):
        ident = self.next_id
        self.next_id += 1
        self._send({"jsonrpc": "2.0", "id": ident, "method": method, "params": params})
        while True:
            msg = self._read()
            if msg is None:
                return None
            if msg.get("id") == ident:
                return msg.get("result")

    def diagnostics(self):
        """Read notifications until a `publishDiagnostics` for any file arrives."""
        while True:
            msg = self._read()
            if msg is None:
                return []
            if msg.get("method") != "textDocument/publishDiagnostics":
                continue
            ds = msg["params"].get("diagnostics", [])
            return [{
                "line": d["range"]["start"]["line"],
                "col": d["range"]["start"]["character"],
                "severity": d["severity"],
                "message": d["message"],
            } for d in ds]

    def shutdown(self):
        self.request("shutdown", {})
        self.notify("exit", {})
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main():
    tmp = tempfile.mkdtemp(prefix="extc-lsp-")
    path = os.path.join(tmp, "sample.extc")
    with open(path, "w", encoding="utf-8") as f:
        f.write(GOOD)
    uri = "file://" + path

    c = Client()
    res = {}
    init = c.request("initialize", {})
    res["capabilities"] = init["capabilities"] if init else {}
    c.notify("initialized", {})

    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "extc", "version": 1, "text": GOOD}})
    res["clean"] = c.diagnostics()

    symbols = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}}) or []
    res["symbols"] = [s["name"] for s in symbols]

    # Ask at the *use* site, not at the declaration: a hover that only works where the
    # symbol is defined would not help anybody reading code.
    lines = GOOD.split("\n")
    use_line = next(i for i, ln in enumerate(lines) if "area(p)" in ln)
    use_char = lines[use_line].index("area(") + 1

    hover = c.request("textDocument/hover", {"textDocument": {"uri": uri},
                                             "position": {"line": use_line,
                                                          "character": use_char}})
    res["hover"] = hover["contents"]["value"] if hover else None

    definition = c.request("textDocument/definition", {"textDocument": {"uri": uri},
                                                      "position": {"line": use_line,
                                                                   "character": use_char}})
    res["definition"] = [definition["uri"], definition["range"]["start"]["line"]] \
        if definition else None

    completion = c.request("textDocument/completion", {"textDocument": {"uri": uri},
                                                       "position": {"line": use_line,
                                                                    "character": use_char}}) or []
    res["completion"] = [it["label"] for it in completion]

    broken = GOOD.replace("return p.x * p.y", "return p.x * p.zz")
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 2},
        "contentChanges": [{"text": broken}]})
    res["broken"] = c.diagnostics()

    c.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
    res["closed"] = c.diagnostics()
    c.shutdown()

    json.dump(res, sys.stdout)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()

// extC language support for VS Code: a thin client for `tools/extc-lsp.py`.
//
// Two deployment shapes, both covered here:
//
//   * **Remote-WSL** (the folder is opened through "Connect to WSL"): the extension host is
//     Linux, so the server is started directly and paths need no translation.
//   * **Windows VS Code with the checkout in WSL**: the server is started through `wsl.exe`
//     and every document URI is translated to the WSL path (`tools/vscode-extc/paths.js`).
//
// The client is written out rather than pulled from `vscode-languageclient` so the extension
// has no node_modules: it must work on a machine where `npm install` never ran.

'use strict';

const { spawn } = require('child_process');
const fs = require('fs');
const vscode = require('vscode');
const { toServerPath, toClientUri, uriToPath, pathToUri } = require('./paths');

let client = null;
const diagnostics = vscode.languages.createDiagnosticCollection('extc');
const log = vscode.window.createOutputChannel('extC');

/** Append one line to the `extC` output channel (a failed spawn must be visible somewhere). */
function note(line) { log.appendLine(line); }

/** One JSON-RPC connection to `extc --diagnostics-json …` through the LSP server. */
class Client {
    constructor(context) {
        this.context = context;
        this.nextId = 1;
        this.pending = new Map();
        this.buffer = Buffer.alloc(0);
        this.known = new Map();     // server path -> URI the editor opened
        this.timers = new Map();    // uri -> debounce timer
        this.proc = this.spawnServer();
        this.proc.stdout.on('data', (chunk) => this.onData(chunk));
        this.proc.stderr.on('data', (chunk) => note('[server] ' + chunk.toString().trimEnd()));
        this.proc.on('error', (err) => {
            note('[client] cannot start the server: ' + err.message);
            log.show(true);
        });
        this.proc.on('exit', (code) => {
            note('[client] server exited with code ' + code);
            for (const [, p] of this.pending) p.reject(new Error('extc-lsp exited (' + code + ')'));
            this.pending.clear();
            diagnostics.clear();
        });
    }

    config() {
        var c = vscode.workspace.getConfiguration('extc');
        var repo = c.get('repoPath', '/home/alphayang/extC_Compiler');
        // Prefer the checkout that is actually open: a clone lives wherever the user put it,
        // and a hard-coded default that does not exist is the most likely reason the server
        // never starts. The configured value still wins whenever it is real.
        if (!fs.existsSync(repo + '/tools/extc-lsp.py')) {
            for (const folder of vscode.workspace.workspaceFolders || []) {
                const p = toServerPath(folder.uri.toString());
                if (fs.existsSync(p + '/tools/extc-lsp.py')) { repo = p; break; }
            }
        }
        return {
            mode: c.get('mode', 'auto'),
            repoPath: repo,
            distro: c.get('wslDistro', ''),
            python: c.get('python', 'python3'),
        };
    }

    spawnServer() {
        const cfg = this.config();
        const command = cfg.python + ' tools/extc-lsp.py';
        const remote = typeof vscode.env !== 'undefined' ? vscode.env.remoteName : undefined;
        const useWsl = cfg.mode === 'wsl' || (cfg.mode === 'auto' && !remote);
        if (!useWsl) {
            const parts = command.split(' ');
            note('[client] starting `' + command + '` in ' + cfg.repoPath);
            return spawn(parts[0], parts.slice(1), {
                cwd: cfg.repoPath,
                env: Object.assign({}, process.env, { EXTC: cfg.repoPath + '/build/extc' }),
            });
        }
        const script = 'cd ' + cfg.repoPath + ' && EXTC=' + cfg.repoPath +
            '/build/extc exec ' + command;
        const args = cfg.distro ? ['-d', cfg.distro] : [];
        args.push('-e', 'bash', '-lc', script);
        note('[client] starting `wsl.exe -e bash -lc <' + script + '>`');
        return spawn('wsl.exe', args, {
            env: Object.assign({}, process.env, { EXTC_WSL_DISTRO: cfg.distro }),
        });
    }

    onData(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        for (;;) {
            const sep = this.buffer.indexOf('\r\n\r\n');
            if (sep < 0) {
                // Some clients send bare LF headers; accept that too.
                const alt = this.buffer.indexOf('\n\n');
                if (alt < 0) return;
                if (!this.readFramed(alt, 2)) return;
                continue;
            }
            if (!this.readFramed(sep, 4)) return;
        }
    }

    readFramed(sep, sepLen) {
        const head = this.buffer.slice(0, sep).toString('ascii');
        const m = /content-length:\s*(\d+)/i.exec(head);
        if (!m) { this.buffer = Buffer.alloc(0); return true; }
        const len = parseInt(m[1], 10);
        const start = sep + sepLen;
        if (this.buffer.length < start + len) return false;
        const body = this.buffer.slice(start, start + len).toString('utf8');
        this.buffer = this.buffer.slice(start + len);
        let msg;
        try { msg = JSON.parse(body); } catch (_) { return true; }
        this.onMessage(msg);
        return true;
    }

    send(msg) {
        const body = Buffer.from(JSON.stringify(msg), 'utf8');
        this.proc.stdin.write('Content-Length: ' + body.length + '\r\n\r\n');
        this.proc.stdin.write(body);
    }

    /** Human-readable description of where the server came from, for the output channel. */
    describe() {
        const cfg = this.config();
        const remote = typeof vscode.env !== 'undefined' ? vscode.env.remoteName : undefined;
        const useWsl = cfg.mode === 'wsl' || (cfg.mode === 'auto' && !remote);
        return (useWsl ? 'wsl.exe -> ' : 'native -> ') + cfg.repoPath + ' (remoteName=' +
            (remote || 'none') + ')';
    }

    notify(method, params) { this.send({ jsonrpc: '2.0', method, params }); }

    request(method, params) {
        const id = this.nextId++;
        return new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            this.send({ jsonrpc: '2.0', id, method, params });
        });
    }

    onMessage(msg) {
        if (msg.id !== undefined && msg.method === undefined) {
            const p = this.pending.get(msg.id);
            if (!p) return;
            this.pending.delete(msg.id);
            if (msg.error) p.reject(new Error(msg.error.message || 'request failed'));
            else p.resolve(msg.result);
            return;
        }
        if (msg.method === 'textDocument/publishDiagnostics') {
            this.onDiagnostics(msg.params);
        }
    }

    onDiagnostics(params) {
        const uri = params.uri;
        const list = (params.diagnostics || []).map((d) => {
            const start = d.range.start, end = d.range.end;
            const r = new vscode.Range(start.line, start.character, end.line, end.character + 1);
            const sev = d.severity === 1 ? vscode.DiagnosticSeverity.Error
                : d.severity === 2 ? vscode.DiagnosticSeverity.Warning
                    : vscode.DiagnosticSeverity.Information;
            const diag = new vscode.Diagnostic(r, d.message, sev);
            diag.source = 'extc';
            return diag;
        });
        const clientUri = toClientUri(uriToPath(uri), this.known);
        diagnostics.set(vscode.Uri.parse(clientUri), list);
    }

    // ---- document sync ---------------------------------------------------
    syncText(uri, text) {
        const serverPath = toServerPath(uri);
        this.known.set(serverPath, uri);
        this.notify('textDocument/didOpen', {
            textDocument: { uri: pathToUri(serverPath), languageId: 'extc', version: 1, text },
        });
    }

    changeText(uri, text) {
        const serverPath = toServerPath(uri);
        this.known.set(serverPath, uri);
        const prev = this.timers.get(uri);
        if (prev) clearTimeout(prev);
        this.timers.set(uri, setTimeout(() => {
            this.timers.delete(uri);
            this.notify('textDocument/didChange', {
                textDocument: { uri: pathToUri(serverPath), version: 2 },
                contentChanges: [{ text }],
            });
        }, 250));
    }

    closeText(uri) {
        const serverPath = toServerPath(uri);
        this.known.delete(serverPath);
        this.notify('textDocument/didClose', { textDocument: { uri: pathToUri(serverPath) } });
        diagnostics.delete(vscode.Uri.parse(uri));
    }

    // ---- requests --------------------------------------------------------
    async docSymbols(uri) {
        const res = await this.request('textDocument/documentSymbol', {
            textDocument: { uri: pathToUri(toServerPath(uri)) },
        });
        const out = [];
        for (const s of res || []) {
            const line = s.range.start.line;
            const r = new vscode.Range(line, 0, line, 0);
            out.push(this.symbol(s.name, s.kind, r, s.detail || ''));
        }
        return out;
    }

    symbol(name, kind, range, detail) {
        const K = vscode.SymbolKind;
        const map = {
            12: K.Function, 6: K.Method, 23: K.Struct, 10: K.Enum, 13: K.Variable, 11: K.Interface,
        };
        const s = new vscode.SymbolInformation(name, map[kind] || K.Variable, range);
        if (detail) s.detail = detail;
        return s;
    }

    onHover(file) {
        for (const [p, u] of this.known) if (p === toServerPath(u)) { /* keep map warm */ }
        return file;
    }

    async hover(uri, position) {
        const res = await this.request('textDocument/hover', {
            textDocument: { uri: pathToUri(toServerPath(uri)) },
            position: { line: position.line, character: position.character },
        });
        if (!res) return null;
        return new vscode.Hover(new vscode.MarkdownString(res.contents.value || res.contents));
    }

    async definition(uri, position) {
        const res = await this.request('textDocument/definition', {
            textDocument: { uri: pathToUri(toServerPath(uri)) },
            position: { line: position.line, character: position.character },
        });
        if (!res) return null;
        const target = toClientUri(uriToPath(res.uri), this.known);
        const r = new vscode.Range(res.range.start.line, res.range.start.character,
            res.range.end.line, res.range.end.character);
        return new vscode.Location(vscode.Uri.parse(target), r);
    }

    async completion(uri, position) {
        const res = await this.request('textDocument/completion', {
            textDocument: { uri: pathToUri(toServerPath(uri)) },
            position: { line: position.line, character: position.character },
        });
        const items = (res || []).map((it) => new vscode.CompletionItem(it.label,
            it.kind === 14 ? vscode.CompletionItemKind.Keyword
                : it.kind === 3 ? vscode.CompletionItemKind.Function
                    : vscode.CompletionItemKind.Text));
        return new vscode.CompletionList(items, true);
    }
}

function selector() {
    return [{ language: 'extc', scheme: 'file' }, { language: 'extc', scheme: 'vscode-remote' },
        { language: 'extc', scheme: 'untitled' }];
}

function activate(context) {
    client = new Client(context);

    client.request('initialize', {
        processId: process.pid,
        rootUri: null,
        capabilities: {
            textDocument: {
                synchronization: { dynamicRegistration: false },
                documentSymbol: { hierarchicalDocumentSymbolSupport: false },
                hover: { contentFormat: ['markdown', 'plaintext'] },
                completion: { completionItem: { snippetSupport: false } },
            },
        },
    }).then(() => {
        client.notify('initialized', {});
        note('[client] ready: ' + client.describe());
    }).catch((err) => {
        note('[client] initialize failed: ' + err.message);
        log.show(true);
        vscode.window.showWarningMessage('extC language server did not start: ' + err.message);
    });

    for (const doc of vscode.workspace.textDocuments) {
        if (doc.languageId === 'extc') client.syncText(doc.uri.toString(), doc.getText());
    }

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument((doc) => {
            if (doc.languageId === 'extc') client.syncText(doc.uri.toString(), doc.getText());
        }),
        vscode.workspace.onDidChangeTextDocument((e) => {
            if (e.document.languageId === 'extc') client.changeText(e.document.uri.toString(), e.document.getText());
        }),
        vscode.workspace.onDidSaveTextDocument((doc) => {
            if (doc.languageId === 'extc') client.changeText(doc.uri.toString(), doc.getText());
        }),
        vscode.workspace.onDidCloseTextDocument((doc) => {
            if (doc.languageId === 'extc') client.closeText(doc.uri.toString());
        }),
        vscode.languages.registerDocumentSymbolProvider(selector(), {
            provideDocumentSymbols: (doc) => client.docSymbols(doc.uri.toString()),
        }),
        vscode.languages.registerHoverProvider(selector(), {
            provideHover: (doc, pos) => client.hover(doc.uri.toString(), pos),
        }),
        vscode.languages.registerDefinitionProvider(selector(), {
            provideDefinition: (doc, pos) => client.definition(doc.uri.toString(), pos),
        }),
        vscode.languages.registerCompletionItemProvider(selector(), {
            provideCompletionItems: (doc, pos) => client.completion(doc.uri.toString(), pos),
        }, '.', ':'),
        diagnostics,
        log,
        { dispose: () => { try { client.notify('exit', {}); } catch (_) { } client.proc.kill(); } },
    );
}

function deactivate() { /* disposal is registered in `activate` */ }

module.exports = { activate, deactivate };

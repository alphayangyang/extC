// Unit test for the Windows↔WSL path translation used by the VS Code client.
// Run by `tests/lsp/run.sh`; needs only node, no editor.
'use strict';
const assert = require('assert');
const { toServerPath, toClientUri, pathToUri } = require('../../tools/vscode-extc/paths');

const cases = [
    ['file:///c%3A/Users/alphayang/proj/a.extc', '/mnt/c/Users/alphayang/proj/a.extc'],
    ['file:///home/alphayang/extC_Compiler/tests/fs/coro-import.extc',
        '/home/alphayang/extC_Compiler/tests/fs/coro-import.extc'],
    ['\\\\wsl$\\Ubuntu\\home\\alphayang\\a.extc', '/home/alphayang/a.extc'],
    ['\\\\wsl.localhost\\Ubuntu\\home\\alphayang\\a.extc', '/home/alphayang/a.extc'],
    ['c:\\Users\\alphayang\\a.extc', '/mnt/c/Users/alphayang/a.extc'],
    ['vscode-remote://wsl+ubuntu/home/alphayang/a.extc', '/home/alphayang/a.extc'],
];
for (const [input, want] of cases) {
    assert.strictEqual(toServerPath(input), want, 'toServerPath(' + input + ')');
}

// The URI a document was opened with must win when a diagnostic names the same file, so the
// editor does not show a second tab under another spelling of the path.
const known = new Map([['/home/alphayang/a.extc', 'file:///home/alphayang/a.extc']]);
assert.strictEqual(toClientUri('/home/alphayang/a.extc', known), 'file:///home/alphayang/a.extc');
assert.strictEqual(toClientUri('/mnt/c/Users/alphayang/a.extc', new Map()),
    'file:///c%3A/Users/alphayang/a.extc');
assert.strictEqual(pathToUri('/mnt/c/x y/a.extc'), 'file:///mnt/c/x%20y/a.extc');
console.log('paths ok');

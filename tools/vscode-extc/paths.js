// Path translation between the Windows side of VS Code and the WSL side of the compiler.
//
// The server runs inside WSL (that is where the compiler and the checkout live), while a
// Windows VS Code sends `file:///c%3A/...` URIs and UNC paths like `\\\\wsl$\\Ubuntu\\home\\...`.
// The mapping is the standard WSL mount rule: drive `C:` is `/mnt/c`, and a UNC path that
// names a WSL distribution is that distribution's own root.
//
// No `vscode` import here: these are pure functions on purpose, so they can be exercised by
// `node tests/lsp/paths.test.js` on any machine -- including one with no editor installed.

'use strict';

const path = require('path');

/** Decode a `file://` URI to a platform path; other schemes are returned unchanged. */
function uriToPath(uri) {
    if (!uri) return uri;
    // A remote (Remote-WSL / Remote-SSH) document keeps an authority we do not need: the
    // path inside the remote file system starts at the first slash after it.
    if (uri.startsWith('vscode-remote://')) {
        const slash = uri.indexOf('/', 'vscode-remote://'.length);
        return slash < 0 ? '/' : decodeURIComponent(uri.slice(slash));
    }
    if (!uri.startsWith('file://')) return uri;
    let rest = uri.slice('file://'.length);
    // `file:///c:/x` -> `/c:/x`; `file://wsl$/Ubuntu/x` keeps its host as part of the path.
    const slash = rest.indexOf('/');
    if (slash > 0 && !rest.startsWith('/')) rest = rest.slice(slash);
    try {
        rest = decodeURIComponent(rest);
    } catch (_) {
        /* leave undecoded: a malformed escape must not break the whole session */
    }
    // VS Code spells a drive as `file:///c%3A/...`; the decoded form is `/c:/...`.
    return rest.replace(/^\/([A-Za-z]:)/, '$1');
}

/** Encode a path (or UNC path) as a `file://` URI. */
function pathToUri(p) {
    if (!p) return p;
    if (p.startsWith('\\\\')) {
        // `\\wsl$\Ubuntu\home\x` -> `file://wsl$/Ubuntu/home/x`
        const parts = p.slice(2).split('\\');
        const host = parts.shift();
        return 'file://' + host + '/' + parts.map(encodeURIComponent).join('/');
    }
    let s = p.replace(/\\/g, '/');
    if (/^[A-Za-z]:\//.test(s)) return 'file:///' + s[0] + '%3A' + s.slice(2);
    // Percent-encode each segment (spaces, non-ASCII), keeping the separators.
    return 'file:///' + s.replace(/^\//, '').split('/').map(encodeURIComponent).join('/');
}

/**
 * Translate a path the editor knows into the path the WSL server can open.
 *
 * Accepts a URI or a plain path. Returns a POSIX path inside the default WSL filesystem.
 */
function toServerPath(uriOrPath) {
    let p = uriToPath(uriOrPath);
    if (!p) return p;
    p = p.replace(/\\/g, '/');

    // UNC to a WSL share: \\wsl$\Ubuntu\home\x, \\wsl.localhost\Ubuntu\home\x
    const unc = /^(\/\/)?(wsl\$|wsl\.localhost)\/([^/]+)(\/.*)?$/.exec(p);
    if (unc) return unc[4] && unc[4].length ? unc[4] : '/';

    // Drive letter: C:/Users/x -> /mnt/c/Users/x
    const drive = /^([A-Za-z]):(\/|$)/.exec(p);
    if (drive) return '/mnt/' + drive[1].toLowerCase() + (p.length > 2 ? p.slice(2) : '/');

    return p;
}

/**
 * Translate a server path back into something the editor can address.
 *
 * `known` maps server paths to the URI the document was opened with; when a diagnostic names
 * a file we opened, reusing that URI is exact (and keeps the editor from showing a second
 * copy of the same buffer under a different spelling).
 */
function toClientUri(serverPath, known) {
    if (!serverPath) return serverPath;
    if (known && known.has(serverPath)) return known.get(serverPath);
    const m = /^\/mnt\/([a-z])\/(.*)$/.exec(serverPath);
    if (m) return pathToUri(m[1].toLowerCase() + ':/' + m[2]);
    if (serverPath.startsWith('/')) return pathToUri('\\\\wsl$\\' + (process.env.EXTC_WSL_DISTRO || 'Ubuntu') + serverPath.replace(/\//g, '\\'));
    return serverPath;
}

module.exports = { uriToPath, pathToUri, toServerPath, toClientUri };

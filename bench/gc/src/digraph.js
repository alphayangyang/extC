// bench/gc 形状 7：digraph —— Node.js v24（V8，追踪 GC；JIT 家族）。
// 与 bench/gc/src/digraph.extc **逐字同算法/同 PRNG/同顺序**，stdout 必须逐字节一致。
//
// 三个移植坑（task-23）：
// 1) LCG mod 2³¹：必须 `Math.imul`（`*` 会丢精度 ✗）——
//    `gstate = (Math.imul(gstate, 1103515245) + 12345) & 2147483647`
//    （imul 给低 32 位，再掩 31 位 = 真值 mod 2³¹ ✓）。
// 2) mixv 掩到 62 位：JS 没有 64 位整数 ⇒ 本文件给**两条路**（`DIGRAPH_JS_MODE`）：
//      `bigint`（默认）：全程 BigInt，精确但慢 ⇒ **可能冤枉 Node**（报告里注明）；
//      `split`         ：拆 (hi,lo) 两个 32 位半，只在**最后打印**一次用 BigInt 转字符串。
// 3) `x >> 12`：参照实现里 x 先被掩到 < 2^62 ⇒ **符号位恒为 0** ⇒ 算术右移 == 逻辑右移 ✓，
//    所以 split 路用无符号位移即可；**但**为了把这条讲清楚，`shr62` 里显式按"高位来自 hi>>>12、
//    低位来自 (hi<<20)|(lo>>>12)"来实现，并用 `>>> 0` 保证无符号位（等价性由对齐判据当场验 ✓）。
//    若哪天参照实现不再掩码（x 可能置符号位），这里必须改成手动符号扩展 ✗。
//
// 预热（WARM=<n>，默认 0 ⇒ 与单跑逐字相同）：跑 n+1 遍整个负载，只打印最后一遍；WARM_STATS=1 时
// 把每遍耗时写 stderr（冷 = 第 0 遍，暖 = 最后一遍）。
//
// 运行：node src/digraph.js N seed      （DIGRAPH_JS_MODE=split 走拆 32 位那条）
'use strict';

const MODE = process.env.DIGRAPH_JS_MODE || 'bigint';
const MASK = 4611686018427387903n;          // 2^62-1
const MASK_HI = 0x3FFFFFFF;                 // hi 只留 30 位
const TWO32 = 4294967296;                   // 2^32（double 精确）

class Edge {
    constructor(to, next, kind) { this.to = to; this.next = next; this.kind = kind; }
}
class Node {
    constructor() {
        this.id = 0; this.gid = 0; this.payload = 0;
        this.first = null; this.ord = null; this.back = null;
        this.mark = 0; this.cmark = 0; this.snext = null; this.gpin = false;
    }
}
class Handle {
    constructor() { this.target = null; this.newest = null; this.size = 0; this.prev = null; this.next = null; }
}
class Ctx {
    constructor() { this.head = null; this.tail = null; this.n = 0; this.hubs = new Array(8).fill(null); this.hubCount = 0; }
}

let gstate = 1, nextId = 0, gidCounter = 0, backedges = 0, nodesM = 0, groupsM = 0;
let droppedG = 0, droppedN = 0, rejected = 0, cyclesDropped = 0, lastNodes = 0, lastEdges = 0;
let epoch = 0, cepoch = 0;
let accBig = 0n, xaccBig = 0n;              // bigint 路
let accHi = 0, accLo = 0, xaccHi = 0, xaccLo = 0;   // split 路

function nxt() {
    gstate = (Math.imul(gstate, 1103515245) + 12345) & 2147483647;
    return gstate;
}

function parseNum(s, def) {
    if (s === undefined || s === null || s === '') return def;
    let v = 0;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c < 48 || c > 57) return def;
        v = v * 10 + (c - 48);
    }
    return v;
}

/* ---------- bigint 路 ---------- */
function mixvBig(a, b) {
    let x = (BigInt(a) * 2654435761n + BigInt(b) * 40503n + 12345n) & MASK;
    x = x ^ (x >> 12n);
    return x & MASK;
}

/* ---------- split 路：62 位值 = (hi * 2^32 + lo)，hi < 2^30、lo 无符号 32 位 ---------- */
function splitOf(v) {                        // v 是 double 精确整数（本形状 < 2^53）
    const hi = Math.floor(v / TWO32);
    return [hi, v - hi * TWO32];
}
function mixvSplit(a, b) {
    // a,b < 2^22 ⇒ a*2654435761 + b*40503 + 12345 < 2^53 ⇒ double 精确 ✓（本形状 N ≤ 4M 之内成立）
    const t = a * 2654435761 + b * 40503 + 12345;
    let hi = Math.floor(t / TWO32);
    let lo = t - hi * TWO32;
    // x ^ (x >> 12)：位移高位 = hi>>>12，低位 = ((hi<<20)|(lo>>>12))>>>0
    const nhi = hi >>> 12;
    const nlo = ((hi << 20) | (lo >>> 12)) >>> 0;
    lo = (lo ^ nlo) >>> 0;
    hi = (hi ^ nhi) & MASK_HI;
    return [hi, lo];
}
function add62(hi, lo, bhi, blo) {
    const l = lo + blo;                       // < 2^33 ⇒ 精确
    const carry = Math.floor(l / TWO32);
    return [(hi + bhi + carry) & MASK_HI, l - carry * TWO32];
}
function xor62(hi, lo, bhi, blo) {
    return [(hi ^ bhi) & MASK_HI, (lo ^ blo) >>> 0];
}
function bigOfPair(hi, lo) {                  // 只在最后打印时用一次
    return (BigInt(hi) * 4294967296n + BigInt(lo)).toString();
}

function handleAt(c, idx) {
    let h = c.head;
    for (let i = 0; i < idx && h !== null; i++) h = h.next;
    return h;
}

function nodeInGroup(h, j) {
    let x = h.newest;
    for (let i = 0; i < j && x !== null; i++) x = x.ord;
    return x;
}

function addEdge(s, t, k) {
    s.first = new Edge(t, s.first, k);
}

function olderInGroup(s, hub) {
    let c1 = 0;
    for (let y = s.ord; y !== null; y = y.ord) c1++;
    if (c1 === 0) return hub;
    const j = nxt() % c1;
    let z = s.ord;
    let i2 = 0;
    while (z !== null) {
        if (i2 === j) return z;
        i2++;
        z = z.ord;
    }
    return hub;
}

function newerInGroup(s, newest, hub) {
    let c2 = 0;
    for (let y = newest; y !== null; y = y.ord) if (y.id > s.id) c2++;
    if (c2 === 0) return hub;
    const j = nxt() % c2;
    let z = newest;
    let i2 = 0;
    while (z !== null) {
        if (z.id > s.id) {
            if (i2 === j) return z;
            i2++;
        }
        z = z.ord;
    }
    return hub;
}

function pickChordTarget(s, hub, newest, size) {
    const r = nxt() % 100;
    if (r < 60) return olderInGroup(s, hub);
    if (r < 80) return newerInGroup(s, newest, hub);
    if (r < 90) return hub;
    const j3 = nxt() % size;
    let z3 = newest;
    for (let i4 = 0; z3 !== null; i4++) {
        if (i4 === j3) return z3;
        z3 = z3.ord;
    }
    return hub;
}

function groupHasCycle(newest, gid) {
    for (let u = newest; u !== null; u = u.ord) {
        cepoch++;
        const ep = cepoch;
        let top = null;
        for (let e = u.first; e !== null; e = e.next) {
            const t = e.to;
            if (t.gid === gid) {
                if (t.id === u.id) return true;
                if (t.cmark !== ep) { t.cmark = ep; t.snext = top; top = t; }
            }
        }
        let found = false;
        while (top !== null) {
            const x = top;
            top = x.snext;
            x.snext = null;
            if (x.id === u.id) found = true;
            if (!found) {
                for (let e2 = x.first; e2 !== null; e2 = e2.next) {
                    const t2 = e2.to;
                    if (t2.gid === gid && t2.cmark !== ep) { t2.cmark = ep; t2.snext = top; top = t2; }
                }
            }
        }
        if (found) return true;
    }
    return false;
}

function createGroup(c, k, pin, isHub) {
    const gid = gidCounter;
    gidCounter++;
    groupsM++;

    const hub = new Node();
    hub.id = nextId;
    hub.gid = gid;
    hub.payload = nxt() % 1000;
    hub.gpin = pin;
    nextId++;
    nodesM++;
    if (isHub) { c.hubs[0] = hub; c.hubCount = 1; }

    let prev = hub;
    for (let cnt = 1; cnt < k; cnt++) {
        const n = new Node();
        n.id = nextId;
        n.gid = gid;
        n.payload = nxt() % 1000;
        n.ord = prev;
        n.back = prev;
        n.gpin = pin;
        nextId++;
        nodesM++;
        if (isHub) { c.hubs[cnt] = n; c.hubCount = cnt + 1; }
        addEdge(prev, n, 1);
        prev = n;
    }

    addEdge(prev, hub, 1);
    backedges++;

    for (let x = prev; x !== null; x = x.ord) {
        const d = nxt() % 3;
        for (let j = 0; j < d; j++) addEdge(x, pickChordTarget(x, hub, prev, k), 2);
    }

    const h = new Handle();
    h.target = hub;
    h.newest = prev;
    h.size = k;
    h.prev = c.tail;
    if (c.tail !== null) c.tail.next = h;
    else c.head = h;
    c.tail = h;
    c.n++;
}

function tryDrop(c) {
    if (c.n === 0) return;
    const base = nxt() % c.n;
    for (let probe = 0; probe < 8; probe++) {
        let idx = base + probe;
        while (idx >= c.n) idx -= c.n;
        const h = handleAt(c, idx);
        let cnt = 0;
        let anypin = false;
        for (let x = h.newest; x !== null; x = x.ord) {
            cnt++;
            if (x.gpin) anypin = true;
        }
        if (anypin) { rejected++; continue; }
        droppedG++;
        droppedN += cnt;
        if (groupHasCycle(h.newest, h.target.gid)) cyclesDropped++;
        const hp = h.prev;
        const hn = h.next;
        if (hp !== null) hp.next = hn; else c.head = hn;
        if (hn !== null) hn.prev = hp; else c.tail = hp;
        h.prev = null;
        h.next = null;
        c.n--;
        return;
    }
}

function traverseBig(c, ep) {
    let top = null;
    for (let h = c.head; h !== null; h = h.next) {
        const t = h.target;
        if (t.mark !== ep) { t.mark = ep; t.snext = top; top = t; }
    }
    let rn = 0, re = 0;
    let radd = 0n, rxor = 0n;
    while (top !== null) {
        const n = top;
        top = n.snext;
        n.snext = null;
        rn++;
        radd = (radd + mixvBig(n.id, n.payload)) & MASK;
        for (let e = n.first; e !== null; e = e.next) {
            re++;
            rxor = rxor ^ mixvBig(n.id, e.to.id) ^ BigInt(e.kind * 4099);
            const t2 = e.to;
            if (t2.mark !== ep) { t2.mark = ep; t2.snext = top; top = t2; }
        }
    }
    const small = BigInt(rn * 1000003 + re);
    accBig = (accBig + radd + BigInt(rn * 1000003 + re * 31)) & MASK;
    xaccBig = (xaccBig ^ (rxor ^ small)) & MASK;
    lastNodes = rn;
    lastEdges = re;
}

function traverseSplit(c, ep) {
    // ⚠️ 热路径：mixvSplit / add62 / xor62 **全部内联**，不留每调用一次的数组分配
    //（否则这条"快"路会被 GC 自己拖慢，量出来就不是算术差异了）。
    let top = null;
    for (let h = c.head; h !== null; h = h.next) {
        const t = h.target;
        if (t.mark !== ep) { t.mark = ep; t.snext = top; top = t; }
    }
    let rn = 0, re = 0;
    let raHi = 0, raLo = 0, rxHi = 0, rxLo = 0;
    while (top !== null) {
        const n = top;
        top = n.snext;
        n.snext = null;
        rn++;
        // ---- mixv(n.id, n.payload) 内联 ----
        let t = n.id * 2654435761 + n.payload * 40503 + 12345;
        let mhi = Math.floor(t / TWO32);
        let mlo = t - mhi * TWO32;
        {
            const sh = mhi >>> 12;
            const sl = ((mhi << 20) | (mlo >>> 12)) >>> 0;
            mlo = (mlo ^ sl) >>> 0;
            mhi = (mhi ^ sh) & MASK_HI;
        }
        {   // radd += mix
            const l = raLo + mlo;
            const carry = Math.floor(l / TWO32);
            raLo = l - carry * TWO32;
            raHi = (raHi + mhi + carry) & MASK_HI;
        }
        for (let e = n.first; e !== null; e = e.next) {
            re++;
            // ---- mixv(n.id, e.to.id) 内联 ----
            let u = n.id * 2654435761 + e.to.id * 40503 + 12345;
            let uhi = Math.floor(u / TWO32);
            let ulo = u - uhi * TWO32;
            {
                const sh = uhi >>> 12;
                const sl = ((uhi << 20) | (ulo >>> 12)) >>> 0;
                ulo = (ulo ^ sl) >>> 0;
                uhi = (uhi ^ sh) & MASK_HI;
            }
            // rxor ^= mix ^ (kind*4099)   （kind*4099 < 2^32 ⇒ 只动低位）
            rxHi = (rxHi ^ uhi) & MASK_HI;
            rxLo = ((rxLo ^ ulo) ^ (e.kind * 4099)) >>> 0;
            const t2 = e.to;
            if (t2.mark !== ep) { t2.mark = ep; t2.snext = top; top = t2; }
        }
    }
    {   // acc = (acc + radd + rn*1000003 + re*31) & MASK
        const v = rn * 1000003 + re * 31;
        const vhi = Math.floor(v / TWO32);
        const vlo = v - vhi * TWO32;
        let l = accLo + raLo;
        let carry = Math.floor(l / TWO32);
        let lo = l - carry * TWO32;
        let hi = (accHi + raHi + carry) & MASK_HI;
        l = lo + vlo;
        carry = Math.floor(l / TWO32);
        lo = l - carry * TWO32;
        accHi = (hi + vhi + carry) & MASK_HI;
        accLo = lo;
    }
    {   // xacc = (xacc ^ (rxor ^ (rn*1000003 + re))) & MASK
        const v = rn * 1000003 + re;
        const vhi = Math.floor(v / TWO32);
        const vlo = v - vhi * TWO32;
        xaccHi = ((xaccHi ^ rxHi) ^ vhi) & MASK_HI;
        xaccLo = ((xaccLo ^ rxLo) ^ vlo) >>> 0;
    }
    lastNodes = rn;
    lastEdges = re;
}

function resetAll() {
    gstate = 1;
    nextId = 0; gidCounter = 0; backedges = 0; nodesM = 0; groupsM = 0;
    droppedG = 0; droppedN = 0; rejected = 0; cyclesDropped = 0;
    lastNodes = 0; lastEdges = 0; epoch = 0; cepoch = 0;
    accBig = 0n; xaccBig = 0n; accHi = 0; accLo = 0; xaccHi = 0; xaccLo = 0;
}

function runOnce(N, sd) {
    resetAll();
    gstate = sd & 2147483647;

    const c = new Ctx();
    createGroup(c, 8, true, true);

    const rounds = 16 + Math.floor(N / 4096);
    let batch = Math.floor(N / rounds);
    if (batch < 24) batch = 24;
    const traverse = (MODE === 'split') ? traverseSplit : traverseBig;

    for (let round = 0; round < rounds; round++) {
        let created = 0, groupsThisRound = 0;
        while (created < batch) {
            const k = 3 + nxt() % 4;
            createGroup(c, k, false, false);
            created += k;
            groupsThisRound++;
        }
        const mOps = 2 + nxt() % 3;
        for (let m = 0; m < mOps; m++) {
            if (c.n > 0) {
                const idx = nxt() % c.n;
                const h = handleAt(c, idx);
                const s = nodeInGroup(h, nxt() % h.size);
                const f = s.first;
                if (f !== null && f.next !== null) s.first = f.next;
                const r = nxt() % 100;
                let tgt = null;
                if (r < 60) tgt = olderInGroup(s, h.target);
                else if (r < 80) tgt = newerInGroup(s, h.newest, h.target);
                else if (r < 90) {
                    if (c.hubCount > 0) tgt = c.hubs[nxt() % c.hubCount];
                } else {
                    const idx2 = nxt() % c.n;
                    const h2 = handleAt(c, idx2);
                    tgt = nodeInGroup(h2, nxt() % h2.size);
                }
                if (tgt !== null) {
                    if (tgt.gid !== s.gid) tgt.gpin = true;
                    addEdge(s, tgt, 3);
                }
            }
        }
        for (let d = 0; d < groupsThisRound; d++) tryDrop(c);
        epoch++;
        traverse(c, epoch);
    }
}

function main() {
    const argv = process.argv;
    let N = 200000, sd = 20261013;
    if (argv.length > 2) N = parseNum(argv[2], N);
    if (argv.length > 3) sd = parseNum(argv[3], sd);
    if (N < 256) N = 256;

    let warm = parseNum(process.env.WARM, 0);
    if (warm < 0) warm = 0;
    const stats = process.env.WARM_STATS !== undefined;
    const iters = [];
    for (let i = 0; i <= warm; i++) {
        const t0 = process.hrtime.bigint();
        runOnce(N, sd);
        iters.push(Number(process.hrtime.bigint() - t0) / 1e6);
    }
    if (stats) {
        process.stderr.write('WARMSTAT warm=' + warm + ' mode=' + MODE +
            ' iters_ms=' + iters.map(v => v.toFixed(3)).join(',') + '\n');
    }

    let total;
    if (MODE === 'split') {
        const p = add62(accHi, accLo, xaccHi, xaccLo);
        total = bigOfPair(p[0], p[1]);
    } else {
        total = ((accBig + xaccBig) & MASK).toString();
    }
    process.stdout.write('nodes=' + lastNodes + ' edges=' + lastEdges + ' sum=' + total + '\n');
    process.stdout.write('backedges=' + backedges + ' groups_made=' + groupsM + ' nodes_made=' + nodesM +
        ' dropped_groups=' + droppedG + ' dropped_nodes=' + droppedN +
        ' rejected_external=' + rejected + ' cycles_dropped=' + cyclesDropped + '\n');
}

main();

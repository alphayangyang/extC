// 同一份功能：有界符号表（64 桶 × 最多 8 条，满则淘汰尾节点）+ 1024 槽缓存 + 20 万项索引 + 查询。
const N = parseInt(process.argv[2], 10);
const BUCKETS = 64, BUCKET_MAX = 8, CACHE = 1024, INDEXMAX = 200000;
class Node { constructor() { this.key = 0; this.val = 0; this.next = null; } }
class Idx  { constructor() { this.next = null; this.target = null; } }
const head = new Array(BUCKETS).fill(null);
const cache = new Array(CACHE).fill(null);
let ihead = null, sink = 0;
const t0 = process.hrtime.bigint();
for (let i = 0; i < N; i++) {
    const b = i % BUCKETS;
    const t1 = new Node(), t2 = new Node(), t3 = new Node();
    t1.next = t2; t2.next = t3; t3.val = i;
    for (let q = t1; q !== null; q = q.next) sink += q.val;
    const nd = new Node(); nd.key = i; nd.val = i * 3;
    nd.next = head[b]; head[b] = nd;
    cache[i % CACHE] = nd;
    if (i < INDEXMAX) { const ix = new Idx(); ix.next = ihead; ix.target = nd; ihead = ix; }
    for (let q = head[b]; q !== null; q = q.next) if (q.key === i - 7) { q.val++; break; }
    let prev = null, cur = head[b], cnt = 0;
    while (cur !== null && cur.next !== null) { prev = cur; cur = cur.next; if (++cnt > BUCKET_MAX) break; }
    if (cur !== null && cnt >= BUCKET_MAX) { if (prev !== null) prev.next = null; else head[b] = null; }
}
const dt = Number(process.hrtime.bigint() - t0) / 1e6;
console.log(`nodejs  耗时=${dt.toFixed(1)}ms sink=${sink + (ihead ? 1 : 0)}`);

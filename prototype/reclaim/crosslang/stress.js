// 压力形状：双向链表(LRU) + 带父指针的树 + 缓存 + 索引 —— 天然形成环。
const N = parseInt(process.argv[2], 10);
class Node { constructor(k, v) { this.key=k; this.val=v; this.next=null; this.prev=null; this.parent=null; this.child=null; } }
const head = new Array(64).fill(null), tail = new Array(64).fill(null);
const size = new Int32Array(64), cache = new Array(1024).fill(null);
const idx = [], nodes = [];
const t0 = process.hrtime.bigint();
for (let i = 0; i < N; i++) {
  const b = i % 64;
  const nd = new Node(i, i * 3);
  if (head[b] !== null) head[b].prev = nd;
  nd.next = head[b]; head[b] = nd;
  if (tail[b] === null) tail[b] = nd;
  size[b]++;
  cache[i % 1024] = nd;
  if (i < 200000) idx.push(nd);
  if ((i & 15) === 0) { nodes.push(nd); if (nodes.length > 1) { const par = nodes[nodes.length - 2]; par.child = nd; nd.parent = par; } }
  for (let q = head[b]; q !== null; q = q.next) if (q.key === i - 7) { q.val++; break; }
  if (size[b] > 8 && tail[b] !== null) { const pr = tail[b].prev;
    if (pr !== null) { pr.next = null; tail[b] = pr; } else { head[b] = null; tail[b] = null; } size[b]--; }
}
console.log(`nodejs-stress 耗时=${(Number(process.hrtime.bigint()-t0)/1e6).toFixed(1)}ms idx=${idx.length}`);

// bench/gc/v2/src/chase.js —— v2 G3 chase（大活集指针追逐 + churn）的 Node.js 移植。
// 唯一真源：bench/gc/v2/SPEC.md；逐行对照 extC 参考 bench/gc/v2/src/chase.extc。
//
// 两条臂由 JS_MODE 切换（run.sh 的 node:split / node:bigint）。
// stdout 恰好两行、单空格 key=value、每行以换行结尾；诊断只走 stderr 且默认关闭。
'use strict';

const MODE = process.env.JS_MODE || 'bigint';
const SPLIT = MODE !== 'bigint';
const MASK = 4611686018427387903n;        // 2^62-1；split 臂里是恒等掩码（见 SPEC §1）

/* ---------------- 公共：LCG / pick / parseI64 ---------------- */
let gstate = 0;
let gstateB = 0n;
function initState(seed) {
  gstate = seed & 2147483647;
  gstateB = BigInt(gstate);
}
function nxt() {
  if (!SPLIT) {
    gstateB = (gstateB * 1103515245n + 12345n) & 2147483647n;
    return Number(gstateB);
  }
  const sLo = gstate & 0xffff;
  const sHi = gstate >>> 16;
  const mid = sHi * 20077 + sLo * 16838;
  gstate = (((mid & 32767) << 16) + sLo * 20077 + 12345) & 0x7fffffff;
  return gstate;
}
function pick(k) { return ((nxt() / 1024) | 0) % k; }

function parseI64(s) {
  let v = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 48 || c > 57) break;
    v = v * 10 + (c - 48);
  }
  return v;
}

/* ---------------- G3 数据结构：出度 4 的节点 + 全空定长图 ---------------- */
class Node {
  constructor() { this.k = 0; this.v = 0; this.o = [null, null, null, null]; }
}

// SPEC §0：churn 出来的即抛节点必须真被分配（活图靠 graph 保持可达，churn 靠 gHold）。
let gHold = null;
function keepAlive() {
  if (gHold !== null && gHold.k === Number.MIN_SAFE_INTEGER) process.stderr.write('');
}

const CAP = 1048576, CHURN_EVERY = 256, CHURN = 4;

function main() {
  const argv = process.argv;
  let m = CAP;
  if (argv.length > 2) m = parseI64(argv[2]);
  let sd = 20261013;
  if (argv.length > 3) sd = parseI64(argv[3]);
  let w = 4000000;
  if (argv.length > 4) w = parseI64(argv[4]);
  initState(sd);

  // SPEC 的图是 [CAP] 槽；实际只用前 m（m ≤ CAP），按 m 开定长数组即可。
  const graph = new Array(m).fill(null);
  for (let i = 0; i < m; i++) {
    const x = new Node();
    x.k = i;
    x.v = nxt() % 1000;
    graph[i] = x;
  }
  for (let i = 0; i < m; i++) {
    const src = graph[i];
    for (let j = 0; j < 4; j++) src.o[j] = graph[nxt() % m];
  }

  let cur = graph[0];
  let sum = SPLIT ? 0 : 0n;
  let churn = 0, tailv = 0;
  for (let step = 0; step < w; step++) {
    sum = SPLIT ? sum + cur.v : (sum + BigInt(cur.v)) & MASK;
    const r = pick(4);
    const nx = cur.o[r];
    if (nx !== null) cur = nx;
    if (step % CHURN_EVERY === 0) {
      let h = null;
      for (let j = 0; j < CHURN; j++) {
        const s = new Node();
        s.k = step;
        s.v = nxt() % 1000;
        s.o[0] = h;
        h = s;
      }
      tailv = h.v;
      churn += CHURN;
      gHold = h;
    }
  }
  keepAlive();

  process.stdout.write('nodes=' + m + ' edges=' + m * 4 + ' walk=' + w + ' churn=' + churn +
    ' sum=' + sum + ' tailv=' + tailv + '\n');
  process.stdout.write('outdeg=4 cap=1048576 churn_every=256 churn=4\n');
}

main();

// bench/gc/v2/src/cycleratio.js —— v2 G4 cycleratio（环垃圾占比 f）的 Node.js 移植。
// 唯一真源：bench/gc/v2/SPEC.md；逐行对照 extC 参考 bench/gc/v2/src/cycleratio.extc。
//
// 两条臂由 JS_MODE 切换（run.sh 的 node:split / node:bigint）。
// 256 槽窗口把成环的旧组顶成**不可达环**（引用计数收不掉、追踪 GC 能收）——窗口持有
// 最近 256 组，靠 win 数组本身就足以让分配可观察。
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

/* ---------------- G4 数据结构：单向链/环节点 + 每槽(头,节点数)的 256 槽窗口 ---------------- */
class Node {
  constructor(k, v) { this.k = k; this.v = v; this.next = null; }
}

const WINDOW = 256;

function main() {
  const argv = process.argv;
  let g = 400000;
  if (argv.length > 2) g = parseI64(argv[2]);
  let sd = 20261013;
  if (argv.length > 3) sd = parseI64(argv[3]);
  let f = 100;
  if (argv.length > 4) f = parseI64(argv[4]);
  initState(sd);

  const winS = new Array(WINDOW).fill(null);
  const winN = new Array(WINDOW).fill(0);
  let sum = SPLIT ? 0 : 0n;
  let made = 0, evicted = 0, dropnodes = 0, rings = 0;

  for (let i = 0; i < g; i++) {
    const k = 3 + pick(4);
    const isring = pick(100) < f;
    let first = null, prev = null;
    for (let j = 0; j < k; j++) {
      const s = new Node(i, nxt() % 1000);
      if (first === null) first = s;
      if (prev !== null) prev.next = s;
      prev = s;
      sum = SPLIT ? sum + s.v : (sum + BigInt(s.v)) & MASK;
    }
    if (isring) {
      rings += 1;
      prev.next = first;                  // 闭成不可达环（顶掉后 RC 收不掉）
    }
    made += k;
    const b = i % WINDOW;
    if (winS[b] !== null) {
      evicted += 1;
      dropnodes += winN[b];
    }
    winS[b] = first;
    winN[b] = k;
  }

  process.stdout.write('groups=' + g + ' made=' + made + ' evicted=' + evicted +
    ' dropnodes=' + dropnodes + ' sum=' + sum + '\n');
  process.stdout.write('f=' + f + ' window=256 kmin=3 kmax=6 rings=' + rings + '\n');
}

main();

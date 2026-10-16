// bench/gc/v2/src/promote.js —— v2 G1 promote（分代假设）的 Node.js 移植。
// 唯一真源：bench/gc/v2/SPEC.md；逐行对照 extC 参考 bench/gc/v2/src/promote.extc。
//
// 两条臂由 JS_MODE 切换（run.sh 的 node:split / node:bigint）：
//   split （快速臂）：形状里所有数值都 < 2^53（SPEC §1 已逐个核过），用 Number；
//                     只有 LCG 的 62 位中间量拆成 16 位 limb 精确算。
//   bigint（对照臂）：LCG 与掩码累加器全程 BigInt。
//
// stdout 恰好两行、单空格 key=value、每行以换行结尾；诊断/计时只走 stderr 且默认关闭。
'use strict';

const MODE = process.env.JS_MODE || 'bigint';
const SPLIT = MODE !== 'bigint';
const MASK = 4611686018427387903n;        // 2^62-1；split 臂里是恒等掩码（见 §1）

/* ---------------- 公共：LCG（SPEC §1） ---------------- */
let gstate = 0;                            // split 臂状态（Number）
let gstateB = 0n;                          // bigint 臂状态（BigInt）
function initState(seed) {
  gstate = seed & 2147483647;
  gstateB = BigInt(gstate);
}
function nxt() {
  if (!SPLIT) {
    gstateB = (gstateB * 1103515245n + 12345n) & 2147483647n;
    return Number(gstateB);
  }
  // state, a < 2^31 ⇒ 积是 62 位。拆成 16 位 limb，只留 mod 2^31 需要的项：
  //   state*a ≡ ((mid mod 2^15) << 16) + sLo*aLo   (mod 2^31)，mid = sHi*aLo + sLo*aHi
  //   2^32 ≡ 0 (mod 2^31)；中间量全 < 2^53 ⇒ double 精确；用位掩码代替取模。
  const sLo = gstate & 0xffff;
  const sHi = gstate >>> 16;
  const mid = sHi * 20077 + sLo * 16838;   // 1103515245 = 16838*65536 + 20077
  gstate = (((mid & 32767) << 16) + sLo * 20077 + 12345) & 0x7fffffff;
  return gstate;
}

function parseI64(s) {                     // 照抄现有形状：逐字符、遇非数字停
  let v = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 48 || c > 57) break;
    v = v * 10 + (c - 48);
  }
  return v;
}

/* ---------------- G1 数据结构：短命链 + 长命定长表 ---------------- */
class Node {
  constructor(k, v) { this.k = k; this.v = v; this.next = null; }
}

// SPEC §0：短命链必须真的被分配。tailv 是尾端读回；再把最近一次的头挂进模块槽并在
// 最后读一次（数据依赖能挡住 V8 的分配消解），但读回分支永不成立 ⇒ stderr 保持空。
let gHold = null;
function keepAlive() {
  if (gHold !== null && gHold.k === Number.MIN_SAFE_INTEGER) process.stderr.write('');
}

const TABLE = 65536, SHORT = 9, EVERY = 10, WALKCAP = 8;

function main() {
  const argv = process.argv;
  let n = 500000;
  if (argv.length > 2) n = parseI64(argv[2]);
  let sd = 20261013;
  if (argv.length > 3) sd = parseI64(argv[3]);
  initState(sd);

  const table = new Array(TABLE).fill(null);
  let sum = SPLIT ? 0 : 0n;
  let walksum = SPLIT ? 0 : 0n;
  let live = 0, tailv = 0;

  for (let i = 0; i < n; i++) {
    let head = null;
    for (let j = 0; j < SHORT; j++) {
      const s = new Node(i * SHORT + j, nxt() % 1000);
      s.next = head;
      head = s;
      sum = SPLIT ? sum + s.v : (sum + BigInt(s.v)) & MASK;
    }
    tailv = head.v;
    if (i % EVERY === 0) {
      const p = new Node(i, nxt() % 1000);
      const b = i % TABLE;
      p.next = table[b];
      table[b] = p;
      live += 1;
    }
    const b2 = (i * 40503) % TABLE;
    let c = table[b2];
    let cnt = 0;
    while (c !== null && cnt < WALKCAP) {
      walksum = SPLIT ? walksum + c.v : (walksum + BigInt(c.v)) & MASK;
      c = c.next;
      cnt += 1;
    }
    gHold = head;
  }
  keepAlive();

  process.stdout.write('rounds=' + n + ' promoted=' + live + ' walksum=' + walksum +
    ' sum=' + sum + ' tailv=' + tailv + '\n');
  process.stdout.write('table=65536 short=9 every=10 walkcap=8\n');
}

main();

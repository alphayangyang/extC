// bench/gc/v2/src/sizeratio.js —— v2 G2 sizeratio（4 个尺寸类的对象池）的 Node.js 移植。
// 唯一真源：bench/gc/v2/SPEC.md；逐行对照 extC 参考 bench/gc/v2/src/sizeratio.extc。
//
// 两条臂由 JS_MODE 切换（run.sh 的 node:split / node:bigint）：
//   split （快速臂）：Number + 16 位 limb 的 LCG；bigint（对照臂）：LCG 与累加器全程 BigInt。
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

/* ---------------- G2 数据结构：4 个尺寸类（k + p[3|7|11|15]）+ 每类 1024 槽定长表 ---------------- */
class C0 { constructor(k) { this.k = k; this.p = [0, 0, 0]; } }
class C1 { constructor(k) { this.k = k; this.p = [0, 0, 0, 0, 0, 0, 0]; } }
class C2 { constructor(k) { this.k = k; this.p = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]; } }
class C3 { constructor(k) { this.k = k; this.p = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]; } }

// SPEC §0：7/8 的对象立即变垃圾，也必须真被分配（长命表本身可观察，短命的靠 gHold）。
let gHold = null;
function keepAlive() {
  if (gHold !== null && gHold.k === Number.MIN_SAFE_INTEGER) process.stderr.write('');
}

const SLOTS = 1024;

function main() {
  const argv = process.argv;
  let n = 2000000;
  if (argv.length > 2) n = parseI64(argv[2]);
  let sd = 20261013;
  if (argv.length > 3) sd = parseI64(argv[3]);
  initState(sd);

  const t0 = new Array(SLOTS).fill(null);
  const t1 = new Array(SLOTS).fill(null);
  const t2 = new Array(SLOTS).fill(null);
  const t3 = new Array(SLOTS).fill(null);
  let sum = SPLIT ? 0 : 0n;
  let kept = 0, livebytes = 0;

  for (let i = 0; i < n; i++) {
    const cls = pick(4);
    let o = null;
    if (cls === 0) {
      o = new C0(i);
      sum = SPLIT ? sum + o.k : (sum + BigInt(o.k)) & MASK;
      if (i % 8 === 0) {
        const b = (i / 8) % SLOTS;
        if (t0[b] !== null) livebytes -= 32; else kept += 1;
        t0[b] = o;
        livebytes += 32;
      }
    } else if (cls === 1) {
      o = new C1(i);
      sum = SPLIT ? sum + o.k : (sum + BigInt(o.k)) & MASK;
      if (i % 8 === 0) {
        const b = (i / 8) % SLOTS;
        if (t1[b] !== null) livebytes -= 64; else kept += 1;
        t1[b] = o;
        livebytes += 64;
      }
    } else if (cls === 2) {
      o = new C2(i);
      sum = SPLIT ? sum + o.k : (sum + BigInt(o.k)) & MASK;
      if (i % 8 === 0) {
        const b = (i / 8) % SLOTS;
        if (t2[b] !== null) livebytes -= 96; else kept += 1;
        t2[b] = o;
        livebytes += 96;
      }
    } else {
      o = new C3(i);
      sum = SPLIT ? sum + o.k : (sum + BigInt(o.k)) & MASK;
      if (i % 8 === 0) {
        const b = (i / 8) % SLOTS;
        if (t3[b] !== null) livebytes -= 128; else kept += 1;
        t3[b] = o;
        livebytes += 128;
      }
    }
    gHold = o;
  }
  keepAlive();

  process.stdout.write('ops=' + n + ' kept=' + kept + ' livebytes=' + livebytes +
    ' sum=' + sum + '\n');
  process.stdout.write('classes=4 sizes=32..128 slots=1024\n');
}

main();

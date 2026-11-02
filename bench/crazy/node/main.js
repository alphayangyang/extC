// bench/crazy —— Node 臂：同一份 TSV、同一行结果。
const fs = require("fs");

const buf = fs.readFileSync("bench/crazy/data/events.tsv");
let n = 0;
for (let i = 0; i < buf.length; i++) if (buf[i] === 10) n++;

const ts = new Float64Array(n), uid = new Float64Array(n), kind = new Float64Array(n);
const region = new Float64Array(n), cents = new Float64Array(n), ms = new Float64Array(n);

let t0 = process.hrtime.bigint();
let at = 0, field = 0, a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
for (let i = 0; i < buf.length; i++) {
  const ch = buf[i];
  if (ch === 10) {
    ts[at] = a; uid[at] = b; kind[at] = c; region[at] = d; cents[at] = e; ms[at] = f;
    at++; a = b = c = d = e = f = 0; field = 0;
  } else if (ch === 9) field++;
  else {
    const v = ch - 48;
    if (field === 0) a = a * 10 + v;
    else if (field === 1) b = b * 10 + v;
    else if (field === 2) c = c * 10 + v;
    else if (field === 3) d = d * 10 + v;
    else if (field === 4) e = e * 10 + v;
    else f = f * 10 + v;
  }
}
const parseUs = Number(process.hrtime.bigint() - t0) / 1000;

t0 = process.hrtime.bigint();
const groups = new Map();
const kc = new Array(8).fill(0), rc = new Array(6).fill(0);
let slow = 0;
for (let i = 0; i < n; i++) {
  const k = uid[i];
  let g = groups.get(k);
  if (g === undefined) { g = { uid: k, count: 0, cents: 0, worst: 0 }; groups.set(k, g); }
  g.count++; g.cents += cents[i];
  if (ms[i] > g.worst) g.worst = ms[i];
  kc[kind[i]]++; rc[region[i]] += cents[i];
  if (ms[i] > 1000) slow++;
}
const aggUs = Number(process.hrtime.bigint() - t0) / 1000;

t0 = process.hrtime.bigint();
const gs = [...groups.values()];
gs.sort((x, y) => (x.cents !== y.cents ? y.cents - x.cents : x.uid - y.uid));
let top = 0;
for (let i = 0; i < 5 && i < gs.length; i++) top = (top * 1000003 + gs[i].uid * 31 + gs[i].cents) % 1000000007;

let jbytes = 0, jcs = 0;
for (let i = 0; i < n; i += 16) {
  const o = { ts: ts[i], uid: uid[i], kind: kind[i], region: region[i], cents: cents[i], ms: ms[i] };
  if (cents[i] > 15000) o.big = cents[i] * 2;
  const s = JSON.stringify(o);
  jbytes += s.length;
  jcs = (jcs * 131 + s.charCodeAt(s.length - 1)) % 1000000007;
}
const jsonUs = Number(process.hrtime.bigint() - t0) / 1000;

t0 = process.hrtime.bigint();
let total = 0;
for (let k = 0; k < 8; k++)
  for (let i = 0; i < n; i++) total += cents[i] * (ms[i] + 1);
total %= 1000000007;
const parUs = Number(process.hrtime.bigint() - t0) / 1000;

let hcs = 0;
for (let i = 0; i < 8; i++) hcs = (hcs * 7 + kc[i]) % 1000000007;
for (let i = 0; i < 6; i++) hcs = (hcs * 7 + rc[i]) % 1000000007;

console.log(`rows=${n} slow=${slow} top=${top} hist=${hcs} json_bytes=${jbytes} json_cs=${jcs} par=${total}`);
console.log(`parse_us=${parseUs.toFixed(0)} agg_us=${aggUs.toFixed(0)} json_us=${jsonUs.toFixed(0)} par_us=${parUs.toFixed(0)}`);

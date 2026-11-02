"""bench/crazy —— Python 臂：同一份 TSV、同一行结果。"""
import json
import sys
import time

t0 = time.perf_counter()
with open("bench/crazy/data/events.tsv", "rb") as f:
    data = f.read()

rows = []
for line in data.split(b"\n"):
    if not line:
        continue
    a, b, c, d, e, f = line.split(b"\t")
    rows.append((int(a), int(b), int(c), int(d), int(e), int(f)))
n = len(rows)
parse_us = (time.perf_counter() - t0) * 1e6

t0 = time.perf_counter()
groups = {}
kc = [0] * 8
rc = [0] * 6
slow = 0
for ts, uid, kind, region, cents, ms in rows:
    g = groups.get(uid)
    if g is None:
        g = [uid, 0, 0, 0]
        groups[uid] = g
    g[1] += 1
    g[2] += cents
    if ms > g[3]:
        g[3] = ms
    kc[kind] += 1
    rc[region] += cents
    if ms > 1000:
        slow += 1
agg_us = (time.perf_counter() - t0) * 1e6

t0 = time.perf_counter()
gs = sorted(groups.values(), key=lambda g: (-g[2], g[0]))
top = 0
for i in range(min(5, len(gs))):
    top = (top * 1000003 + gs[i][0] * 31 + gs[i][2]) % 1000000007

jbytes = 0
jcs = 0
for i in range(0, n, 16):
    ts, uid, kind, region, cents, ms = rows[i]
    o = {"ts": ts, "uid": uid, "kind": kind, "region": region, "cents": cents, "ms": ms}
    if cents > 15000:
        o["big"] = cents * 2
    s = json.dumps(o, separators=(",", ":"))
    jbytes += len(s)
    jcs = (jcs * 131 + ord(s[-1])) % 1000000007
json_us = (time.perf_counter() - t0) * 1e6

t0 = time.perf_counter()
total = 0
for _ in range(8):
    for ts, uid, kind, region, cents, ms in rows:
        total += cents * (ms + 1)
total %= 1000000007
par_us = (time.perf_counter() - t0) * 1e6

hcs = 0
for v in kc:
    hcs = (hcs * 7 + v) % 1000000007
for v in rc:
    hcs = (hcs * 7 + v) % 1000000007

print(f"rows={n} slow={slow} top={top} hist={hcs} json_bytes={jbytes} json_cs={jcs} par={total}")
print(f"parse_us={parse_us:.0f} agg_us={agg_us:.0f} json_us={json_us:.0f} par_us={par_us:.0f}")

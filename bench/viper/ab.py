import subprocess, time, statistics, os, sys
# usage: bench.py <dir> <label> [args...]
d, label = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
os.chdir(d)
ARMS = [a.split("=") for a in os.environ["ARMS"].split(",")]
REPS = int(os.environ.get("REPS", "11"))
outs = {}; times = {a[0]: [] for a in ARMS}
for r in range(REPS):
    order = ARMS if r % 2 == 0 else list(reversed(ARMS))
    for name, cmd in order:
        t0 = time.perf_counter()
        p = subprocess.run(["taskset","-c","6",cmd] + args, capture_output=True)
        dt = time.perf_counter() - t0
        times[name].append(dt); outs[name] = p.stdout
ref = outs[ARMS[0][0]]
print("shape:", label, "args:", " ".join(args))
for name, _ in ARMS:
    ts = sorted(times[name])
    eq = "SAME" if outs[name] == ref else "DIFF"
    print("  %-14s min=%8.2f ms  med=%8.2f ms  stdout=%s" % (name, ts[0]*1000, statistics.median(ts)*1000, eq))
print("  RSS(kB):")
for name, cmd in ARMS:
    t = subprocess.run(["/usr/bin/time","-f","%M","taskset","-c","6",cmd] + args, capture_output=True)
    print("    %-14s %s" % (name, t.stderr.decode().strip().splitlines()[-1]))

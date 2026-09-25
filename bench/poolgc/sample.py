import subprocess, sys, time
def run(path):
    p = subprocess.Popen([path], stdout=subprocess.PIPE, text=True)
    peak = 0; last = 0; samples = []
    while p.poll() is None:
        try:
            with open(f"/proc/{p.pid}/statm") as f:
                rss = int(f.read().split()[1]) * 4      # 页 → KB (4 KiB 页)
        except FileNotFoundError:
            break
        peak = max(peak, rss); last = rss; samples.append(rss)
        time.sleep(0.02)
    out = p.stdout.read().strip()
    return peak, last, out
for tag in sys.argv[1:]:
    peak, last, out = run(f"/tmp/prb/gc/{tag}.bin")
    print(f"{tag:12s} 峰值 {peak:7d} KB · 结束前 {last:7d} KB · {out}")

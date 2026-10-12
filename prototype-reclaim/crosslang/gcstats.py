import sys, re, subprocess, os
def dist(ps):
    ps=sorted(ps); n=len(ps)
    if not n: return None
    q=lambda p: ps[min(n-1,int(n*p))]
    return (q(.5),q(.9),q(.99),ps[-1],n)
def go_dist(binary, N, gogc):
    env=dict(os.environ, GODEBUG="gctrace=1", GOGC=str(gogc))
    out=subprocess.run([binary,str(N)],capture_output=True,text=True,env=env).stderr
    ps=[]
    for m in re.finditer(r':\s*([0-9.]+)\+([0-9.]+)\+([0-9.]+)\s*ms clock', out):
        ps.append((float(m.group(1))+float(m.group(3)))*1000.0)   # X + Z = 两次 STW
    return dist(ps), ps
for tag,binary,N in (("压力版","./stress_go",2000000), ("轻负载","./bench_go",2000000)):
    for gogc in (100, 20, 5):
        d,ps = go_dist(binary,N,gogc)
        if d: print(f"Go {tag} GOGC={gogc:<3} p50={d[0]:7.1f} p90={d[1]:7.1f} p99={d[2]:7.1f} max={d[3]:8.1f} n={d[4]}")

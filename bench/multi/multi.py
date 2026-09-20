import numpy as np, time
def t(f, r=3):
    best = 9e9
    for _ in range(r):
        t0 = time.perf_counter(); f(); best = min(best, time.perf_counter()-t0)
    return best*1000
np.seterr(all='ignore')
def mod():
    i = np.arange(30000000, dtype=np.int64)
    return int(((i % 7) + (i % 1000003)).sum())
def sieve():
    n = 10000000; comp = np.zeros(n+1, dtype=bool)
    for i in range(2, int(n**0.5)+1):
        if not comp[i]: comp[i*i::i] = True
    return int(n - 1 - comp[2:].sum())
def matmul():
    n = 512
    a = np.full((n,n), 1, dtype=np.float32); b = np.full((n,n), 2, dtype=np.float32)
    return (a @ b)[0,0]
print("mod 3e7    %.1f ms" % t(mod))
print("sieve 1e7  %.1f ms" % t(sieve))
print("matmul 512 %.1f ms" % t(matmul))

# Python：原生 generator（每次 next() = 一次协程推进）
import sys
def gen(id, n, salt):
    for i in range(n):
        yield (i % 65536) * 65521 + id * 40503 + salt
def run(k, m, salt):
    s = 0
    for id in range(k):
        for v in gen(id, m, salt):
            s += v
    return s
t = run(8, 2000, 0)
for r in range(R_ROUNDS):
    t += run(512, 20000, r)
print(t)

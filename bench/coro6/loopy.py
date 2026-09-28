def run(k, m):
    s = 0
    for id in range(k):
        for i in range(m):
            s += (i % 65536) * 65521 + id * 40503
    return s
t = run(8, 2000)
for _ in range(1):
    t += run(512, 20000)
print(t)

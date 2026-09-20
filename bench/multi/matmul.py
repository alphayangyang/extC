n = 64; N = n*n
a = [1]*N; b = [2]*N; c = [0]*N
for x in range(n):
    for y in range(n):
        s = 0
        for k in range(n): s += a[x*n+k] * b[k*n+y]
        c[x*n+y] = s
print("c[0] =", c[0])

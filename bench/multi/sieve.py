n = 1000000; comp = bytearray(n+1); cnt = 0
for i in range(2, n+1):
    if not comp[i]:
        cnt += 1
        for j in range(i*i, n+1, i): comp[j] = 1
print("pi =", cnt)

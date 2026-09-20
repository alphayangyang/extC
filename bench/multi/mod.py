s = 0
for i in range(3000000): s += (i % 7) + (i % 1000003)
print("s =", s)

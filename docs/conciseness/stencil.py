# 语料 T3：一维 3 点平滑迭代 10 轮，输出总和（整数除法，边界不动）。
a = [(i * 13 + 5) % 17 for i in range(64)]
b = [0] * 64
for _ in range(10):
    for k in range(1, 63):
        b[k] = (a[k - 1] + a[k] + a[k + 1]) // 3
    a, b = b, a
print(f"sum={sum(a)}")

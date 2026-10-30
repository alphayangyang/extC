# 语料 T2：3x3 整数矩阵乘法，输出对角线之和。
a = [[((r * 3 + j) * 5 + 1) % 7 for j in range(3)] for r in range(3)]
b = [[((r * 3 + j) * 3 + 2) % 5 for j in range(3)] for r in range(3)]
c = [[sum(a[r][j] * b[j][k] for j in range(3)) for k in range(3)] for r in range(3)]
print(f"diag={c[0][0] + c[1][1] + c[2][2]}")

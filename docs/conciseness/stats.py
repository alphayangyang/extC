# 语料 T1：一维整数数组的统计（求和/最小/最大/平均的整数部分）。
v = [(i * 7 + 3) % 23 for i in range(16)]
sum_ = sum(v)
print(f"sum={sum_} min={min(v)} max={max(v)} avg={sum_ // 16}")

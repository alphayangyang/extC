# 语料 T4：统计一段文本的行数/词数/字符数（词 = 非空白连续段）。
text = "the quick brown fox\njumps over the lazy dog\n\nand keeps running fast\n"
print(f"lines={text.count(chr(10))} words={len(text.split())} chars={len(text)}")

# 同一份功能：有界符号表（64 桶 × 每桶≤8，满则淘汰尾节点）+ 1024 槽缓存 + 20 万项索引 + 查询。
# 有意思的是 CPython 的内存管理策略与本设计最终版**相同**：引用计数归零即释放 + 环由追踪 GC 兜底。
import sys, time, gc

BUCKETS, BUCKET_MAX, CACHE, INDEXMAX = 64, 8, 1024, 200000

class Node:
    __slots__ = ('key', 'val', 'next')
    def __init__(self, key, val, nxt):
        self.key = key; self.val = val; self.next = nxt

def main():
    n = int(sys.argv[1])
    head  = [None] * BUCKETS
    size  = [0] * BUCKETS
    cache = [None] * CACHE
    idx   = []
    sink  = 0
    pause = [0.0]; mark = [0.0]; ncyc = [0]
    def cb(phase, info):
        if phase == 'start': mark[0] = time.perf_counter()
        else:
            d = time.perf_counter() - mark[0]
            if d > pause[0]: pause[0] = d
            ncyc[0] += 1
    gc.callbacks.append(cb)
    t0 = time.perf_counter()
    for i in range(n):
        b = i % BUCKETS
        # 解析：3 个临时值是栈上的算术，不分配对象
        sink += i + (i + 1) + (i + 2)
        # 插入桶（共享引用：桶 + 缓存 + 索引）
        nd = Node(i, i * 3, head[b])
        head[b] = nd
        if size[b] == 0: pass
        size[b] += 1
        cache[i % CACHE] = nd
        if i < INDEXMAX: idx.append(nd)
        # 查询
        q = head[b]
        while q is not None:
            if q.key == i - 7: q.val += 1; break
            q = q.next
        # 淘汰尾节点
        if size[b] > BUCKET_MAX:
            q = head[b]
            while q.next is not None and q.next.next is not None: q = q.next
            if q.next is not None: q.next = None
            else: head[b] = None
            size[b] -= 1
    dt = time.perf_counter() - t0
    print(f"python  耗时={dt*1000:.1f}ms sink={sink} idx={len(idx)} 环GC次数={ncyc[0]} 环GC最大停顿us={pause[0]*1e6:.1f}")

main()

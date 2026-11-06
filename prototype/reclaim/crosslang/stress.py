# 压力形状：双向链表(LRU) + 带父指针的树 + 缓存 + 索引 —— 天然形成环。
import sys, time, gc
class Node:
    __slots__ = ('key','val','next','prev','parent','child')
    def __init__(self, k, v):
        self.key=k; self.val=v; self.next=None; self.prev=None; self.parent=None; self.child=None
def main():
    n = int(sys.argv[1])
    head=[None]*64; tail=[None]*64; size=[0]*64; cache=[None]*1024
    idx=[]; nodes=[]; pause=[0.0]; mark=[0.0]; ncyc=[0]
    def cb(phase, info):
        if phase=='start': mark[0]=time.perf_counter()
        else:
            d=time.perf_counter()-mark[0]
            if d>pause[0]: pause[0]=d
            ncyc[0]+=1
    gc.callbacks.append(cb)
    t0=time.perf_counter()
    for i in range(n):
        b=i%64
        nd=Node(i, i*3)
        if head[b] is not None: head[b].prev=nd
        nd.next=head[b]; head[b]=nd
        if tail[b] is None: tail[b]=nd
        size[b]+=1
        cache[i%1024]=nd
        if i<200000: idx.append(nd)
        if (i & 15)==0:
            nodes.append(nd)
            if len(nodes)>1:
                par=nodes[-2]; par.child=nd; nd.parent=par
        q=head[b]
        while q is not None:
            if q.key==i-7: q.val+=1; break
            q=q.next
        if size[b]>8 and tail[b] is not None:
            pr=tail[b].prev
            if pr is not None: pr.next=None; tail[b]=pr
            else: head[b]=None; tail[b]=None
            size[b]-=1
    dt=time.perf_counter()-t0
    print(f"python-stress 耗时={dt*1000:.1f}ms idx={len(idx)} 环GC次数={ncyc[0]} 环GC最大停顿us={pause[0]*1e6:.1f}")
main()

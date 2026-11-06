// 同一份功能：有界符号表（64 桶 × 最多 8 条，满则淘汰尾节点）+ 1024 槽缓存 + 20 万项索引 + 查询。
public class Bench {
    static final int BUCKETS = 64, BUCKET_MAX = 8, CACHE = 1024, INDEXMAX = 200000;
    static class Node { long key, val; Node next; }
    static class Idx  { Idx next; Node target; }
    public static void main(String[] args) {
        int N = Integer.parseInt(args[0]);
        Node[] head = new Node[BUCKETS];
        Node[] cache = new Node[CACHE];
        Idx ihead = null;
        long sink = 0;
        long t0 = System.nanoTime();
        for (int i = 0; i < N; i++) {
            int b = i % BUCKETS;
            Node t1 = new Node(), t2 = new Node(), t3 = new Node();
            t1.next = t2; t2.next = t3; t3.val = i;
            for (Node q = t1; q != null; q = q.next) sink += q.val;
            Node nd = new Node(); nd.key = i; nd.val = (long) i * 3;
            nd.next = head[b]; head[b] = nd;
            cache[i % CACHE] = nd;
            if (i < INDEXMAX) { Idx ix = new Idx(); ix.next = ihead; ix.target = nd; ihead = ix; }
            for (Node q = head[b]; q != null; q = q.next) if (q.key == i - 7) { q.val++; break; }
            Node prev = null, cur = head[b]; int cnt = 0;
            while (cur != null && cur.next != null) { prev = cur; cur = cur.next; if (++cnt > BUCKET_MAX) break; }
            if (cur != null && cnt >= BUCKET_MAX) { if (prev != null) prev.next = null; else head[b] = null; }
        }
        long dt = System.nanoTime() - t0;
        System.out.printf("java    耗时=%.1fms sink=%d%n", dt / 1e6, sink + (ihead == null ? 0 : 1) + cache[0].key);
    }
}

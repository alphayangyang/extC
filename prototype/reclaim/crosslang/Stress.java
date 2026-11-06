// 压力形状：双向链表(LRU) + 带父指针的树 + 缓存 + 索引 —— 天然形成环。
public class Stress {
    static class Node { long key, val; Node next, prev, parent, child; }
    public static void main(String[] a) {
        int N = Integer.parseInt(a[0]);
        Node[] head = new Node[64], tail = new Node[64], cache = new Node[1024];
        int[] size = new int[64];
        Node[] idx = new Node[200000]; Node[] nodes = new Node[1 << 20];
        int ni = 0, nn = 0; long sink = 0;
        long t0 = System.nanoTime();
        for (int i = 0; i < N; i++) {
            int b = i % 64;
            Node nd = new Node(); nd.key = i; nd.val = (long) i * 3;
            if (head[b] != null) head[b].prev = nd;
            nd.next = head[b]; head[b] = nd;
            if (tail[b] == null) tail[b] = nd;
            size[b]++;
            cache[i % 1024] = nd;
            if (i < 200000) idx[ni++] = nd;
            if ((i & 15) == 0) { nodes[nn++] = nd; if (nn > 1) { Node par = nodes[nn - 2]; par.child = nd; nd.parent = par; } }
            for (Node q = head[b]; q != null; q = q.next) if (q.key == i - 7) { q.val++; break; }
            if (size[b] > 8 && tail[b] != null) { Node pr = tail[b].prev;
                if (pr != null) { pr.next = null; tail[b] = pr; } else { head[b] = null; tail[b] = null; } size[b]--; }
        }
        long dt = System.nanoTime() - t0;
        System.out.printf("java-stress 耗时=%.1fms idx=%d%n", dt / 1e6, ni + (sink == 0 ? 0 : 1));
    }
}

// bench/gc/v2 形状 G4 cycleratio —— Java 自然写法：单链节点 + 256 槽定长窗口（无库容器）。
// 与 bench/gc/v2/src/cycleratio.extc 逐位同 PRNG、同 LCG 调用顺序、同 stdout（SPEC §2/§3）。
// 构建：javac -d $B/classes bench/gc/v2/src/cycleratio.java
// 运行：java -cp $B/classes cycleratio [G=400000] [seed=20261013] [f=100]
public final class cycleratio {
    static final long MASK = 4611686018427387903L;
    static final int WINDOW = 256;

    // 节点：{ k, v, next 可空引用 }
    static final class Node {
        long k;
        long v;
        Node next;
    }

    static long gstate = 1;

    static long nxt() {
        gstate = (gstate * 1103515245L + 12345L) & 2147483647L;
        return gstate;
    }

    static long pick(long k) {
        return (nxt() / 1024) % k;
    }

    static long parseI64(String s) {
        long v = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') break;
            v = v * 10 + (c - '0');
        }
        return v;
    }

    public static void main(String[] args) {
        long g = 400000;
        if (args.length > 0) g = parseI64(args[0]);
        long sd = 20261013;
        if (args.length > 1) sd = parseI64(args[1]);
        long f = 100;
        if (args.length > 2) f = parseI64(args[2]);
        gstate = sd & 2147483647L;

        Node[] winS = new Node[WINDOW]; // 每槽：组头节点
        long[] winN = new long[WINDOW]; // 每槽：该组节点数
        long sum = 0, made = 0, evicted = 0, dropnodes = 0, rings = 0;
        for (long i = 0; i < g; i++) {
            long k = 3 + pick(4);
            boolean isring = pick(100) < f;
            Node first = null, prev = null;
            for (long j = 0; j < k; j++) {
                Node s = new Node();
                s.k = i;
                s.v = nxt() % 1000;
                s.next = null;
                if (first == null) first = s;
                if (prev != null) prev.next = s;
                prev = s;
                sum = (sum + s.v) & MASK;
            }
            if (isring) {
                rings++;
                prev.next = first; // 闭成环：整组在窗口里只有 first 可达（成环时依然可达）
            }
            made += k;
            int b = (int) (i % WINDOW);
            Node old = winS[b];
            if (old != null) {
                evicted++;
                dropnodes += winN[b];
            }
            winS[b] = first;
            winN[b] = k;
        }
        System.out.print("groups=" + g + " made=" + made + " evicted=" + evicted
                + " dropnodes=" + dropnodes + " sum=" + sum + "\n");
        System.out.print("f=" + f + " window=256 kmin=3 kmax=6 rings=" + rings + "\n");
    }
}

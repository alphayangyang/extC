// bench/gc/v2 形状 G3 chase —— Java 自然写法：节点数组 + 每节点 4 出边数组（无库容器）。
// 与 bench/gc/v2/src/chase.extc 逐位同 PRNG、同 LCG 调用顺序、同 stdout（SPEC §2/§3）。
// 构建：javac -d $B/classes bench/gc/v2/src/chase.java
// 运行：java -cp $B/classes chase [M=1048576] [seed=20261013] [W=4000000]
public final class chase {
    static final long MASK = 4611686018427387903L;
    static final int CAP = 1048576, OUTDEG = 4, CHURN_EVERY = 256, CHURN = 4;

    // 节点：{ k, v, o[4] 可空引用 }
    static final class Node {
        long k;
        long v;
        Node[] o = new Node[OUTDEG];
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
        long m = 1048576;
        if (args.length > 0) m = parseI64(args[0]);
        long sd = 20261013;
        if (args.length > 1) sd = parseI64(args[1]);
        long w = 4000000;
        if (args.length > 2) w = parseI64(args[2]);
        gstate = sd & 2147483647L;

        Node[] graph = new Node[CAP]; // 全空
        for (long i = 0; i < m; i++) {
            Node x = new Node();
            x.k = i;
            x.v = nxt() % 1000;
            graph[(int) i] = x;
        }
        for (long i = 0; i < m; i++) {
            Node src = graph[(int) i];
            if (src != null) {
                for (int j = 0; j < OUTDEG; j++) {
                    long tgt = nxt() % m;
                    src.o[j] = graph[(int) tgt];
                }
            }
        }

        Node cur = graph[0];
        long sum = 0, churn = 0, tailv = 0;
        for (long step = 0; step < w; step++) {
            if (cur != null) sum = (sum + cur.v) & MASK;
            long r = pick(4);
            if (cur != null) cur = cur.o[(int) r];
            if (step % CHURN_EVERY == 0) {
                Node h = null;
                for (int j = 0; j < CHURN; j++) {
                    Node s = new Node();
                    s.k = step;
                    s.v = nxt() % 1000;
                    s.o[0] = h;
                    h = s;
                }
                // 4 个即抛节点每批都真实分配：s.o[0] = h 是数组元素存储，C2 会保守判定逃逸；
                // tailv 是 SPEC §2 G3 规定的尾端读回。
                tailv = h.v;
                churn += CHURN;
            }
        }
        System.out.print("nodes=" + m + " edges=" + (m * 4) + " walk=" + w + " churn=" + churn
                + " sum=" + sum + " tailv=" + tailv + "\n");
        System.out.print("outdeg=4 cap=1048576 churn_every=256 churn=4\n");
    }
}

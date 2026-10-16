// bench/gc/v2 形状 G1 promote —— Java 自然写法：对象链 + 定长引用数组表（无库容器）。
// 与 bench/gc/v2/src/promote.extc 逐位同 PRNG、同 LCG 调用顺序、同 stdout（SPEC §2/§3）。
// 构建：javac -d $B/classes bench/gc/v2/src/promote.java
// 运行：java -cp $B/classes promote [N=500000] [seed=20261013]
public final class promote {
    static final long MASK = 4611686018427387903L;
    static final int TABLE = 65536, SHORT = 9, EVERY = 10, WALKCAP = 8;

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
        long n = 500000;
        if (args.length > 0) n = parseI64(args[0]);
        long sd = 20261013;
        if (args.length > 1) sd = parseI64(args[1]);
        gstate = sd & 2147483647L;

        Node[] table = new Node[TABLE]; // 65536 槽，全空
        long sum = 0, walksum = 0, live = 0, tailv = 0;
        for (long i = 0; i < n; i++) {
            Node head = null;
            for (int j = 0; j < SHORT; j++) {
                Node s = new Node();
                s.k = i * SHORT + j;
                s.v = nxt() % 1000;
                s.next = head;
                head = s;
                sum = (sum + s.v) & MASK;
            }
            // 9 个短命节点每轮都真实分配：链在循环里构造成环，且下一轮的头插会引用上一轮的链，
            // C2 逃逸分析对这种链式图不做标量替换（分配可观察性见交付报告）。
            tailv = head.v;     // 尾端读回（SPEC §2 G1）
            if (i % EVERY == 0) {
                Node p = new Node();
                p.k = i;
                p.v = nxt() % 1000;
                int b = (int) (i % TABLE);
                p.next = table[b];
                table[b] = p;
                live++;
            }
            int b2 = (int) ((i * 40503) % TABLE);
            Node c = table[b2];
            int cnt = 0;
            while (c != null && cnt < WALKCAP) {
                walksum = (walksum + c.v) & MASK;
                c = c.next;
                cnt++;
            }
        }
        System.out.print("rounds=" + n + " promoted=" + live + " walksum=" + walksum
                + " sum=" + sum + " tailv=" + tailv + "\n");
        System.out.print("table=65536 short=9 every=10 walkcap=8\n");
    }
}

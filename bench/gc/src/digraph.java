// bench/gc 形状 7：digraph —— Java（JDK 25，追踪 GC；第一次 JIT 家族）。
// 与 bench/gc/src/digraph.extc **逐字同算法/同 PRNG/同顺序**，stdout 必须逐字节一致。
//
// 移植要点（三个坑，见 task-23）：
// 1) LCG 必须 mod 2³¹ ⇒ 用 long 算 `(gstate*1103515245 + 12345) & 2147483647`（与参照实现逐位相同；
//    用 int 回绕再掩码的低 31 位也一样，这里取更不易错的那种）。
// 2) mixv 掩到 62 位 ⇒ Java 有 64 位 long，精确 ✓（不像 JS 要拆 32 位）。
// 3) `x >> 12` 对**正数** long 就是算术右移 ✓（值恒 < 2^62，符号位不参与）。
//
// 预热（WARM=<n>，默认 0 ⇒ 行为与单跑逐字相同）：跑 n+1 遍整个负载，只打印**最后一遍**的结果；
// WARM_STATS=1 时把每遍耗时写 stderr（冷=第 0 遍，暖=最后一遍）。
//
// 构建：javac -d build/classes src/digraph.java   运行：java -cp build/classes digraph N seed
import java.util.Locale;

public final class digraph {
    static final long MASK = 4611686018427387903L;

    static final class Edge {
        Node to;
        Edge next;
        long kind;
    }

    static final class Node {
        long id;
        long gid;
        long payload;
        Edge first;
        Node ord;
        Node back;   // 命名回指（Java 是追踪 GC：这里只是普通引用）
        long mark;
        long cmark;
        Node snext;
        boolean gpin;
    }

    static final class Handle {
        Node target;
        Node newest;
        long size;
        Handle prev;
        Handle next;
    }

    static final class Ctx {
        Handle head;
        Handle tail;
        long n;
        Node[] hubs = new Node[8];
        long hubCount;
    }

    static long gstate = 1;
    static long nextId = 0;
    static long gidCounter = 0;
    static long backedges = 0;
    static long nodesM = 0;
    static long groupsM = 0;
    static long droppedG = 0;
    static long droppedN = 0;
    static long rejected = 0;
    static long cyclesDropped = 0;
    static long acc = 0;
    static long xacc = 0;
    static long lastNodes = 0;
    static long lastEdges = 0;
    static long epoch = 0;
    static long cepoch = 0;

    static long nxt() {
        gstate = (gstate * 1103515245L + 12345L) & 2147483647L;
        return gstate;
    }

    static long mixv(long a, long b) {
        long x = (a * 2654435761L + b * 40503L + 12345L) & MASK;
        x = x ^ (x >> 12);
        return x & MASK;
    }

    static long parseNum(String s, long def) {
        if (s == null || s.isEmpty()) return def;
        long v = 0;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch < '0' || ch > '9') return def;
            v = v * 10 + (ch - '0');
        }
        return v;
    }

    static Handle handleAt(Ctx c, long idx) {
        Handle h = c.head;
        for (long i = 0; i < idx && h != null; i++) h = h.next;
        return h;
    }

    static Node nodeInGroup(Handle h, long j) {
        Node x = h.newest;
        for (long i = 0; i < j && x != null; i++) x = x.ord;
        return x;
    }

    static void addEdge(Node s, Node t, long k) {
        Edge e = new Edge();
        e.to = t;
        e.next = s.first;
        e.kind = k;
        s.first = e;
    }

    // 组内“更老”（id 更小）的一个随机祖先；没有就退回 hub
    static Node olderInGroup(Node s, Node hub) {
        long c1 = 0;
        for (Node y = s.ord; y != null; y = y.ord) c1++;
        if (c1 == 0) return hub;
        long j = nxt() % c1;
        Node z = s.ord;
        long i2 = 0;
        while (z != null) {
            if (i2 == j) return z;
            i2++;
            z = z.ord;
        }
        return hub;
    }

    // 组内“更新”（id 更大）的一个随机后代；没有就退回 hub
    static Node newerInGroup(Node s, Node newest, Node hub) {
        long c2 = 0;
        for (Node y = newest; y != null; y = y.ord) {
            if (y.id > s.id) c2++;
        }
        if (c2 == 0) return hub;
        long j = nxt() % c2;
        Node z = newest;
        long i2 = 0;
        while (z != null) {
            if (z.id > s.id) {
                if (i2 == j) return z;
                i2++;
            }
            z = z.ord;
        }
        return hub;
    }

    // 建组时的弦边目标：60% 组内更老 / 20% 组内更新 / 10% 组内 hub / 10% 组内长程随机
    static Node pickChordTarget(Node s, Node hub, Node newest, long size) {
        long r = nxt() % 100;
        if (r < 60) return olderInGroup(s, hub);
        if (r < 80) return newerInGroup(s, newest, hub);
        if (r < 90) return hub;
        long j3 = nxt() % size;
        Node z3 = newest;
        for (long i4 = 0; z3 != null; i4++) {
            if (i4 == j3) return z3;
            z3 = z3.ord;
        }
        return hub;
    }

    // 组内是否存在有向环（丢弃时现场验证；只用组内边，独立于“构造上必有环”）
    static boolean groupHasCycle(Node newest, long gid) {
        for (Node u = newest; u != null; u = u.ord) {
            cepoch++;
            long ep = cepoch;
            Node top = null;
            for (Edge e = u.first; e != null; e = e.next) {
                Node t = e.to;
                if (t.gid == gid) {
                    if (t.id == u.id) return true;
                    if (t.cmark != ep) {
                        t.cmark = ep;
                        t.snext = top;
                        top = t;
                    }
                }
            }
            boolean found = false;
            while (top != null) {
                Node x = top;
                top = x.snext;
                x.snext = null;
                if (x.id == u.id) found = true;
                if (!found) {
                    for (Edge e2 = x.first; e2 != null; e2 = e2.next) {
                        Node t2 = e2.to;
                        if (t2.gid == gid && t2.cmark != ep) {
                            t2.cmark = ep;
                            t2.snext = top;
                            top = t2;
                        }
                    }
                }
            }
            if (found) return true;
        }
        return false;
    }

    static void createGroup(Ctx c, long k, boolean pin, boolean isHub) {
        long gid = gidCounter;
        gidCounter++;
        groupsM++;

        Node hub = new Node();
        hub.id = nextId;
        hub.gid = gid;
        hub.payload = nxt() % 1000;
        hub.gpin = pin;
        nextId++;
        nodesM++;
        if (isHub) {
            c.hubs[0] = hub;
            c.hubCount = 1;
        }

        Node prev = hub;
        for (long cnt = 1; cnt < k; cnt++) {
            Node n = new Node();
            n.id = nextId;
            n.gid = gid;
            n.payload = nxt() % 1000;
            n.ord = prev;
            n.back = prev;
            n.gpin = pin;
            nextId++;
            nodesM++;
            if (isHub) {
                c.hubs[(int) cnt] = n;
                c.hubCount = cnt + 1;
            }
            addEdge(prev, n, 1);
            prev = n;
        }

        addEdge(prev, hub, 1);
        backedges++;

        for (Node x = prev; x != null; x = x.ord) {
            long d = nxt() % 3;
            for (long j = 0; j < d; j++) {
                Node tgt = pickChordTarget(x, hub, prev, k);
                addEdge(x, tgt, 2);
            }
        }

        Handle h = new Handle();
        h.target = hub;
        h.newest = prev;
        h.size = k;
        h.prev = c.tail;
        if (c.tail != null) c.tail.next = h;
        else c.head = h;
        c.tail = h;
        c.n++;
    }

    // 丢一个组：(c) 步。优先摘无跨组入边（gpin==false）的组；顺手做现场环验证与计数。
    static void tryDrop(Ctx c) {
        if (c.n == 0) return;
        long base = nxt() % c.n;
        for (long probe = 0; probe < 8; probe++) {
            long idx = base + probe;
            while (idx >= c.n) idx -= c.n;
            Handle h = handleAt(c, idx);
            long cnt = 0;
            boolean anypin = false;
            for (Node x = h.newest; x != null; x = x.ord) {
                cnt++;
                if (x.gpin) anypin = true;
            }
            if (anypin) {
                rejected++;
                continue;
            }
            droppedG++;
            droppedN += cnt;
            if (groupHasCycle(h.newest, h.target.gid)) cyclesDropped++;
            Handle hp = h.prev;
            Handle hn = h.next;
            if (hp != null) hp.next = hn;
            else c.head = hn;
            if (hn != null) hn.prev = hp;
            else c.tail = hp;
            h.prev = null;
            h.next = null;
            c.n--;
            return;
        }
    }

    // (d) 步：从 handle 表出发对存活部分做一次遍历（显式栈，用 snext 串），累加校验和。
    static void traverse(Ctx c, long ep) {
        Node top = null;
        for (Handle h = c.head; h != null; h = h.next) {
            Node t = h.target;
            if (t.mark != ep) {
                t.mark = ep;
                t.snext = top;
                top = t;
            }
        }
        long rn = 0;
        long re = 0;
        long radd = 0;
        long rxor = 0;
        while (top != null) {
            Node n = top;
            top = n.snext;
            n.snext = null;
            rn++;
            radd = (radd + mixv(n.id, n.payload)) & MASK;
            for (Edge e = n.first; e != null; e = e.next) {
                re++;
                rxor = rxor ^ mixv(n.id, e.to.id) ^ (e.kind * 4099);
                Node t2 = e.to;
                if (t2.mark != ep) {
                    t2.mark = ep;
                    t2.snext = top;
                    top = t2;
                }
            }
        }
        acc = (acc + radd + rn * 1000003 + re * 31) & MASK;
        xacc = (xacc ^ (rxor ^ (rn * 1000003 + re))) & MASK;
        lastNodes = rn;
        lastEdges = re;
    }

    static void resetAll() {
        gstate = 1;
        nextId = 0;
        gidCounter = 0;
        backedges = 0;
        nodesM = 0;
        groupsM = 0;
        droppedG = 0;
        droppedN = 0;
        rejected = 0;
        cyclesDropped = 0;
        acc = 0;
        xacc = 0;
        lastNodes = 0;
        lastEdges = 0;
        epoch = 0;
        cepoch = 0;
    }

    static void runOnce(long N, long sd) {
        resetAll();
        gstate = sd & 2147483647L;

        Ctx c = new Ctx();
        createGroup(c, 8, true, true);

        long rounds = 16 + N / 4096;
        long batch = N / rounds;
        if (batch < 24) batch = 24;

        for (long round = 0; round < rounds; round++) {
            // (a) 建
            long created = 0;
            long groupsThisRound = 0;
            while (created < batch) {
                long k = 3 + nxt() % 4;
                createGroup(c, k, false, false);
                created += k;
                groupsThisRound++;
            }
            // (b) 改：删头边 + 按 60/20/10/10 加边
            long mOps = 2 + nxt() % 3;
            for (long m = 0; m < mOps; m++) {
                if (c.n > 0) {
                    long idx = nxt() % c.n;
                    Handle h = handleAt(c, idx);
                    Node s = nodeInGroup(h, nxt() % h.size);
                    Edge f = s.first;
                    if (f != null && f.next != null) s.first = f.next;
                    long r = nxt() % 100;
                    Node tgt = null;
                    if (r < 60) {
                        tgt = olderInGroup(s, h.target);
                    } else if (r < 80) {
                        tgt = newerInGroup(s, h.newest, h.target);
                    } else if (r < 90) {
                        if (c.hubCount > 0) {
                            long hj = nxt() % c.hubCount;
                            tgt = c.hubs[(int) hj];
                        }
                    } else {
                        long idx2 = nxt() % c.n;
                        Handle h2 = handleAt(c, idx2);
                        tgt = nodeInGroup(h2, nxt() % h2.size);
                    }
                    if (tgt != null) {
                        if (tgt.gid != s.gid) tgt.gpin = true;
                        addEdge(s, tgt, 3);
                    }
                }
            }
            // (c) 丢
            for (long d = 0; d < groupsThisRound; d++) tryDrop(c);
            // (d) 遍历
            epoch++;
            traverse(c, epoch);
        }
    }

    public static void main(String[] args) {
        long N = 200000;
        long sd = 20261013;
        if (args.length > 0) N = parseNum(args[0], N);
        if (args.length > 1) sd = parseNum(args[1], sd);
        if (N < 256) N = 256;

        int warm = (int) parseNum(System.getenv("WARM"), 0);
        if (warm < 0) warm = 0;
        boolean warmStats = System.getenv("WARM_STATS") != null;
        long[] iters = new long[warm + 1];
        for (int i = 0; i <= warm; i++) {
            long t0 = System.nanoTime();
            runOnce(N, sd);
            iters[i] = System.nanoTime() - t0;
        }
        if (warmStats) {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < iters.length; i++) {
                if (i > 0) sb.append(',');
                sb.append(String.format(Locale.ROOT, "%.3f", iters[i] / 1e6));
            }
            System.err.printf("WARMSTAT warm=%d iters_ms=%s%n", warm, sb.toString());
        }

        long total = (acc + xacc) & MASK;
        System.out.printf("nodes=%d edges=%d sum=%d%n", lastNodes, lastEdges, total);
        System.out.printf(
            "backedges=%d groups_made=%d nodes_made=%d dropped_groups=%d dropped_nodes=%d "
                + "rejected_external=%d cycles_dropped=%d%n",
            backedges, groupsM, nodesM, droppedG, droppedN, rejected, cyclesDropped);
    }
}

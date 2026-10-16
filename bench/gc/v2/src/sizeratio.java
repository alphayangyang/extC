// bench/gc/v2 形状 G2 sizeratio —— Java 自然写法：4 个尺寸类 + 每类 1024 槽定长引用数组（无库容器）。
// 与 bench/gc/v2/src/sizeratio.extc 逐位同 PRNG、同 LCG 调用顺序、同 stdout（SPEC §2/§3）。
// 构建：javac -d $B/classes bench/gc/v2/src/sizeratio.java
// 运行：java -cp $B/classes sizeratio [N=2000000] [seed=20261013]
public final class sizeratio {
    static final long MASK = 4611686018427387903L;

    // 4 个尺寸类：k(8B) + p[3|7|11|15] -> 32/64/96/128 字节
    static final class C0 { long k; long[] p = new long[3]; }
    static final class C1 { long k; long[] p = new long[7]; }
    static final class C2 { long k; long[] p = new long[11]; }
    static final class C3 { long k; long[] p = new long[15]; }

    static final class Table {
        C0[] a0 = new C0[1024];
        C1[] a1 = new C1[1024];
        C2[] a2 = new C2[1024];
        C3[] a3 = new C3[1024];
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
        long n = 2000000;
        if (args.length > 0) n = parseI64(args[0]);
        long sd = 20261013;
        if (args.length > 1) sd = parseI64(args[1]);
        gstate = sd & 2147483647L;

        Table t = new Table();
        long sum = 0, kept = 0, livebytes = 0;
        for (long i = 0; i < n; i++) {
            long cls = pick(4);
            if (cls == 0) {
                C0 o = new C0();
                o.k = i;
                sum = (sum + o.k) & MASK;
                if (i % 8 == 0) {
                    int b = (int) ((i / 8) % 1024);
                    C0 old = t.a0[b];
                    if (old != null) livebytes -= 32; else kept++;
                    t.a0[b] = o;
                    livebytes += 32;
                }
            } else if (cls == 1) {
                C1 o = new C1();
                o.k = i;
                sum = (sum + o.k) & MASK;
                if (i % 8 == 0) {
                    int b = (int) ((i / 8) % 1024);
                    C1 old = t.a1[b];
                    if (old != null) livebytes -= 64; else kept++;
                    t.a1[b] = o;
                    livebytes += 64;
                }
            } else if (cls == 2) {
                C2 o = new C2();
                o.k = i;
                sum = (sum + o.k) & MASK;
                if (i % 8 == 0) {
                    int b = (int) ((i / 8) % 1024);
                    C2 old = t.a2[b];
                    if (old != null) livebytes -= 96; else kept++;
                    t.a2[b] = o;
                    livebytes += 96;
                }
            } else {
                C3 o = new C3();
                o.k = i;
                sum = (sum + o.k) & MASK;
                if (i % 8 == 0) {
                    int b = (int) ((i / 8) % 1024);
                    C3 old = t.a3[b];
                    if (old != null) livebytes -= 128; else kept++;
                    t.a3[b] = o;
                    livebytes += 128;
                }
            }
        }
        System.out.print("ops=" + n + " kept=" + kept + " livebytes=" + livebytes + " sum=" + sum + "\n");
        System.out.print("classes=4 sizes=32..128 slots=1024\n");
    }
}

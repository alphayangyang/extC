public final class SpectrumP5 {
    static final long MASK = 4611686018427387903L; static long gstate = 1;
    static long nxt() { gstate = (gstate * 1103515245L + 12345L) & 2147483647L; return gstate; }
    static final class Ent { long k, v; Ent next; Ent(long a, long b, Ent c){k=a;v=b;next=c;} }
    static final class Idx { Ent[] buckets = new Ent[1024]; long n, hit, ins; }
    static final class Win { Ent[] slots = new Ent[256]; }
    static void idxPut(Idx x, long k, long v) { int h = (int)((k * 2654435761L) & 1023L); x.buckets[h] = new Ent(k, v, x.buckets[h]); x.n++; x.ins++; }
    static long idxGet(Idx x, long k) { int h = (int)((k * 2654435761L) & 1023L); for (Ent c = x.buckets[h]; c != null; c = c.next) { if (c.k == k) { x.hit++; return c.v; } } return -1; }

    // ---- 应用侧单位时间戳（USER-STALL）：只写 stderr，stdout 不变 ----
    static long[] usSt; static int usN; static long usBase;
    static void usInit(long cap) { usSt = new long[(int) cap]; usN = 0; usBase = System.nanoTime(); }
    static void us() { if (usN < usSt.length) usSt[usN++] = System.nanoTime() - usBase; }
    static String usGC() {
        StringBuilder b = new StringBuilder();
        for (java.lang.management.GarbageCollectorMXBean g
                : java.lang.management.ManagementFactory.getGarbageCollectorMXBeans()) {
            if (b.length() > 0) b.append('+');
            b.append(g.getName().replace(' ', '_')).append(':').append(g.getCollectionCount());
        }
        return b.toString();
    }
    static void usDump() {
        StringBuilder sb = new StringBuilder(1 << 21);
        sb.append("USMETA n=").append(usN).append(" gc=").append(usGC()).append('\n');
        for (int i = 0; i < usN; i++) sb.append("US ").append(usSt[i]).append('\n');
        System.err.print(sb); System.err.flush();
    }

    public static void main(String[] a) {
        long ops = 300000; if (a.length > 0) ops = Long.parseLong(a[0]);
        long sd = 20261013; if (a.length > 1) sd = Long.parseLong(a[1]);
        gstate = sd & 2147483647L;
        Idx x = new Idx(); Win w = new Win(); long sum = 0;
        usInit(ops + 2);
        for (long i = 0; i < ops; i++) {
            us();
            long k = nxt() % 4096;
            w.slots[(int)(i % 256)] = new Ent(k, i % 1000, null);
            if (i % 8 == 0) idxPut(x, k, i % 1000);
            long g = idxGet(x, k); if (g >= 0) sum = (sum + g) & MASK;
            long s = 0;
            for (long j = 0; j < 4; j++) { Ent t = w.slots[(int)((i + j) % 256)]; if (t != null) s += t.k; }
            sum = (sum + s) & MASK;
        }
        us();
        usDump();
        System.out.print("ops=" + ops + " idx=" + x.n + " sum=" + sum + "\n");
        System.out.print("ins=" + x.ins + " hit=" + x.hit + " win=256\n");
    }
}

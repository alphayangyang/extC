public final class SpectrumP2 {
    static final long MASK = 4611686018427387903L;
    static long gstate = 1;
    static long nxt() { gstate = (gstate * 1103515245L + 12345L) & 2147483647L; return gstate; }
    static final class Blk { long[] v = new long[256]; Blk next; }
    static final class Vec { Blk head, tail; long n; }
    static void push(Vec x, long val) {
        if (x.tail == null || x.n % 256 == 0) { Blk b = new Blk(); if (x.tail == null) x.head = b; else x.tail.next = b; x.tail = b; }
        x.tail.v[(int)(x.n % 256)] = val; x.n++;
    }

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

    public static void main(String[] args) {
        long n = 200000; if (args.length > 0) n = Long.parseLong(args[0]);
        long sd = 20261013; if (args.length > 1) sd = Long.parseLong(args[1]);
        gstate = sd & 2147483647L;
        Vec x = new Vec();
        usInit(n / 256 + 2);
        for (long i = 0; i < n; i++) { if (i % 256 == 0) us(); push(x, nxt() % 1000); }
        us();
        usDump();
        long sum = 0, idx = 0;
        for (Blk b = x.head; b != null; ) {
            for (long j = 0; j < 256 && idx < x.n; j++) { sum = (sum + b.v[(int)j] * (idx % 7 + 1)) & MASK; idx++; }
            b = b.next;
        }
        System.out.print("n=" + x.n + " sum=" + sum + "\n");
        System.out.print("blocks=" + ((x.n + 255) / 256) + " per=256\n");
    }
}

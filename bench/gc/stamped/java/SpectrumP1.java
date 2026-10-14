public final class SpectrumP1 {
    static final long MASK = 4611686018427387903L; static long gstate = 1;
    static long nxt() { gstate = (gstate * 1103515245L + 12345L) & 2147483647L; return gstate; }
    static final class Buf { long[] p = new long[64]; }

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
        long n = 200000; if (a.length > 0) n = Long.parseLong(a[0]);
        long sd = 20261013; if (a.length > 1) sd = Long.parseLong(a[1]);
        gstate = sd & 2147483647L;
        long sum = 0;
        usInit(n + 2);
        for (long i = 0; i < n; i++) { us(); Buf b = new Buf(); for (long j = 0; j < 64; j++) { b.p[(int)j] = nxt() % 1000; sum = (sum + b.p[(int)j] * (j + 1)) & MASK; } }
        us();
        usDump();
        System.out.print("n=" + n + " sum=" + sum + "\n");
        System.out.print("allocs=" + n + " per=64\n");
    }
}

public final class SpectrumP1 {
    static final long MASK = 4611686018427387903L; static long gstate = 1;
    static long nxt() { gstate = (gstate * 1103515245L + 12345L) & 2147483647L; return gstate; }
    static final class Buf { long[] p = new long[64]; }
    public static void main(String[] a) {
        long n = 200000; if (a.length > 0) n = Long.parseLong(a[0]);
        long sd = 20261013; if (a.length > 1) sd = Long.parseLong(a[1]);
        gstate = sd & 2147483647L;
        long sum = 0;
        for (long i = 0; i < n; i++) { Buf b = new Buf(); for (long j = 0; j < 64; j++) { b.p[(int)j] = nxt() % 1000; sum = (sum + b.p[(int)j] * (j + 1)) & MASK; } }
        System.out.print("n=" + n + " sum=" + sum + "\n");
        System.out.print("allocs=" + n + " per=64\n");
    }
}

// bench/gc/stamped/OverheadUs.java —— 量"打一次应用侧时间戳"本身要多少 ns。
// 与 stamped/java/*.java 里的 us() 逐字同形（System.nanoTime()）。
// 先跑 200 万次预热让 JIT 编译，再正式量 100 万次。
public final class OverheadUs {
    static long[] usSt; static int usN; static long usBase;
    static void usInit(long cap) { usSt = new long[(int) cap]; usN = 0; usBase = System.nanoTime(); }
    static void us() { if (usN < usSt.length) usSt[usN++] = System.nanoTime() - usBase; }

    public static void main(String[] a) {
        final int n = 1000000;
        for (int w = 0; w < 2; w++) {
            usInit(n + 2);
            for (int i = 0; i < n; i++) us();
        }
        usInit(n + 2);
        long t0 = System.nanoTime();
        for (int i = 0; i < n; i++) us();
        long t1 = System.nanoTime();
        System.out.printf("USOH n=%d total_ns=%d stamps=%d%n", n, t1 - t0, usN);
    }
}

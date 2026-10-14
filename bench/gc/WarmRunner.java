// bench/gc/WarmRunner.java —— task-78 ①-d：在同一 JVM 进程内重复跑某个形状 N 轮，
// 逐轮耗时打 stderr（WARMSTAT），stdout 只保留**最后一轮**的产物（便于与冷值/其它臂 cmp）。
// 用法：java -cp <dir> WarmRunner <主类> <warm> <N> <seed>
public final class WarmRunner {
    public static void main(String[] args) throws Exception {
        String cls = args[0];
        int warm = Integer.parseInt(args[1]);
        String[] rest = new String[]{args[2], args[3]};
        Class<?> k = Class.forName(cls);
        java.lang.reflect.Method m = k.getMethod("main", String[].class);
        StringBuilder sb = new StringBuilder();
        long[] it = new long[warm + 1];
        java.io.PrintStream realOut = System.out;
        for (int i = 0; i <= warm; i++) {
            java.io.ByteArrayOutputStream buf = new java.io.ByteArrayOutputStream();
            System.setOut(new java.io.PrintStream(buf, true, "US-ASCII"));
            long t0 = System.nanoTime();
            m.invoke(null, (Object) rest);
            long t1 = System.nanoTime();
            System.setOut(realOut);
            it[i] = t1 - t0;
            sb.setLength(0);
            sb.append(buf.toString("US-ASCII"));   // 只留最后一轮的产物
        }
        for (int i = 0; i <= warm; i++) {
            if (i > 0) System.err.print(",");
            System.err.printf(java.util.Locale.ROOT, "%.3f", it[i] / 1e6);
        }
        System.err.print("\n");
        System.err.flush();
        realOut.print(sb.toString());
        realOut.flush();
    }
}

// Java 25：虚拟线程 + SynchronousQueue（一次交接 = park/unpark = 一次"推进"）
import java.util.concurrent.SynchronousQueue;
public class Java {
  static long run(long k, long m, long salt) throws Exception {
    long s = 0;
    for (long id = 0; id < k; id++) {
      SynchronousQueue<Long> q = new SynchronousQueue<>();
      final long fid = id;
      Thread.startVirtualThread(() -> {
        try { for (long i = 0; i < m; i++) q.put((i % 65536) * 65521 + fid * 40503 + salt); }
        catch (InterruptedException e) { throw new RuntimeException(e); }
      });
      for (long i = 0; i < m; i++) s += q.take();
    }
    return s;
  }
  public static void main(String[] a) throws Exception {
    long t = run(8, 2000, 0);
    for (int r = 0; r < R_ROUNDS; r++) t += run(512, 20000, r);
    System.out.println(t);
  }
}

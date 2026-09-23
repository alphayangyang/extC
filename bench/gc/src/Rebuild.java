// GC 敏感形状 2：每轮重建（Java）。
public class Rebuild {
    static final class Node { long v; Node next; }
    public static void main(String[] a) {
        final int rounds = 20000, k = 1000;
        long acc = 0;
        for (int r = 0; r < rounds; r++) {
            Node h = null;
            for (int i = 0; i < k; i++) {
                Node c = new Node();
                c.v = i; c.next = h; h = c;
            }
            for (Node p = h; p != null; p = p.next) acc += p.v;
        }
        System.out.println(acc);
    }
}

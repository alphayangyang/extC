// 形状 1：churn（Java）—— 与 churn.extc 逐字同语义。
public class Churn {
    static final class Node { long v; Node next; }
    public static void main(String[] x) {
        final int rounds = 32000000;
        long acc = 0;
        for (int i = 0; i < rounds; i++) {
            Node a = null, b = null;
            Node p = new Node();
            p.v = i;
            if ((i & 1) == 0) {
                if (a != null) acc += a.v;
                a = p;
            } else {
                if (b != null) acc += b.v;
                b = p;
            }
            if (a != null) acc += a.v;
            if (b != null) acc += b.v;
        }
        System.out.println(acc);
    }
}

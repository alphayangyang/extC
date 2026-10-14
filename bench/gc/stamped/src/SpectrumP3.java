public final class SpectrumP3 {
    static final long MASK = 4611686018427387903L; static long gstate = 1;
    static long nxt() { gstate = (gstate * 1103515245L + 12345L) & 2147483647L; return gstate; }
    static final class Node { long v; Node next; }
    public static void main(String[] a) {
        long rounds = 5000; if (a.length > 0) rounds = Long.parseLong(a[0]);
        long sd = 20261013; if (a.length > 1) sd = Long.parseLong(a[1]);
        gstate = sd & 2147483647L;
        long sum = 0, total = 0;
        for (long r = 0; r < rounds; r++) {
            Node head = null; long k = 40 + nxt() % 40;
            for (long i = 0; i < k; i++) { Node p = new Node(); p.v = nxt() % 1000; p.next = head; head = p; }
            for (Node cur = head; cur != null; cur = cur.next) sum = (sum + cur.v) & MASK;
            total += k;
        }
        System.out.print("rounds=" + rounds + " nodes=" + total + " sum=" + sum + "\n");
        System.out.print("drops=" + rounds + " per_round_min=" + 40 + "\n");
    }
}

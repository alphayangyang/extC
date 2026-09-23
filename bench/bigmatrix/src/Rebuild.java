/* Shape 5: rebuild -- Java port of src/rebuild.c (allocation pressure).
 *
 * Read `rounds k`; each round builds a k-node chain, sums it, then throws it
 * away.  The C reference routes malloc/free through volatile function pointers
 * because gcc otherwise proves the whole pair away (SPEC.md's trap #1); the
 * Java counterpart of that trap is escape analysis, so the chain head lives in
 * a static field -- a heap store the JIT cannot see through -- and is nulled at
 * the end of every round to drop the chain.  No System.gc() anywhere: the
 * collector must meet this on its own, exactly like free() does in C. */
import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.InputStream;

public class Rebuild {
    /** C: typedef struct node { int64_t v; struct node *next; } node; */
    private static final class Node {
        long v;
        Node next;
    }

    /** Chain head; static on purpose (see the header comment). */
    private static Node head;

    public static void main(String[] args) throws IOException {
        FastIn in = new FastIn();
        long rounds = in.readLong();
        long k = in.readLong();

        long acc = 0;
        for (long r = 0; r < rounds; r++) {
            head = null;                       /* start the round with a fresh chain */
            for (long i = 0; i < k; i++) {
                Node c = new Node();
                c.v = i;
                c.next = head;
                head = c;
            }
            for (Node p = head; p != null; p = p.next) acc += p.v;
            head = null;                       /* drop it all: the chain is garbage now */
        }
        System.out.print("acc=" + acc + "\n");  /* C: %lld */
        System.out.flush();
    }

    /** 1 MiB block reader with hand-written integer parsing: the Java stand-in
     *  for src/fastio.h (fread + hand-rolled digits).  Scanner is deliberately
     *  not used -- it is about two orders of magnitude slower than this. */
    private static final class FastIn {
        private static final int CAP = 1 << 20;
        private final InputStream in = new BufferedInputStream(System.in, CAP);
        private final byte[] buf = new byte[CAP];
        private int pos = 0, len = 0;

        /** Refill the block buffer; returns the bytes now available (0 at EOF). */
        private int fill() throws IOException {
            int n = 0;
            while (n < CAP) {
                int r = in.read(buf, n, CAP - n);
                if (r < 0) break;
                n += r;
            }
            pos = 0;
            len = n;
            return n;
        }

        /** Signed integer, same grammar as fastio.h's bm_i64(). */
        long readLong() throws IOException {
            byte[] b = buf;
            int p = pos, l = len, c;
            for (;;) {
                if (p >= l) {
                    l = fill();
                    p = 0;
                    if (l == 0) { pos = 0; return 0; }
                }
                c = b[p++] & 0xFF;
                if (c != ' ' && c != '\n' && c != '\t' && c != '\r') break;
            }
            boolean neg = false;
            if (c == '-') {
                if (p >= l) {
                    l = fill();
                    p = 0;
                    if (l == 0) { pos = 0; return 0; }
                }
                c = b[p++] & 0xFF;
            }
            long v = 0;
            while (c >= '0' && c <= '9') {
                v = v * 10 + (c - '0');
                if (p >= l) {
                    l = fill();
                    p = 0;
                    if (l == 0) break;
                }
                c = b[p++] & 0xFF;
            }
            pos = p;                       /* like bm_get(), the terminator is consumed */
            len = l;
            return neg ? -v : v;
        }

        /** Unsigned 32-bit value as raw bits: bm_u64()'s grammar (skip anything
         *  that is not a digit) plus the C reference's (uint32_t) narrowing. */
        int readU32() throws IOException {
            byte[] b = buf;
            int p = pos, l = len, c;
            for (;;) {
                if (p >= l) {
                    l = fill();
                    p = 0;
                    if (l == 0) { pos = 0; return 0; }
                }
                c = b[p++] & 0xFF;
                if (c >= '0' && c <= '9') break;
            }
            long v = c - '0';
            for (;;) {
                if (p >= l) {
                    l = fill();
                    p = 0;
                    if (l == 0) break;
                }
                c = b[p++] & 0xFF;
                if (c < '0' || c > '9') break;
                v = v * 10 + (c - '0');
            }
            pos = p;
            len = l;
            return (int) v;
        }
    }
}

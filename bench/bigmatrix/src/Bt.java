/* Shape 3: bt -- Java port of src/bt.c.
 * Read `maxdepth bigdepth iters`; build perfect binary trees bottom-up,
 * count nodes; then count one big tree `iters` times. */
import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.InputStream;

public class Bt {
    /** C: typedef struct tree { struct tree *left, *right; } tree; */
    private static final class Tree {
        Tree left, right;
    }

    private static Tree bottomUp(int depth) {
        Tree t = new Tree();
        if (depth > 0) {
            t.left = bottomUp(depth - 1);
            t.right = bottomUp(depth - 1);
        }
        return t;
    }

    /** C: if (!t->left) return 1; -- a leaf (both children null) counts as one. */
    private static long check(Tree t) {
        if (t.left == null) return 1;
        return 1 + check(t.left) + check(t.right);
    }

    public static void main(String[] args) throws IOException {
        FastIn in = new FastIn();
        int maxdepth = (int) in.readLong();
        int bigdepth = (int) in.readLong();
        long iters = in.readLong();

        long total = 0;
        for (int d = 4; d <= maxdepth; d++) {
            Tree t = bottomUp(d);
            total += check(t);
            /* t dies here: the collector reclaims the tree, which is what the C
             * reference's freeTree()/free() pair does explicitly. */
        }
        Tree big = bottomUp(bigdepth);
        for (long i = 0; i < iters; i++) total += check(big);

        System.out.print("trees=" + total + "\n");       /* C: %lld */
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

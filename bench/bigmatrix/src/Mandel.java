/* Shape 4: mandel -- Java port of src/mandel.c.
 * Read `w h maxiter`; same loop as bench/heavy/mb.c (double, escape at |z|>2). */
import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.InputStream;

public class Mandel {
    public static void main(String[] args) throws IOException {
        FastIn in = new FastIn();
        int w = (int) in.readLong();
        int h = (int) in.readLong();
        long maxiter = in.readLong();

        byte[] img = new byte[w * h];
        long acc = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double cr = (double) x / (double) w * 3.5 - 2.5;
                double ci = (double) y / (double) h * 2.0 - 1.0;
                double zr = 0, zi = 0;
                long it = 0;
                while (it < maxiter) {
                    double t = zr * zr - zi * zi + cr;
                    zi = 2.0 * zr * zi + ci;
                    zr = t;
                    if (zr * zr + zi * zi > 4.0) break;
                    it++;
                }
                img[y * w + x] = (byte) (it & 255);
                acc += it;
            }
        }
        System.out.print("mandel=" + acc                    /* C: %lld */
                + " img0=" + (img[0] & 0xFF)                /* C: %d of a uint8_t */
                + "\n");
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

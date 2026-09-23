/* Shape 1: radix -- Java port of src/radix.c.
 * Read n and n u32 values from stdin, LSD radix sort (4 passes x 8 bits),
 * print sorted/first/last.
 *
 * The C reference sorts uint32_t, so the ordering check is an *unsigned*
 * comparison and the two reported values print unsigned: on the small input
 * last=4294966545, which is larger than Integer.MAX_VALUE. */
import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.InputStream;

public class Radix {
    public static void main(String[] args) throws IOException {
        FastIn in = new FastIn();
        int n = (int) in.readLong();
        int[] a = new int[n];
        int[] b = new int[n];
        for (int i = 0; i < n; i++) a[i] = in.readU32();

        for (int pass = 0; pass < 4; pass++) {
            int sh = pass * 8;
            long[] cnt = new long[256];                          /* C: int64_t cnt[256] */
            for (int i = 0; i < n; i++) cnt[(a[i] >>> sh) & 255]++;
            long sum = 0;
            for (int k = 0; k < 256; k++) { long c = cnt[k]; cnt[k] = sum; sum += c; }
            for (int i = 0; i < n; i++) b[(int) cnt[(a[i] >>> sh) & 255]++] = a[i];
            System.arraycopy(b, 0, a, 0, n);                     /* C: for (i) a[i] = b[i] */
        }

        int ok = 1;
        for (int i = 1; i < n; i++) {
            if (Integer.compareUnsigned(a[i - 1], a[i]) > 0) { ok = 0; break; }   /* uint32_t > */
        }
        System.out.print("sorted=" + ok
                + " first=" + Integer.toUnsignedString(a[0])      /* %u */
                + " last=" + Integer.toUnsignedString(a[n - 1])   /* %u */
                + "\n");
        System.out.flush();
    }

    /** 1 MiB block reader with hand-written integer parsing: the Java stand-in
     *  for src/fastio.h (fread + hand-rolled digits).  Scanner is deliberately
     *  not used -- with 10^7 numbers it would cost more than the sort itself. */
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

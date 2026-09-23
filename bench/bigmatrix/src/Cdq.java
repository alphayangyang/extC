/* Shape 2: cdq -- Java port of src/cdq.c (3D dominance, CDQ + weight BIT).
 *
 * Read n, then n triples (a b c) with values in [1,10^6].
 *   1. sort by (a,b,c)   2. collapse duplicates, keeping cnt
 *   3. CDQ over b, BIT over c: ans[j] += cnt[i] for every i<j that dominates j
 *      (the BIT is indexed by c itself: the generator emits c >= 1, so no +1
 *       shift is needed and the array stays V+1 wide, indices 0..V)
 *   4. f = ans + cnt (itself), reported answer = f - 1
 *   print sum = SUM cnt_i * ans_i (u64 wrap) and max = max ans_i
 *
 * The sort is a genuine comparison sort -- the C reference calls qsort there --
 * so Arrays.sort on Comparable points is the honest translation. */
import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;

public class Cdq {
    private static final int V = 1_000_000;              /* C: #define V 1000000 */

    /** C: typedef struct { int32_t a, b, c, cnt, ans; } Pt; */
    private static final class Pt implements Comparable<Pt> {
        int a, b, c, cnt, ans;

        @Override
        public int compareTo(Pt o) {                     /* C: cmpPt() */
            if (a != o.a) return a < o.a ? -1 : 1;
            if (b != o.b) return b < o.b ? -1 : 1;
            if (c != o.c) return c < o.c ? -1 : 1;
            return 0;
        }
    }

    private static Pt[] p, tmp;
    private static int[] bit;                            /* C: int32_t *bit, calloc(V + 1) */
    private static int m;

    private static void bitAdd(int i, int v) {
        for (; i <= V; i += i & (-i)) bit[i] += v;
    }

    private static int bitSum(int i) {
        int s = 0;
        for (; i > 0; i -= i & (-i)) s += bit[i];
        return s;
    }

    private static void cdq(int l, int r) {
        if (l >= r) return;
        int mid = l + (r - l) / 2;
        cdq(l, mid);
        cdq(mid + 1, r);

        int i = l, j = mid + 1;
        while (j <= r) {
            while (i <= mid && p[i].b <= p[j].b) { bitAdd(p[i].c, p[i].cnt); i++; }
            p[j].ans += bitSum(p[j].c);
            j++;
        }
        for (int t = l; t < i; t++) bitAdd(p[t].c, -p[t].cnt);      /* undo */

        i = l; j = mid + 1;
        int k = l;
        while (i <= mid && j <= r) tmp[k++] = (p[i].b <= p[j].b) ? p[i++] : p[j++];
        while (i <= mid) tmp[k++] = p[i++];
        while (j <= r) tmp[k++] = p[j++];
        System.arraycopy(tmp, l, p, l, r - l + 1);
    }

    public static void main(String[] args) throws IOException {
        FastIn in = new FastIn();
        int n = (int) in.readLong();
        Pt[] raw = new Pt[n];
        for (int i = 0; i < n; i++) {
            Pt q = new Pt();
            q.a = in.readU32();
            q.b = in.readU32();
            q.c = in.readU32();
            q.cnt = 1;
            q.ans = 0;
            raw[i] = q;
        }
        Arrays.sort(raw);                                /* C: qsort(raw, n, sizeof(Pt), cmpPt) */

        p = new Pt[n];
        tmp = new Pt[n];
        m = 0;
        for (int i = 0; i < n; ) {
            int j = i + 1;
            while (j < n && raw[j].a == raw[i].a && raw[j].b == raw[i].b && raw[j].c == raw[i].c) j++;
            Pt q = raw[i];                               /* C copies the representative into p[m] */
            q.cnt = j - i;
            q.ans = 0;
            p[m++] = q;
            i = j;
        }

        /* V + 1 slots, exactly like the C calloc: c is in [1, V], so both
         * bitAdd(c, ..) (i <= V) and bitSum(c) stay inside 0..V. */
        bit = new int[V + 1];
        cdq(0, m - 1);

        long sum = 0;                                    /* C: uint64_t -- wraps the same way */
        long mx = 0;
        for (int i = 0; i < m; i++) {
            long ans = (long) p[i].ans + (long) p[i].cnt - 1;
            sum += (long) p[i].cnt * ans;
            if (ans > mx) mx = ans;
        }
        System.out.print("sum=" + Long.toUnsignedString(sum) + " max=" + mx + "\n");   /* %llu %lld */
        System.out.flush();
    }

    /** 1 MiB block reader with hand-written integer parsing: the Java stand-in
     *  for src/fastio.h (fread + hand-rolled digits).  Scanner is deliberately
     *  not used -- with 2*10^6 points it would cost more than the CDQ itself. */
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

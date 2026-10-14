public final class SpectrumP4 {
    static final long MASK = 4611686018427387903L;
    static long gstate = 1;
    static long nxt() { gstate = (gstate * 1103515245L + 12345L) & 2147483647L; return gstate; }
    static long bucketOf(long n) { return (n * 2654435761L) & 2047L; }
    static final class Sym { long name, val; Sym next; Sym(long a, long b, Sym c){name=a;val=b;next=c;} }
    static final class UnitNode { long id, kind, payload; UnitNode sib; }
    static final class Table { Sym[] buckets = new Sym[2048]; long count; }
    static void tableInsert(Table t, long name, long val) {
        int h = (int) bucketOf(name);
        t.buckets[h] = new Sym(name, val, t.buckets[h]);
        t.count++;
    }
    static long tableFind(Table t, long name) {
        int h = (int) bucketOf(name);
        long probes = 0;
        for (Sym cur = t.buckets[h]; cur != null; cur = cur.next) { probes++; if (cur.name == name) return (cur.val << 20) | probes; }
        return -1;
    }
    public static void main(String[] args) {
        long units = 2000; if (args.length > 0) units = Long.parseLong(args[0]);
        long sd = 20261013; if (args.length > 1) sd = Long.parseLong(args[1]);
        if (units < 8) units = 8;
        gstate = sd & 2147483647L;
        Table t = new Table();
        long sum = 0, asts = 0, inserts = 0, hits = 0, misses = 0, probes = 0, nmake = 0;
        for (long pre = 0; pre < 4096; pre++) { tableInsert(t, pre, pre * 7 + 11); inserts++; }
        for (long u = 0; u < units; u++) {
            UnitNode ast = null;
            long m = 640 + nxt() % 256;
            for (long j = 0; j < m; j++) { UnitNode p = new UnitNode(); p.id = nmake; p.kind = nxt() % 7; p.payload = nxt() % 1000; p.sib = ast; nmake++; ast = p; }
            asts += m;
            for (UnitNode cur = ast; cur != null; cur = cur.sib) sum = (sum + cur.id * 3 + cur.payload + cur.kind) & MASK;
            for (long ins = 0; ins < 8; ins++) { long name = 4096 + nxt() % (units * 4 + 64); if (tableFind(t, name) < 0) { tableInsert(t, name, name * 13 + u); inserts++; } }
            for (long q = 0; q < 96; q++) {
                long name2 = nxt() % (4096 + u * 3 + 8);
                long res = tableFind(t, name2);
                if (res < 0) misses++; else { hits++; probes += res & 1048575L; sum = (sum + (res >> 20)) & MASK; }
            }
        }
        System.out.print("units=" + units + " symbols=" + t.count + " asts=" + asts + " sum=" + sum + "\n");
        System.out.print("inserts=" + inserts + " hits=" + hits + " misses=" + misses + " probes=" + probes + "\n");
    }
}

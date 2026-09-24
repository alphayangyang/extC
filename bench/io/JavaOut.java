// Java · 输出：BufferedWriter + StringBuilder ✓
import java.io.*;

public class JavaOut {
    public static void main(String[] args) throws IOException {
        long n = Long.parseLong(args[1]);
        Writer w = args[0].equals("-")
            ? new BufferedWriter(new OutputStreamWriter(System.out), 1 << 16)
            : new BufferedWriter(new FileWriter(args[0]), 1 << 16);
        StringBuilder sb = new StringBuilder(1 << 16);
        for (long i = 0; i < n; i++) {
            sb.append(i).append(' ').append(i * i).append('\n');
            if (sb.length() > (1 << 15)) { w.write(sb.toString()); sb.setLength(0); }
        }
        w.write(sb.toString());
        w.flush();
    }
}

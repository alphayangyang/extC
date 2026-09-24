// Java · 输入：BufferedReader + StringTokenizer（经典快写法 ✓）
// 契约：`in <文件|->` ⇒ 打印总和 ✓（JVM 启动另算，见 run.sh 的基线 ✓）
import java.io.*;
import java.util.*;

public class JavaIn {
    public static void main(String[] args) throws IOException {
        BufferedReader br = args[0].equals("-")
            ? new BufferedReader(new InputStreamReader(System.in), 1 << 16)
            : new BufferedReader(new FileReader(args[0]), 1 << 16);
        long total = 0;
        String line;
        while ((line = br.readLine()) != null) {
            StringTokenizer st = new StringTokenizer(line);
            while (st.hasMoreTokens()) total += Long.parseLong(st.nextToken());
        }
        System.out.println(total);
    }
}

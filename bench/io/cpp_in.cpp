/* C++ · 输入：`cin`（默认同步）/ `-DFAST` ⇒ sync_with_stdio(false)+tie(nullptr)
 * 文件侧就是经典的 `std::ifstream fin(路径)` ✓
 * 契约：`in <文件|->` ⇒ 打印总和 ✓ */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace std;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法: in <文件|->\n"); return 2; }
#ifdef FAST
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
#endif
    long long total = 0;
    if (strcmp(argv[1], "-") == 0) {
        long long a, b;
        while (cin >> a >> b) total += a + b;
    } else {
        ifstream fin(argv[1]);
        if (!fin) { perror("open"); return 1; }
        long long a, b;
        while (fin >> a >> b) total += a + b;
        fin.close();
    }
    printf("%lld\n", total);
    return 0;
}

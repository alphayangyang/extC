/* C++ · 输出：`cout` / `ofstream fout`；`-DUSE_ENDL` ⇒ 用 `endl`（每次都 flush ✗）
 * 契约：`out <文件|-> <n>` ⇒ 写 n 行 "i i*i" ✓ */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace std;

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "用法: out <文件|-> <n>\n"); return 2; }
#ifdef FAST
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
#endif
    long long n = atoll(argv[2]);
    ostream *os = &cout;
    ofstream fout;
    if (strcmp(argv[1], "-") != 0) { fout.open(argv[1]); if (!fout) { perror("open"); return 1; } os = &fout; }
    for (long long i = 0; i < n; i++) {
#ifdef USE_ENDL
        *os << i << ' ' << i * i << endl;
#else
        *os << i << ' ' << i * i << "\n";
#endif
    }
    return 0;
}

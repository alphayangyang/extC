// S2 的对照：std::string 用 64 字节块 append 65536 次 × 16 条。
#include <cstdio>
#include <string>

int main() {
    long long sum = 0;
    for (int k = 0; k < 16; ++k) {
        std::string s;
        char chunk[64];
        for (int j = 0; j < 64; ++j) chunk[j] = (char)(65 + j % 26);
        for (int r = 0; r < 65536; ++r) s.append(chunk, 64);
        sum += (long long)s.size();
    }
    std::printf("n=%lld cap=%d\n", sum, 1);
    return 0;
}

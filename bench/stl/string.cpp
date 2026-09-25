// 与 bench/stl/string.extc 同算法同参数（std::string）
#include <cstdint>
#include <cstdio>
#include <string>
const int64_t N = 10000000;
int main() {
    std::string s;
    s.reserve(N);
    for (int64_t i = 0; i < N; i++) s.push_back((char)(97 + i % 26));
    std::string acc;
    acc.reserve(1000000 * 8);
    for (int64_t i = 0; i < 1000000; i++) acc.append("abcdefgh");
    int64_t found = 0;
    for (int64_t i = 0; i < 10000; i++) {
        size_t at = s.find("abcdefghijklmnop");
        found += (at == std::string::npos) ? -1 : (int64_t)at;
    }
    printf("len=%lld alen=%lld found=%lld\n", (long long)s.size(), (long long)acc.size(), (long long)found);
    return 0;
}

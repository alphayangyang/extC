// 形状 V4 的对照。
#include <cstdio>
#include <vector>

int main() {
    long long total = 0;
    for (long long r = 0; r < 200000; ++r) {
        std::vector<long long> v;
        for (long long i = 0; i < 8; ++i) v.push_back(r + i);
        for (long long x : v) total += x;
    }
    std::printf("n=%d sum=%lld\n", 8, total);
    return 0;
}
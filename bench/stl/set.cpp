// 与 bench/stl/set.extc 同算法同参数（std::unordered_set）
#include <cstdint>
#include <cstdio>
#include <unordered_set>
const int64_t N = 1000000;
int main() {
    std::unordered_set<int64_t> s;
    s.reserve(N);
    uint64_t u = 12345;
    int64_t hits = 0, removed = 0;
    for (int64_t i = 0; i < N; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        s.insert((int64_t)(u & 0x7fffffffULL));
    }
    u = 12345;
    for (int64_t i = 0; i < N; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        if (s.count((int64_t)(u & 0x7fffffffULL))) hits++;
    }
    u = 12345;
    for (int64_t i = 0; i < N; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        if (i % 2 == 0) removed += s.erase((int64_t)(u & 0x7fffffffULL));
    }
    printf("len=%lld hits=%lld removed=%lld\n", (long long)s.size(), (long long)hits, (long long)removed);
    return 0;
}

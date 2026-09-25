// 与 bench/stl/map.extc 同算法同参数（std::map，红黑树）
#include <cstdint>
#include <cstdio>
#include <map>
const int64_t N = 1000000;
int main() {
    std::map<int64_t, int32_t> m;
    uint64_t u = 12345;
    int64_t sum = 0, removed = 0;
    for (int64_t i = 0; i < N; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t k = (int64_t)(u & 0x7fffffffULL);
        m[k] = (int32_t)k;
    }
    u = 12345;
    for (int64_t i = 0; i < N; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t k = (int64_t)(u & 0x7fffffffULL);
        auto it = m.find(k);
        sum += (it == m.end()) ? -1 : it->second;
    }
    for (auto it = m.begin(); it != m.end(); ++it) sum += it->first % 1000;
    u = 12345;
    for (int64_t i = 0; i < N; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t k = (int64_t)(u & 0x7fffffffULL);
        if (i % 2 == 0) removed += m.erase(k);
    }
    printf("len=%lld sum=%lld removed=%lld\n", (long long)m.size(), (long long)sum, (long long)removed);
    return 0;
}

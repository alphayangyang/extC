// 与 bench/stl/hashmap.extc 同算法同参数（std::unordered_map）
#include <cstdint>
#include <cstdio>
#include <unordered_map>
const int64_t N = 1000000;
int main() {
    std::unordered_map<int64_t, int32_t> m;
    m.reserve(N);
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
    u = 12345;
    for (int64_t i = 0; i < N; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t k = (int64_t)(u & 0x7fffffffULL);
        if (i % 2 == 0) removed += m.erase(k);
    }
    printf("len=%lld sum=%lld removed=%lld\n", (long long)m.size(), (long long)sum, (long long)removed);
    return 0;
}

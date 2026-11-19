// 形状 LS1 的对照：线性查找的集合（有序数组 + 线性扫描）。
#include <cstdint>
#include <cstdio>
#include <vector>

static const uint64_t SEED0 = 0x2545F4914F6CDD1Dull;
static const long long N = 1024;
static const long long Q = 1000000;
static const uint64_t SPACE = 2048ull;

struct Rng { uint64_t s; };

static uint64_t next(Rng &r) {
    uint64_t x = r.s;
    x = x ^ (x << 13);
    x = x ^ (x >> 7);
    x = x ^ (x << 17);
    r.s = x;
    return x;
}

static bool has(const std::vector<long long> &v, long long k) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == k) return true;
    return false;
}

int main() {
    std::vector<long long> s;
    s.reserve((size_t)N);
    Rng r{SEED0};
    long long added = 0;
    for (long long i = 0; i < N; ++i) {
        long long k = (long long)(next(r) % SPACE);
        if (!has(s, k)) { s.push_back(k); ++added; }
    }
    Rng r2{SEED0};
    long long hits = 0;
    for (long long i = 0; i < Q; ++i) {
        long long k = (long long)(next(r2) % SPACE);
        if (has(s, k)) ++hits;
    }
    std::printf("n=%lld added=%lld hits=%lld\n", (long long)s.size(), added, hits);
    return 0;
}

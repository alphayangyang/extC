// 形状 HS1 的对照：std::unordered_set<long long> 先 reserve(N)。
#include <cstdint>
#include <cstdio>
#include <unordered_set>

static const uint64_t SEED0 = 0x2545F4914F6CDD1Dull;
static const long long N = 4000000;
static const uint64_t SPACE = 1000000007ull;

struct Rng { uint64_t s; };

static uint64_t next(Rng &r) {
    uint64_t x = r.s;
    x = x ^ (x << 13);
    x = x ^ (x >> 7);
    x = x ^ (x << 17);
    r.s = x;
    return x;
}

int main() {
    std::unordered_set<long long> s;
    s.reserve((size_t)N);
    Rng r{SEED0};
    long long added = 0;
    for (long long i = 0; i < N; ++i) {
        long long k = (long long)(next(r) % SPACE);
        if (s.insert(k).second) ++added;
    }
    Rng r2{SEED0};
    long long hits = 0;
    for (long long i = 0; i < N; ++i) {
        long long k = (long long)(next(r2) % SPACE);
        if (s.count(k)) ++hits;
    }
    std::printf("n=%lld added=%lld hits=%lld\n", (long long)s.size(), added, hits);
    return 0;
}

// 形状 H1 的对照：std::unordered_map<long long,long long> 先 reserve(N)。
#include <cstdint>
#include <cstdio>
#include <unordered_map>

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
    std::unordered_map<long long, long long> m;
    m.reserve((size_t)N);
    Rng r{SEED0};
    for (long long i = 0; i < N; ++i) {
        long long k = (long long)(next(r) % SPACE);
        m[k] = k * 3 + 1;
    }
    Rng r2{SEED0};
    long long sum = 0, hits = 0;
    for (long long i = 0; i < N; ++i) {
        long long k = (long long)(next(r2) % SPACE);
        auto it = m.find(k);
        long long v = (it == m.end()) ? 0 : it->second;
        if (v != 0) ++hits;
        sum += v;
    }
    std::printf("n=%lld hits=%lld sum=%lld\n", (long long)m.size(), hits, sum);
    return 0;
}

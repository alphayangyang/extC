// 形状 M1 的对照：std::map<long long,long long>（红黑树）插入 1e6 条再查 1e6 次。
#include <cstdint>
#include <cstdio>
#include <map>

static const uint64_t SEED0 = 0x2545F4914F6CDD1Dull;
static const long long N = 1000000;
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
    std::map<long long, long long> m;
    Rng r{SEED0};
    long long added = 0;
    for (long long i = 0; i < N; ++i) {
        long long k = (long long)(next(r) % SPACE);
        auto it = m.find(k);
        if (it == m.end()) { m.emplace(k, k * 3 + 1); ++added; }
        else it->second = k * 3 + 1;
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
    std::printf("n=%lld added=%lld hits=%lld sum=%lld\n", (long long)m.size(), added, hits, sum);
    return 0;
}

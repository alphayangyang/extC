// 形状 H4 的对照：std::unordered_map 两遍全表遍历（值一遍、键一遍）。
#include <cstdint>
#include <cstdio>
#include <unordered_map>

static const uint64_t SEED0 = 0x2545F4914F6CDD1Dull;
static const long long N = 2000000;
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
    long long vsum = 0, ksum = 0;
    for (const auto &kv : m) vsum += kv.second;
    for (const auto &kv : m) ksum += kv.first;
    std::printf("n=%lld dn=%lld vsum=%lld ksum=%lld\n", (long long)m.size(), (long long)m.size(), vsum, ksum);
    return 0;
}

// 形状 H3 的对照：std::unordered_map 上的同一条动作序列。
#include <cstdint>
#include <cstdio>
#include <unordered_map>

static const uint64_t SEED0 = 0x2545F4914F6CDD1Dull;
static const long long OPS = 8000000;
static const uint64_t SPACE = 1000000ull;

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
    Rng r{SEED0};
    long long sum = 0, hits = 0, removed = 0, repl = 0;
    for (long long i = 0; i < OPS; ++i) {
        uint64_t x = next(r);
        long long k = (long long)(x % SPACE);
        uint64_t op = (x >> 40) % 4ull;
        if (op < 2ull) {
            if (m.find(k) != m.end()) ++repl;
            m[k] = (long long)(x % 1000003ull);
        } else if (op == 2ull) {
            auto it = m.find(k);
            long long v = (it == m.end()) ? -1 : it->second;
            if (v != -1) ++hits;
            sum += v;
        } else {
            if (m.erase(k)) ++removed;
        }
    }
    std::printf("n=%lld hits=%lld removed=%lld repl=%lld sum=%lld\n", (long long)m.size(), hits, removed, repl, sum);
    return 0;
}

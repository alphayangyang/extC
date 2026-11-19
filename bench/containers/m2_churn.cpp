// 形状 M2 的对照：std::map 在 30 万个键上做 2e6 次写入/删除/读取。
#include <cstdint>
#include <cstdio>
#include <map>

static const uint64_t SEED0 = 0x2545F4914F6CDD1Dull;
static const long long OPS = 2000000;
static const uint64_t SPACE = 300000ull;

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
    long long added = 0, rem = 0, hits = 0, sum = 0;
    for (long long i = 0; i < OPS; ++i) {
        long long k = (long long)(next(r) % SPACE);
        long long op = i % 3;
        if (op == 0) {
            auto it = m.find(k);
            if (it == m.end()) { m.emplace(k, k * 3 + 1); ++added; }
            else it->second = k * 3 + 1;
        }
        if (op == 1) { if (m.erase(k)) ++rem; }
        if (op == 2) {
            auto it = m.find(k);
            long long v = (it == m.end()) ? 0 : it->second;
            if (v != 0) ++hits;
            sum += v;
        }
    }
    std::printf("n=%lld added=%lld rem=%lld hits=%lld sum=%lld\n", (long long)m.size(), added, rem, hits, sum);
    return 0;
}

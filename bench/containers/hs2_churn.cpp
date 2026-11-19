// 形状 HS2 的对照：std::unordered_set 在 1e6 个键上做 8e6 次插入/删除/查询。
#include <cstdint>
#include <cstdio>
#include <unordered_set>

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
    std::unordered_set<long long> s;
    Rng r{SEED0};
    long long added = 0, rem = 0, hits = 0;
    for (long long i = 0; i < OPS; ++i) {
        long long k = (long long)(next(r) % SPACE);
        long long op = i % 3;
        if (op == 0) { if (s.insert(k).second) ++added; }
        if (op == 1) { if (s.erase(k)) ++rem; }
        if (op == 2) { if (s.count(k)) ++hits; }
    }
    std::printf("n=%lld added=%lld rem=%lld hits=%lld\n", (long long)s.size(), added, rem, hits);
    return 0;
}

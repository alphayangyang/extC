// 形状 LM2 的对照：有序数组 + 线性查找 + swap-remove。
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

static const uint64_t SEED0 = 0x2545F4914F6CDD1Dull;
static const long long N = 1024;
static const long long OPS = 1000000;
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

static long long findIn(const std::vector<std::pair<long long, long long>> &v, long long k) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i].first == k) return (long long)i;
    return -1;
}

int main() {
    std::vector<std::pair<long long, long long>> m;
    m.reserve((size_t)N);
    Rng r{SEED0};
    long long rem = 0, hits = 0, sum = 0;
    for (long long i = 0; i < OPS; ++i) {
        long long k = (long long)(next(r) % SPACE);
        long long op = i % 3;
        long long at = findIn(m, k);
        if (op == 0) {
            if (at >= 0) m[(size_t)at].second = k * 3 + 1;
            else m.push_back(std::make_pair(k, k * 3 + 1));
        }
        if (op == 1) {
            if (at >= 0) {
                m[(size_t)at] = m.back();
                m.pop_back();
                ++rem;
            }
        }
        if (op == 2) {
            long long v = (at < 0) ? 0 : m[(size_t)at].second;
            if (v != 0) ++hits;
            sum += v;
        }
    }
    std::printf("n=%lld rem=%lld hits=%lld sum=%lld\n", (long long)m.size(), rem, hits, sum);
    return 0;
}

// bench/containers/h_flat.cpp —— C++ 那一侧的「同类算法」参考：手写开放寻址表。
//
// libstdc++ 的 `unordered_map` 是链式节点表（每元素一次分配、查找沿指针追），在开放寻址
// 实现面前天然吃亏。这个文件用 C++ 写一张和 extC hashMap **同一类算法**的表：2 的幂容量、
// 线性探测、墓碑、0.5 装载率、扩容重排；区别是键与值**内联在桶里**，没有 extC 那条稠密值列
// 与「稠密下标 -> 桶」的反查列（那是为了跨扩容的稳定句柄）。它回答的问题是：
// extC 的优势来自算法，还是来自 C++ 标准库的实现选择？
//
// 用法：./h_flat h1|h2|h3|h4|h5   （输出必须与同名形状的 extC 臂逐字相同）
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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

static uint64_t mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

struct Flat {
    std::vector<uint64_t> keys;
    std::vector<uint8_t> tag;    // 0 EMPTY / 1 LIVE / 2 TOMB
    std::vector<long long> val;
    size_t cap = 8, n = 0, dead = 0;

    void alloc(size_t c) {
        cap = c; keys.assign(cap, 0); tag.assign(cap, 0); val.assign(cap, 0); n = dead = 0;
    }
    void init(size_t want) {
        size_t c = 8;
        while (c < want * 2) c <<= 1;
        alloc(c);
    }
    static size_t h(uint64_t k, size_t cap) { return (size_t)(mix(k) & (uint64_t)(cap - 1)); }
    bool find(uint64_t k, size_t &at) const {
        size_t i = h(k, cap);
        for (;;) {
            if (tag[i] == 0) return false;
            if (tag[i] == 1 && keys[i] == k) { at = i; return true; }
            i = (i + 1) & (cap - 1);
        }
    }
    void place(uint64_t k, long long v) {   // 只用于重排：表里保证没有同键
        size_t i = h(k, cap);
        while (tag[i] == 1) i = (i + 1) & (cap - 1);
        keys[i] = k; tag[i] = 1; val[i] = v; ++n;
    }
    void rehash(size_t newcap) {
        std::vector<uint64_t> k2; std::vector<long long> v2;
        k2.reserve(n); v2.reserve(n);
        for (size_t i = 0; i < cap; ++i)
            if (tag[i] == 1) { k2.push_back(keys[i]); v2.push_back(val[i]); }
        alloc(newcap);
        for (size_t j = 0; j < k2.size(); ++j) place(k2[j], v2[j]);
    }
    void grow_for_one() {
        if ((n + dead + 1) * 2 <= cap) return;
        // 墓碑占多数就原地重排（省一次翻倍），否则翻倍重排
        rehash(dead * 2 >= n ? cap : cap * 2);
    }
    bool put(uint64_t k, long long v) {
        size_t at;
        if (find(k, at)) { val[at] = v; return true; }
        grow_for_one();
        size_t i = h(k, cap);
        while (tag[i] == 1) i = (i + 1) & (cap - 1);
        if (tag[i] == 2) --dead;
        keys[i] = k; tag[i] = 1; val[i] = v; ++n;
        return false;
    }
    bool get(uint64_t k, long long &v) const {
        size_t at;
        if (!find(k, at)) return false;
        v = val[at];
        return true;
    }
    bool erase(uint64_t k) {
        size_t at;
        if (!find(k, at)) return false;
        tag[at] = 2; --n; ++dead;
        return true;
    }
};

static void shape_build(bool cap_upfront) {
    Flat m;
    m.init(cap_upfront ? (size_t)N : 8);
    Rng r{SEED0};
    for (long long i = 0; i < N; ++i) {
        uint64_t k = next(r) % SPACE;
        m.put(k, (long long)k * 3 + 1);
    }
    Rng r2{SEED0};
    long long sum = 0, hits = 0;
    for (long long i = 0; i < N; ++i) {
        uint64_t k = next(r2) % SPACE;
        long long v = 0;
        if (m.get(k, v)) { if (v != 0) ++hits; } else v = 0;
        sum += v;
    }
    std::printf("n=%lld hits=%lld sum=%lld\n", (long long)m.n, hits, sum);
}

static void shape_h3() {
    Flat m;
    m.init(1024);
    Rng r{SEED0};
    long long sum = 0, hits = 0, removed = 0, repl = 0;
    for (long long i = 0; i < 8000000; ++i) {
        uint64_t x = next(r);
        uint64_t k = x % 1000000ull;
        uint64_t op = (x >> 40) % 4ull;
        if (op < 2) { if (m.put(k, (long long)(x % 1000003ull))) ++repl; }
        else if (op == 2) {
            long long v = -1;
            if (m.get(k, v)) { if (v != -1) ++hits; } else v = -1;
            sum += v;
        } else { if (m.erase(k)) ++removed; }
    }
    std::printf("n=%lld hits=%lld removed=%lld repl=%lld sum=%lld\n",
                (long long)m.n, hits, removed, repl, sum);
}

static void shape_h4() {
    Flat m;
    m.init((size_t)2000000);
    Rng r{SEED0};
    for (long long i = 0; i < 2000000; ++i) {
        uint64_t k = next(r) % SPACE;
        m.put(k, (long long)k * 3 + 1);
    }
    long long vsum = 0, ksum = 0; size_t live = 0;
    for (size_t i = 0; i < m.cap; ++i)
        if (m.tag[i] == 1) { vsum += m.val[i]; ksum += (long long)m.keys[i]; ++live; }
    std::printf("n=%lld dn=%lld vsum=%lld ksum=%lld\n", (long long)m.n, (long long)live, vsum, ksum);
}

static void shape_h5() {
    Flat m;
    m.init((size_t)N);
    Rng r{SEED0};
    for (long long i = 0; i < N; ++i) {
        uint64_t k = next(r) % SPACE;
        m.put(k, (long long)k * 3 + 1);
    }
    Rng r2{SEED0 ^ 0x5DEECE66Dull};
    long long misses = 0, sum = 0;
    for (long long i = 0; i < N * 2; ++i) {
        uint64_t k = next(r2) % SPACE;
        long long v = -1;
        if (m.get(k, v)) { if (v != -1) sum += v; else ++misses; } else ++misses;
    }
    std::printf("n=%lld misses=%lld sum=%lld\n", (long long)m.n, misses, sum);
}

static void shape_p1() {
    Flat m;
    m.init((size_t)N);
    Rng r{SEED0};
    for (long long i = 0; i < N; ++i) {
        uint64_t k = next(r) % SPACE;
        m.put(k, (long long)k * 3 + 1);
    }
    std::printf("n=%lld\n", (long long)m.n);
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: h_flat h1|h2|h3|h4|h5\n"); return 2; }
    if (!std::strcmp(argv[1], "h1")) shape_build(true);
    else if (!std::strcmp(argv[1], "h2")) shape_build(false);
    else if (!std::strcmp(argv[1], "h3")) shape_h3();
    else if (!std::strcmp(argv[1], "h4")) shape_h4();
    else if (!std::strcmp(argv[1], "h5")) shape_h5();
    else if (!std::strcmp(argv[1], "p1")) shape_p1();
    else { std::fprintf(stderr, "unknown shape: %s\n", argv[1]); return 2; }
    return 0;
}

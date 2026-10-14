// 与 bench/app/route.extc 同算法同参数（std::map，红黑树）
//
// 场景 B：路由表。① 插入/更新 ops 条 ② 前驱查找 ops 次 ③ 区间扫描 ops/4 次 ④ 删除 ops/2 条。
// 用法：route <ops>   （默认 2000000）
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>

/* ================ 应用侧逐单位时间戳（USER-STALL / bench/app） ================
 * 只写**戳文件**（路径 = argv 最后一项）；stdout 一个字节都不动。
 * 40 字节头 + (nstamps-1) 个 unsigned-LEB128 增量（小端）。
 * 戳数 = 单位数 + 1：第一次 us() 记 t0，其后每次记一个增量，循环后补最后一次。
 * ============================================================================ */
#include <chrono>
#include <cstdint>
#include <fstream>
#include <vector>

static const std::size_t US_BODY = 40;
static std::vector<uint8_t> g_usBuf;
static std::size_t g_usCap = 0;
static long long g_usN = 0, g_usT0 = 0, g_usPrev = 0, g_usNeg = 0, g_usOvf = 0;
static bool g_usStart = false;

static inline long long usNow() {
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static inline void us() {
    long long t = usNow();
    if (!g_usStart) {
        g_usStart = true;
        g_usT0 = t;
        g_usPrev = t;
        g_usN = 1;
        return;
    }
    long long d = t - g_usPrev;
    if (d < 0) {
        g_usNeg++;
        d = 0;
    }
    g_usPrev = t;
    g_usN++;
    unsigned long long v = (unsigned long long)d;
    while (v >= 128) {
        if (g_usBuf.size() < g_usCap) g_usBuf.push_back((uint8_t)((v & 127u) | 128u));
        else g_usOvf++;
        v >>= 7;
    }
    if (g_usBuf.size() < g_usCap) g_usBuf.push_back((uint8_t)v);
    else g_usOvf++;
}

static void usReserve(long long units) {
    g_usCap = (std::size_t)(units * 5 + 64);
    g_usBuf.reserve(g_usCap);
}

static void usDump(const char* path) {
    uint8_t hdr[US_BODY];
    for (std::size_t i = 0; i < US_BODY; i++) hdr[i] = 0;
    const char* m = "USAPPST1";
    for (int i = 0; i < 8; i++) hdr[i] = (uint8_t)m[i];
    unsigned long long vals[4] = {(unsigned long long)g_usN, (unsigned long long)g_usT0,
                                  (unsigned long long)g_usNeg, (unsigned long long)g_usOvf};
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 8; i++) hdr[8 + j * 8 + i] = (uint8_t)(vals[j] >> (8 * i));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)hdr, (std::streamsize)US_BODY);
    if (!g_usBuf.empty()) f.write((const char*)g_usBuf.data(), (std::streamsize)g_usBuf.size());
    f.close();
}

static int64_t toInt(const char* s, int64_t dflt) {
    if (s == nullptr || *s == '\0') return dflt;
    int64_t v = 0;
    for (const char* p = s; *p != '\0'; p++) {
        int64_t b = (int64_t)(unsigned char)*p;
        if (b < 48 || b > 57) return dflt;
        v = v * 10 + (b - 48);
    }
    return v;
}

int main(int argc, char** argv) {
    int64_t ops = 2000000;
    if (argc > 1) ops = toInt(argv[1], ops);
    const char* usPath = (argc > 2) ? argv[argc - 1] : "";
    usReserve((long long)(ops + ops + ops/4 + ops/2));

    std::map<int64_t, int32_t> routes;
    uint64_t u = 12345;
    int64_t ins = 0;
    int64_t lkp = 0;
    int64_t scan = 0;
    int64_t del = 0;

    // ① 插入 / 更新：键是 /24 前缀（低 8 位清零）
    for (int64_t i = 0; i < ops; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t key = (int64_t)(u & 0xFFFFFF00ULL);
        us();
        auto r = routes.insert({key, (int32_t)(key % 1024)});
        if (r.second) ins++;
    }

    // ② 前驱查找：最后一个 ≤ q 的前缀（upper_bound(q) 的前一个）
    u = 12345;
    for (int64_t i = 0; i < ops; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t q = (int64_t)(u & 0xFFFFFFFFULL);
        us();
        auto it = routes.upper_bound(q);
        if (it == routes.begin()) {
            lkp -= 1;
        } else {
            --it;
            lkp += (int64_t)it->second;
        }
    }

    // ③ 区间扫描：数 [lo, lo+65536) 里的路由条数
    u = 12345;
    for (int64_t i = 0; i < ops / 4; i++) {
        us();
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t lo = (int64_t)(u & 0xFFFF0000ULL);
        int64_t hi = lo + 65536;
        scan += (int64_t)std::distance(routes.lower_bound(lo), routes.lower_bound(hi));
    }

    // ④ 删除一半
    u = 12345;
    for (int64_t i = 0; i < ops / 2; i++) {
        us();
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t key = (int64_t)(u & 0xFFFFFF00ULL);
        if (routes.erase(key)) del++;
    }

    us();
    usDump(usPath);
    printf("b.ins=%lld b.lkp=%lld b.scan=%lld b.del=%lld b.len=%lld\n",
           (long long)ins, (long long)(lkp % 1000000007), (long long)scan,
           (long long)del, (long long)routes.size());
    return 0;
}

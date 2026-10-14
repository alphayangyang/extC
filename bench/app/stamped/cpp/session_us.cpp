// 与 bench/app/session.extc 同算法同参数（std::unordered_map）
//
// 场景 A：会话 / 连接表。查找(60%) + 建/更(20%) + 删(20%)，键为 i64 连接 id。
// 用法：session <ops> <active>   （默认 2000000 / 65536）
#include <cstdint>
#include <cstdio>
#include <unordered_map>

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

struct Session {
    int64_t user;
    int32_t state;
    int64_t bytes;
    int64_t last;
};

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
    int64_t active = 65536;
    if (argc > 1) ops = toInt(argv[1], ops);
    if (argc > 2) active = toInt(argv[2], active);
    const char* usPath = (argc > 3) ? argv[argc - 1] : "";
    usReserve((long long)(ops));

    std::unordered_map<int64_t, Session> m;
    m.reserve((size_t)active);
    int64_t acc = 0;
    int64_t hits = 0;
    for (int64_t i = 0; i < ops; i++) {
        us();
        int64_t k = i % active;   // 活跃集
        int64_t op = i % 977;     // 与 2 的幂互质 ⇒ 相位逐轮错开
        int64_t ph = op % 5;      // 0,1,2 = 查；3 = 建/更；4 = 删
        if (ph < 3) {
            // 必须用 find，不能用 m[k]（后者会插入）
            auto it = m.find(k);
            int64_t b = (it == m.end()) ? 0 : it->second.bytes;
            acc += b;
            if (b > 0) hits++;
        } else if (ph == 3) {
            Session s{k % 1024, 1, i + k, i};
            m[k] = s;
        } else {
            if (m.erase(k)) acc += 1;
        }
    }
    us();
    usDump(usPath);
    printf("a.len=%lld a.hits=%lld a.acc=%lld\n",
           (long long)m.size(), (long long)hits, (long long)(acc % 1000000007));
    return 0;
}

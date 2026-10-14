// 与 bench/app/log.extc 同算法同参数（std::string）
//
// 场景 C：有界日志缓冲。定长 24 字节记录顺序追加，满 CAP 就丢掉最老一半、留最新 KEEP，
// 每 512 轮在缓冲里 find 上一条记录的前 4 个字节。
// 用法：log <ops>   （默认 2000000）
#include <cstdint>
#include <cstdio>
#include <string>

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

static const int64_t CAP = 16384;
static const int64_t KEEP = 8192;
static const int64_t REC = 24;

// 第 i 条记录的第 j 个字节：33..122
static inline uint8_t recByte(int64_t i, int64_t j) {
    return (uint8_t)(33 + ((i + j * 3) % 90));
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
    usReserve((long long)(ops));

    std::string s;
    s.reserve((size_t)CAP);
    int64_t trunc = 0;
    int64_t findSum = 0;
    int64_t acc = 0;
    for (int64_t i = 0; i < ops; i++) {
        us();
        for (int64_t j = 0; j < REC; j++) s.push_back((char)recByte(i, j));
        if (i % 512 == 0 && i > 0) {
            std::string needle;
            needle.reserve(4);
            for (int64_t j = 0; j < 4; j++) needle.push_back((char)recByte(i - 1, j));
            size_t at = s.find(needle);
            findSum += (at == std::string::npos) ? -1 : (int64_t)at;
        }
        if ((int64_t)s.size() >= CAP) {
            acc += (int64_t)(uint8_t)s[0] + (int64_t)(uint8_t)s[s.size() - 1];
            s.erase(0, s.size() - (size_t)KEEP);   // 丢掉最老的，留最新 KEEP
            trunc++;
        }
    }
    us();
    usDump(usPath);
    printf("c.appended=%lld c.trunc=%lld c.find=%lld c.acc=%lld c.len=%lld\n",
           (long long)(ops * REC), (long long)trunc, (long long)(findSum % 1000000007),
           (long long)acc, (long long)s.size());
    return 0;
}

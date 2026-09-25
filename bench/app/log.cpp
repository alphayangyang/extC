// 与 bench/app/log.extc 同算法同参数（std::string）
//
// 场景 C：有界日志缓冲。定长 24 字节记录顺序追加，满 CAP 就丢掉最老一半、留最新 KEEP，
// 每 512 轮在缓冲里 find 上一条记录的前 4 个字节。
// 用法：log <ops>   （默认 2000000）
#include <cstdint>
#include <cstdio>
#include <string>

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

    std::string s;
    s.reserve((size_t)CAP);
    int64_t trunc = 0;
    int64_t findSum = 0;
    int64_t acc = 0;
    for (int64_t i = 0; i < ops; i++) {
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
    printf("c.appended=%lld c.trunc=%lld c.find=%lld c.acc=%lld c.len=%lld\n",
           (long long)(ops * REC), (long long)trunc, (long long)(findSum % 1000000007),
           (long long)acc, (long long)s.size());
    return 0;
}

// bench/string_vs_cpp/cpp.cpp —— C++ `std::string` 这一侧的横评程序。
//
// 与 s.extc 逐条等价：同样的次数、同样的字节、同样的公式；每个场景打印一行 `名字 cs=整数`，
// 校验和两边必须逐位相同。计时与 RSS 都在外面量（见 run.sh），程序里不放计时器。
// 另有一个不进表的 `noop`：什么都不做，只为在同一口径下量出"进程启动 + 运行时"这个常数项。
// `argv[2]` 是可选的盐水（取它第一个字节，不给就是 0）：内容恒定的那两个循环不加它会被
// g++ 整个折成常数。默认 0 时推的字节与不加盐完全一样。
//
// 只准用 std::string 的原生接口：push_back / append / reserve / find / substr /
// == / < / size / capacity / empty / clear / shrink_to_fit / erase(0,n)。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 只在这一格用：告诉编译器"这个对象的内存被外面的东西读过了"（Google Benchmark 的
// DoNotOptimize 同款，一条 `asm volatile` 空语句，不产生指令）。不加的话 g++ 会把这
// 100 万次 push 整个折成一句 `mov $0x6146580,%eax`（实测：内层耗时 0.000 ms）——
// 那这一格量的就不是 std::string 而是"gcc 会不会做算术"。extC 侧不需要：
// 它生成的 C 里 `push` 是一次真实的跨函数调用，gcc 折不掉。
#define BENCH_BARRIER(x) asm volatile("" : : "r,m"(x) : "memory")

// ---------------------------------------------------------------- 公共公式（与 s.extc 同）
static inline uint8_t lcgByte(int64_t i) {
    return (uint8_t)(((i * 1103515245LL + 12345LL) >> 16) & 255);
}

static inline uint64_t xs(uint64_t x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

static int64_t sampleSum(const std::string& s, int64_t step) {
    int64_t acc = 0;
    for (int64_t p = 0; p < (int64_t)s.size(); p += step) acc += (int64_t)(unsigned char)s[(size_t)p];
    return acc;
}

static int64_t triple(const std::string& s) {
    if (s.empty()) return 0;
    int64_t a = (int64_t)(unsigned char)s[0];
    int64_t b = (int64_t)(unsigned char)s[s.size() / 2];
    int64_t c = (int64_t)(unsigned char)s[s.size() - 1];
    return a * 3 + b * 5 + c * 7;
}

// ================================================================ 1. short_many
static int64_t scene_short_many(int64_t salt) {
    int64_t acc = 0;
    for (int64_t i = 0; i < 1000000; i++) {
        std::string s;
        s.push_back((char)((97 + (i % 26)) ^ salt));
        s.push_back((char)(98 + (i % 24)));
        s.push_back((char)99);
        BENCH_BARRIER(s);
        acc += (int64_t)s.size() + (int64_t)(unsigned char)s[2];
    }
    return acc;
}

// ================================================================ 2. short_cross
static int64_t scene_short_cross(int64_t salt) {
    int64_t acc = 0;
    for (int64_t i = 0; i < 1000000; i++) {
        std::string s;
        for (int64_t k = 0; k < 20; k++) s.push_back((char)((32 + k) ^ salt));
        acc += (int64_t)s.size() + (int64_t)(unsigned char)s[19];
    }   // ← 析构：这一轮的块在这里还回去（extC 那边是显式 release()）
    return acc;
}

// ================================================================ 3. long_build_byte
static int64_t scene_long_build_byte() {
    std::string s;
    for (int64_t i = 0; i < 4000000; i++) s.push_back((char)lcgByte(i));
    return (int64_t)s.size() + triple(s) + sampleSum(s, 4096);
}

// ================================================================ 4. long_build_chunk
static int64_t scene_long_build_chunk() {
    const int64_t CH = 4096;
    const int64_t TOT = 67108864;
    uint8_t buf[4096];
    std::string s;
    for (int64_t done = 0; done < TOT; done += CH) {
        for (int64_t k = 0; k < CH; k++) buf[k] = lcgByte(done + k);
        s.append((const char*)buf, (size_t)CH);
    }
    return (int64_t)s.size() + triple(s) + sampleSum(s, 1048576);
}

// ================================================================ 5. reserve_growth
static int64_t scene_reserve_growth() {
    const int64_t TOT = 67108864;
    std::string s;
    s.reserve((size_t)TOT);
    for (int64_t i = 0; i < TOT; i++) s.push_back((char)lcgByte(i));
    return (int64_t)s.size() + triple(s) + sampleSum(s, 1048576);
}

// ================================================================ 6. read_at
static int64_t scene_read_at() {
    const int64_t N = 1048576;
    std::string s;
    s.reserve((size_t)N);
    for (int64_t i = 0; i < N; i++) s.push_back((char)lcgByte(i));
    uint64_t st = 88172645463325252ULL;
    int64_t acc = 0;
    for (int64_t i = 0; i < 10000000; i++) {
        st = xs(st);
        int64_t idx = (int64_t)((st >> 16) % (uint64_t)N);
        acc += (int64_t)(unsigned char)s[(size_t)idx];   // 无检查（extC 那边是 at()，有界检查）
    }
    return acc + (int64_t)s.size();
}

// ================================================================ 7. find
static int64_t scene_find() {
    const int64_t N = 1048576;
    std::string s;
    s.reserve((size_t)N);
    for (int64_t i = 0; i < N; i++) s.push_back((char)lcgByte(i));
    uint8_t pat[16];
    int64_t acc = 0;
    for (int64_t i = 0; i < 10000; i++) {
        int64_t L = 4 + (i % 12);
        int64_t pos = (i * 7919) % (N - 32);
        for (int64_t k = 0; k < L; k++) {
            int64_t b = (int64_t)lcgByte(pos + k);
            if (k == 0 && (i % 2) == 0) b = (b + 1) & 255;
            pat[k] = (uint8_t)b;
        }
        std::string needle((const char*)pat, (size_t)L);   // L <= 15 ⇒ SSO，不分配
        size_t at = s.find(needle);
        acc += (at == std::string::npos) ? 0 : (int64_t)at + 1;
    }
    return acc;
}

// ================================================================ 8. compare
static int64_t scene_compare() {
    std::string a, b;
    a.reserve(64);
    b.reserve(64);
    for (int64_t i = 0; i < 32; i++) {
        a.push_back((char)(97 + (i % 26)));
        b.push_back((char)(97 + ((i + 13) % 26)));
    }
    int64_t eq = 0, lt = 0;
    for (int64_t i = 0; i < 1000000; i++) {
        int64_t x = i & 255;
        a.push_back((char)x);
        a.erase(0, 1);
        b.push_back((char)((x + 128) & 255));
        b.erase(0, 1);
        if (a == b) eq++;
        if (a < b) lt++;
    }
    return eq * 1000 + lt;
}

// ================================================================ 9. substr_copy
static int64_t scene_substr_copy() {
    const int64_t N = 1048576;
    std::string s;
    s.reserve((size_t)N);
    for (int64_t i = 0; i < N; i++) s.push_back((char)lcgByte(i));
    int64_t acc = 0;
    for (int64_t i = 0; i < 100000; i++) {
        std::string p = s.substr(1000, 1000);   // 半开区间 [1000,2000) = 1000 字节，与 extC sub(1000,2000) 同一段
        acc += (int64_t)p.size() + (int64_t)(unsigned char)p[0] + (int64_t)(unsigned char)p[999];
    }
    return acc;
}

// ================================================================ 10. clone_many
static int64_t scene_clone_many() {
    std::string base;
    base.reserve(64);
    for (int64_t i = 0; i < 32; i++) base.push_back((char)(97 + (i % 26)));
    int64_t acc = 0;
    for (int64_t i = 0; i < 1000000; i++) {
        std::string c = base;   // 深拷贝（extC 那边是 clone()）
        acc += (int64_t)c.size() + (int64_t)(unsigned char)c[31];
    }   // ← 析构归还
    return acc;
}

// ================================================================ 11. clear_shrink_cycle
static int64_t scene_clear_shrink_cycle() {
    const int64_t CH = 4096;
    const int64_t TOT = 65536;
    uint8_t buf[4096];
    std::string s;
    int64_t acc = 0;
    for (int64_t round = 0; round < 10000; round++) {
        s.clear();
        for (int64_t done = 0; done < TOT; done += CH) {
            for (int64_t k = 0; k < CH; k++) buf[k] = lcgByte(round * TOT + done + k);
            s.append((const char*)buf, (size_t)CH);
        }
        acc += (int64_t)s.size() + (int64_t)(unsigned char)s[0] + (int64_t)(unsigned char)s[65535];
        s.clear();
        s.shrink_to_fit();
    }
    return acc + (int64_t)s.size();
}

// ================================================================ 12. many_live_short
static int64_t scene_many_live_short() {
    const int64_t N = 1000000;
    std::vector<std::string> arr((size_t)N);
    for (int64_t i = 0; i < N; i++)
        for (int64_t k = 0; k < 8; k++) arr[(size_t)i].push_back((char)(65 + ((i + k) % 26)));
    int64_t acc = 0;
    for (int64_t i = 0; i < N; i++)
        acc += (int64_t)arr[(size_t)i].size() + (int64_t)(unsigned char)arr[(size_t)i][7];
    return acc;
}

// ================================================================ 调度
static bool run(const char* name, int64_t salt) {
    // 地板：什么都不做，只打印一个 cs（量"进程启动 + 运行时"这个常数项，不进结果表）
    if (!strcmp(name, "noop"))               { printf("noop cs=0\n");                                                                   return true; }
    if (!strcmp(name, "short_many"))         { printf("short_many cs=%lld\n",         (long long)scene_short_many(salt));     return true; }
    if (!strcmp(name, "short_cross"))        { printf("short_cross cs=%lld\n",        (long long)scene_short_cross(salt));    return true; }
    if (!strcmp(name, "long_build_byte"))    { printf("long_build_byte cs=%lld\n",    (long long)scene_long_build_byte());    return true; }
    if (!strcmp(name, "long_build_chunk"))   { printf("long_build_chunk cs=%lld\n",   (long long)scene_long_build_chunk());   return true; }
    if (!strcmp(name, "reserve_growth"))     { printf("reserve_growth cs=%lld\n",     (long long)scene_reserve_growth());     return true; }
    if (!strcmp(name, "read_at"))            { printf("read_at cs=%lld\n",            (long long)scene_read_at());            return true; }
    if (!strcmp(name, "find"))               { printf("find cs=%lld\n",               (long long)scene_find());               return true; }
    if (!strcmp(name, "compare"))            { printf("compare cs=%lld\n",            (long long)scene_compare());            return true; }
    if (!strcmp(name, "substr_copy"))        { printf("substr_copy cs=%lld\n",        (long long)scene_substr_copy());        return true; }
    if (!strcmp(name, "clone_many"))         { printf("clone_many cs=%lld\n",         (long long)scene_clone_many());         return true; }
    if (!strcmp(name, "clear_shrink_cycle")) { printf("clear_shrink_cycle cs=%lld\n", (long long)scene_clear_shrink_cycle()); return true; }
    if (!strcmp(name, "many_live_short"))    { printf("many_live_short cs=%lld\n",    (long long)scene_many_live_short());    return true; }
    return false;
}

int main(int argc, char** argv) {
    // 可选的"盐水"（argv[2] 的第一个字节，不给就是 0）：只为了让**内容恒定**的那两个循环
    // 在编译期不可知 —— 否则 g++ 会把它们整个折成常数（实测：100 万次 push 变成一句
    // `mov $0x6146580,%eax`）。默认 0 时推的字节与不加盐完全一样 ⇒ 校验和照旧逐位相同。
    int64_t salt = 0;
    if (argc > 2) salt = (int64_t)(unsigned char)argv[2][0] & 255;
    if (argc > 1) {
        if (!run(argv[1], salt)) { printf("unknown scenario: %s\n", argv[1]); return 2; }
        return 0;
    }
    run("short_many", salt);
    run("short_cross", salt);
    run("long_build_byte", salt);
    run("long_build_chunk", salt);
    run("reserve_growth", salt);
    run("read_at", salt);
    run("find", salt);
    run("compare", salt);
    run("substr_copy", salt);
    run("clone_many", salt);
    run("clear_shrink_cycle", salt);
    run("many_live_short", salt);
    return 0;
}

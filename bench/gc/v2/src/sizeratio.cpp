// bench/gc/v2 形状 G2 sizeratio —— C++ 自然写法。
// 与 src/sizeratio.extc 同算法 / 同 LCG / 同求值顺序，stdout 必须逐字节一致。
//
// 数据结构：四个 POD 尺寸类（32/64/96/128 字节，sizeof 精确等于规格值），
//           每类一张 1024 槽定长表（std::vector 只按下标读写）；对象裸 new，
//           7/8 立即成垃圾、1/8 进表顶掉旧对象 —— 都不 delete，由 GC 语义决定生死。
// 两条臂：默认臂 -DSIZERATIO_NAIVE 与 -DSIZERATIO_CAREFUL 源码相同（本形状无环）。
//
// 构建：g++ -O2 -std=c++17 -DSIZERATIO_NAIVE   src/sizeratio.cpp -o b/sizeratio.cpp.bin
//       g++ -O2 -std=c++17 -DSIZERATIO_CAREFUL src/sizeratio.cpp -o b/sizeratio.cpp_careful.bin
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int64_t MASK = 4611686018427387903LL;
constexpr int64_t SLOTS = 1024;
constexpr int64_t SZ0 = 32, SZ1 = 64, SZ2 = 96, SZ3 = 128;

int64_t gstate = 1;

int64_t nxt() {
    gstate = (gstate * 1103515245 + 12345) & 2147483647;
    return gstate;
}

int64_t pick(int64_t k) { return (nxt() / 1024) % k; }

int64_t parseI64(const char* s) {
    int64_t v = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') break;
        v = v * 10 + (*p - '0');
    }
    return v;
}

// POD 尺寸类：k + p[n] 恰好铺满 32/64/96/128 字节，无尾部填充。
struct C0 { int64_t k; int64_t p[3]; };    // 32 B
struct C1 { int64_t k; int64_t p[7]; };    // 64 B
struct C2 { int64_t k; int64_t p[11]; };   // 96 B
struct C3 { int64_t k; int64_t p[15]; };   // 128 B
static_assert(sizeof(C0) == SZ0, "C0 size");
static_assert(sizeof(C1) == SZ1, "C1 size");
static_assert(sizeof(C2) == SZ2, "C2 size");
static_assert(sizeof(C3) == SZ3, "C3 size");

} // namespace

int main(int argc, char** argv) {
    int64_t n = 2000000;
    int64_t sd = 20261013;
    if (argc > 1) n = parseI64(argv[1]);
    if (argc > 2) sd = parseI64(argv[2]);
    gstate = sd & 2147483647;

    std::vector<C0*> t0(SLOTS, nullptr);
    std::vector<C1*> t1(SLOTS, nullptr);
    std::vector<C2*> t2(SLOTS, nullptr);
    std::vector<C3*> t3(SLOTS, nullptr);
    int64_t sum = 0, kept = 0, livebytes = 0;
    for (int64_t i = 0; i < n; ++i) {
        const int64_t cls = pick(4);
        if (cls == 0) {
            C0* o = new C0;
            o->k = i;
            sum = (sum + o->k) & MASK;
            if (i % 8 == 0) {
                const int64_t b = (i / 8) % SLOTS;
                if (t0[b] != nullptr) livebytes -= SZ0; else ++kept;
                t0[b] = o;
                livebytes += SZ0;
            }
        } else if (cls == 1) {
            C1* o = new C1;
            o->k = i;
            sum = (sum + o->k) & MASK;
            if (i % 8 == 0) {
                const int64_t b = (i / 8) % SLOTS;
                if (t1[b] != nullptr) livebytes -= SZ1; else ++kept;
                t1[b] = o;
                livebytes += SZ1;
            }
        } else if (cls == 2) {
            C2* o = new C2;
            o->k = i;
            sum = (sum + o->k) & MASK;
            if (i % 8 == 0) {
                const int64_t b = (i / 8) % SLOTS;
                if (t2[b] != nullptr) livebytes -= SZ2; else ++kept;
                t2[b] = o;
                livebytes += SZ2;
            }
        } else {
            C3* o = new C3;
            o->k = i;
            sum = (sum + o->k) & MASK;
            if (i % 8 == 0) {
                const int64_t b = (i / 8) % SLOTS;
                if (t3[b] != nullptr) livebytes -= SZ3; else ++kept;
                t3[b] = o;
                livebytes += SZ3;
            }
        }
    }
    std::printf("ops=%lld kept=%lld livebytes=%lld sum=%lld\n",
                (long long)n, (long long)kept, (long long)livebytes, (long long)sum);
    std::printf("classes=4 sizes=32..128 slots=1024\n");
    return 0;
}

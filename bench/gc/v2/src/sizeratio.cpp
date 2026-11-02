// bench/gc/v2 形状 G2 sizeratio —— C++ 自然写法（两条臂内存纪律不同）。
// 与 src/sizeratio.extc 同算法 / 同 LCG / 同求值顺序，stdout 必须逐字节一致。
//
// 数据结构：四个 POD 尺寸类（32/64/96/128 字节，sizeof 精确等于规格值），
//           每类一张 1024 槽定长表（std::vector 只按下标读写）；对象裸 new，
//           7/8 立即成垃圾、1/8 进表顶掉旧对象。
//   naive  (-DSIZERATIO_NAIVE)  ：从不 delete，生死交给 GC 语义 —— 按"有 GC 的语言"的习惯写。
//   careful(-DSIZERATIO_CAREFUL)：显式释放（走 FAIR 同一条路径）。
//   fair   (-DSIZERATIO_FAIR)   ：与 careful 同一路径的别名（run.sh 用 _CAREFUL 构建该臂，
//                                  这里同时接受 _FAIR，两条宏名出同一个可执行体）。
// FAIR 臂：显式释放，让 C++ 与 extC/Rust 在同一个内存纪律下比较。覆盖槽里被顶掉的旧对象、
// 以及本轮没进表、立刻成垃圾的对象，都在这里 delete；只改释放时机，不改任何输出。
// 程序退出时表里还剩 ≤ 4096 个活对象，不释放：那不影响峰值 RSS，只会给 C++ 臂加上
// 其它语言没有的收尾成本（本卡具只比 stdout 与峰值）。
//
// 构建：g++ -O2 -std=c++17 -DSIZERATIO_NAIVE   src/sizeratio.cpp -o b/sizeratio.cpp.bin
//       g++ -O2 -std=c++17 -DSIZERATIO_CAREFUL src/sizeratio.cpp -o b/sizeratio.cpp_careful.bin
//       g++ -O2 -std=c++17 -DSIZERATIO_FAIR    src/sizeratio.cpp -o b/sizeratio.cpp_fair.bin
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

#if defined(SIZERATIO_CAREFUL) || defined(SIZERATIO_FAIR)
// 防消解（SPEC §0 坑 1）：-O2 会把「new 完立刻 delete」整对对消，这条臂就根本不再分配，
// 计时会虚假变快。释放前先把指针写进这个只写不读的 volatile 槽，分配因此不可省
// （同 SPEC 建议的「把最近一次的头存进全局/静态槽」）。槽本身不参与任何输出。
void* volatile g_freeSink = nullptr;
#endif

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
                if (t0[b] != nullptr) {
                    livebytes -= SZ0;
#if defined(SIZERATIO_CAREFUL) || defined(SIZERATIO_FAIR)
                    // FAIR 臂：显式释放，让 C++ 与 extC/Rust 在同一个内存纪律下比较。
                    // 这个槽是旧对象的最后一位持有者，顶掉之前先 delete 它。
                    g_freeSink = t0[b];
                    delete t0[b];
#endif
                } else {
                    ++kept;
                }
                t0[b] = o;
                livebytes += SZ0;
            }
#if defined(SIZERATIO_CAREFUL) || defined(SIZERATIO_FAIR)
            else {
                // FAIR 臂：本轮没进表、立刻成垃圾的对象，当场释放。
                g_freeSink = o;
                delete o;
            }
#endif
        } else if (cls == 1) {
            C1* o = new C1;
            o->k = i;
            sum = (sum + o->k) & MASK;
            if (i % 8 == 0) {
                const int64_t b = (i / 8) % SLOTS;
                if (t1[b] != nullptr) {
                    livebytes -= SZ1;
#if defined(SIZERATIO_CAREFUL) || defined(SIZERATIO_FAIR)
                    // FAIR 臂：顶掉槽里的旧对象之前显式 delete（同一内存纪律）。
                    g_freeSink = t1[b];
                    delete t1[b];
#endif
                } else {
                    ++kept;
                }
                t1[b] = o;
                livebytes += SZ1;
            }
#if defined(SIZERATIO_CAREFUL) || defined(SIZERATIO_FAIR)
            else {
                // FAIR 臂：本轮没进表、立刻成垃圾的对象，当场释放。
                g_freeSink = o;
                delete o;
            }
#endif
        } else if (cls == 2) {
            C2* o = new C2;
            o->k = i;
            sum = (sum + o->k) & MASK;
            if (i % 8 == 0) {
                const int64_t b = (i / 8) % SLOTS;
                if (t2[b] != nullptr) {
                    livebytes -= SZ2;
#if defined(SIZERATIO_CAREFUL) || defined(SIZERATIO_FAIR)
                    // FAIR 臂：顶掉槽里的旧对象之前显式 delete（同一内存纪律）。
                    g_freeSink = t2[b];
                    delete t2[b];
#endif
                } else {
                    ++kept;
                }
                t2[b] = o;
                livebytes += SZ2;
            }
#if defined(SIZERATIO_CAREFUL) || defined(SIZERATIO_FAIR)
            else {
                // FAIR 臂：本轮没进表、立刻成垃圾的对象，当场释放。
                g_freeSink = o;
                delete o;
            }
#endif
        } else {
            C3* o = new C3;
            o->k = i;
            sum = (sum + o->k) & MASK;
            if (i % 8 == 0) {
                const int64_t b = (i / 8) % SLOTS;
                if (t3[b] != nullptr) {
                    livebytes -= SZ3;
#if defined(SIZERATIO_CAREFUL) || defined(SIZERATIO_FAIR)
                    // FAIR 臂：顶掉槽里的旧对象之前显式 delete（同一内存纪律）。
                    g_freeSink = t3[b];
                    delete t3[b];
#endif
                } else {
                    ++kept;
                }
                t3[b] = o;
                livebytes += SZ3;
            }
#if defined(SIZERATIO_CAREFUL) || defined(SIZERATIO_FAIR)
            else {
                // FAIR 臂：本轮没进表、立刻成垃圾的对象，当场释放。
                g_freeSink = o;
                delete o;
            }
#endif
        }
    }
    std::printf("ops=%lld kept=%lld livebytes=%lld sum=%lld\n",
                (long long)n, (long long)kept, (long long)livebytes, (long long)sum);
    std::printf("classes=4 sizes=32..128 slots=1024\n");
    return 0;
}

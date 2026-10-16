// bench/gc/v2 形状 G4 cycleratio —— C++ 自然写法（两臂语义不同）。
// 与 src/cycleratio.extc 同算法 / 同 LCG / 同求值顺序，stdout 必须逐字节一致。
//
// 数据结构：节点 = { k, v, next }，next 是 std::shared_ptr<Node>（SPEC §0 指定的 C++ 自然引用）；
//           窗口 = 256 槽，每槽存 (头引用, 组节点数)。每组建链、以 f% 概率闭成环，
//           然后顶掉 i%256 槽的旧组。
//   naive  (-DCYCLERATIO_NAIVE)  ：next 是强引用。成环组被顶掉后仍互相持有 ⇒ 不可达环收不掉（泄漏）。
//   careful(-DCYCLERATIO_CAREFUL)：顶掉前先断环（顺着 next 清一遍链），引用计数级联归零。
// 两臂 stdout 相同（计数只看窗口记账，不看可达性）。
//
// 构建：g++ -O2 -std=c++17 -DCYCLERATIO_NAIVE   src/cycleratio.cpp -o b/cycleratio.cpp.bin
//       g++ -O2 -std=c++17 -DCYCLERATIO_CAREFUL src/cycleratio.cpp -o b/cycleratio.cpp_careful.bin
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int64_t MASK = 4611686018427387903LL;
constexpr int64_t WINDOW = 256;

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

struct Node {
    int64_t k = 0;
    int64_t v = 0;
    std::shared_ptr<Node> next;
};

struct Slot {
    std::shared_ptr<Node> head;
    int64_t n = 0;
};

} // namespace

int main(int argc, char** argv) {
    int64_t g = 400000;
    int64_t sd = 20261013;
    int64_t f = 100;
    if (argc > 1) g = parseI64(argv[1]);
    if (argc > 2) sd = parseI64(argv[2]);
    if (argc > 3) f = parseI64(argv[3]);
    gstate = sd & 2147483647;

    std::vector<Slot> win(WINDOW);
    int64_t sum = 0, made = 0, evicted = 0, dropnodes = 0, rings = 0;
    for (int64_t i = 0; i < g; ++i) {
        const int64_t k = 3 + pick(4);
        const bool isring = pick(100) < f;
        std::shared_ptr<Node> first, prev;
        for (int64_t j = 0; j < k; ++j) {
            std::shared_ptr<Node> s = std::make_shared<Node>();
            s->k = i;
            s->v = nxt() % 1000;
            s->next.reset();
            if (!first) first = s;
            if (prev) prev->next = s;
            prev = s;
            sum = (sum + s->v) & MASK;
        }
        if (isring) {
            ++rings;
            if (prev) prev->next = first;   // 闭成环
        }
        made += k;
        const int64_t b = i % WINDOW;
        if (win[b].head) {
            ++evicted;
            dropnodes += win[b].n;
#if defined(CYCLERATIO_CAREFUL)
            // careful：顶掉前断环。顺链清 next；成环组因此被打破、引用计数级联归零。
            std::shared_ptr<Node> p = win[b].head;
            for (int64_t t = 0; t < win[b].n && p; ++t) {
                std::shared_ptr<Node> nx = p->next;
                p->next.reset();
                p = nx;
            }
#endif
        }
        win[b].head = first;
        win[b].n = k;
    }
    std::printf("groups=%lld made=%lld evicted=%lld dropnodes=%lld sum=%lld\n",
                (long long)g, (long long)made, (long long)evicted, (long long)dropnodes,
                (long long)sum);
    std::printf("f=%lld window=256 kmin=3 kmax=6 rings=%lld\n", (long long)f, (long long)rings);
    return 0;
}

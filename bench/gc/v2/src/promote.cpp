// bench/gc/v2 形状 G1 promote —— C++ 自然写法。
// 与 src/promote.extc 同算法 / 同 LCG / 同求值顺序，stdout 必须逐字节一致。
//
// 数据结构：手写单向链节点（裸 new，短命节点不做 delete —— 交给 GC 才是本形状的语义），
//           长命表 = 65536 槽的定长数组（std::vector 只按下标读写，不当动态容器用）。
// 两条臂：默认臂 -DPROMOTE_NAIVE 与 -DPROMOTE_CAREFUL 源码相同（G1 无环，语义无差别）。
//
// 构建：g++ -O2 -std=c++17 -DPROMOTE_NAIVE   src/promote.cpp -o b/promote.cpp.bin
//       g++ -O2 -std=c++17 -DPROMOTE_CAREFUL src/promote.cpp -o b/promote.cpp_careful.bin
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int64_t MASK = 4611686018427387903LL;  // 2^62-1
constexpr int64_t TABLE = 65536;
constexpr int64_t SHORT = 9;
constexpr int64_t EVERY = 10;
constexpr int64_t WALKCAP = 8;

int64_t gstate = 1;

int64_t nxt() {
    gstate = (gstate * 1103515245 + 12345) & 2147483647;
    return gstate;
}

// 逐字符解析，遇非数字停（与 extC 参考实现同上限）。
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
    Node* next = nullptr;
};

} // namespace

int main(int argc, char** argv) {
    int64_t n = 500000;
    int64_t sd = 20261013;
    if (argc > 1) n = parseI64(argv[1]);
    if (argc > 2) sd = parseI64(argv[2]);
    gstate = sd & 2147483647;

    std::vector<Node*> table(TABLE, nullptr);  // 长命表：65536 槽，全空
    int64_t sum = 0, walksum = 0, live = 0, tailv = 0;
    for (int64_t i = 0; i < n; ++i) {
        Node* head = nullptr;
        for (int64_t j = 0; j < SHORT; ++j) {
            Node* s = new Node;             // 短命节点：下一轮即垃圾
            s->k = i * SHORT + j;
            s->v = nxt() % 1000;
            s->next = head;
            head = s;
            sum = (sum + s->v) & MASK;
        }
        tailv = head->v;                    // 尾端读回（SPEC §0）：防优化器消掉分配
        if (i % EVERY == 0) {
            Node* p = new Node;             // 提升到长命表
            p->k = i;
            p->v = nxt() % 1000;
            const int64_t b = i % TABLE;
            p->next = table[b];
            table[b] = p;
            ++live;
        }
        const int64_t b2 = (i * 40503) % TABLE;
        Node* c = table[b2];
        int64_t cnt = 0;
        while (c != nullptr && cnt < WALKCAP) {
            walksum = (walksum + c->v) & MASK;
            c = c->next;
            ++cnt;
        }
    }
    std::printf("rounds=%lld promoted=%lld walksum=%lld sum=%lld tailv=%lld\n",
                (long long)n, (long long)live, (long long)walksum, (long long)sum, (long long)tailv);
    std::printf("table=65536 short=9 every=10 walkcap=8\n");
    return 0;
}

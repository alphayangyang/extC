// bench/gc/v2 形状 G3 chase —— C++ 自然写法。
// 与 src/chase.extc 同算法 / 同 LCG / 同求值顺序，stdout 必须逐字节一致。
//
// 数据结构：节点 = { k, v, o[4] }（定长出边数组）；图 = CAP 槽定长表
//           （std::vector 只按下标读写）。先建 M 个节点、再连 4M 条边，
//           然后随机游走 W 步；每 256 步新分配 4 个即抛节点。
//           全部裸 new、不 delete —— 活图 + churn 垃圾正是本形状要压的。
// 两条臂：默认臂 -DCHASE_NAIVE 与 -DCHASE_CAREFUL 源码相同（本形状无环）。
//
// 构建：g++ -O2 -std=c++17 -DCHASE_NAIVE   src/chase.cpp -o b/chase.cpp.bin
//       g++ -O2 -std=c++17 -DCHASE_CAREFUL src/chase.cpp -o b/chase.cpp_careful.bin
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int64_t MASK = 4611686018427387903LL;
constexpr int64_t CAP = 1048576;
constexpr int64_t OUTDEG = 4;
constexpr int64_t CHURN_EVERY = 256;
constexpr int64_t CHURN = 4;

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
    int64_t k;
    int64_t v;
    Node* o[OUTDEG];   // 定长出边表
};

} // namespace

int main(int argc, char** argv) {
    int64_t m = CAP;          // 必须 <= CAP
    int64_t sd = 20261013;
    int64_t w = 4000000;
    if (argc > 1) m = parseI64(argv[1]);
    if (argc > 2) sd = parseI64(argv[2]);
    if (argc > 3) w = parseI64(argv[3]);
    gstate = sd & 2147483647;

    std::vector<Node*> graph(CAP, nullptr);
    for (int64_t i = 0; i < m; ++i) {
        Node* x = new Node;
        x->k = i;
        x->v = nxt() % 1000;
        graph[i] = x;
    }
    for (int64_t i = 0; i < m; ++i) {
        Node* src = graph[i];
        for (int64_t j = 0; j < OUTDEG; ++j) {
            const int64_t t = nxt() % m;
            src->o[j] = graph[t];
        }
    }
    Node* cur = graph[0];
    int64_t sum = 0, churn = 0, tailv = 0;
    for (int64_t step = 0; step < w; ++step) {
        if (cur != nullptr) sum = (sum + cur->v) & MASK;
        const int64_t r = pick(OUTDEG);
        if (cur != nullptr) cur = cur->o[r];
        if (step % CHURN_EVERY == 0) {
            Node* h = nullptr;
            for (int64_t j = 0; j < CHURN; ++j) {
                Node* s = new Node;      // churn：即抛垃圾
                s->k = step;
                s->v = nxt() % 1000;
                s->o[0] = h;
                h = s;
            }
            tailv = h->v;                // 尾端读回（SPEC §0）
            churn += CHURN;
        }
    }
    std::printf("nodes=%lld edges=%lld walk=%lld churn=%lld sum=%lld tailv=%lld\n",
                (long long)m, (long long)(m * OUTDEG), (long long)w, (long long)churn,
                (long long)sum, (long long)tailv);
    std::printf("outdeg=4 cap=1048576 churn_every=256 churn=4\n");
    return 0;
}

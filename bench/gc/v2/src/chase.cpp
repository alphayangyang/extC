// bench/gc/v2 形状 G3 chase —— C++ 自然写法（两条臂内存纪律不同）。
// 与 src/chase.extc 同算法 / 同 LCG / 同求值顺序，stdout 必须逐字节一致。
//
// 数据结构：节点 = { k, v, o[4] }（定长出边数组）；图 = CAP 槽定长表
//           （std::vector 只按下标读写）。先建 M 个节点、再连 4M 条边，
//           然后随机游走 W 步；每 256 步新分配 4 个即抛节点 —— 活图 + churn 垃圾
//           正是本形状要压的。
//   naive  (-DCHASE_NAIVE)  ：全部裸 new、不 delete，生死交给 GC 语义。
//   careful(-DCHASE_CAREFUL)：显式释放（走 FAIR 同一条路径）。
//   fair   (-DCHASE_FAIR)   ：与 careful 同一路径的别名（run.sh 用 _CAREFUL 构建该臂，
//                              这里同时接受 _FAIR，两条宏名出同一个可执行体）。
// FAIR 臂：显式释放，让 C++ 与 extC/Rust 在同一个内存纪律下比较 —— 游走里 churn 出来的
// 节点当轮就 delete。图本身活到进程退出，不释放：那不影响峰值 RSS，只会给 C++ 臂加上
// 其它语言没有的收尾成本（本卡具只比 stdout 与峰值）。只改释放时机，不改任何输出。
//
// 构建：g++ -O2 -std=c++17 -DCHASE_NAIVE   src/chase.cpp -o b/chase.cpp.bin
//       g++ -O2 -std=c++17 -DCHASE_CAREFUL src/chase.cpp -o b/chase.cpp_careful.bin
//       g++ -O2 -std=c++17 -DCHASE_FAIR    src/chase.cpp -o b/chase.cpp_fair.bin
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

#if defined(CHASE_CAREFUL) || defined(CHASE_FAIR)
// 防消解（SPEC §0 坑 1）：-O2 会把「new 完立刻 delete」整对对消，这条臂就根本不再分配，
// 计时会虚假变快。释放前先把指针写进这个只写不读的 volatile 槽，分配因此不可省
// （同 SPEC 建议的「把最近一次的头存进全局/静态槽」）。槽本身不参与任何输出。
void* volatile g_freeSink = nullptr;
#endif

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
#if defined(CHASE_CAREFUL) || defined(CHASE_FAIR)
            // FAIR 臂：显式释放，让 C++ 与 extC/Rust 在同一个内存纪律下比较。
            // 这 4 个 churn 节点只在本轮里用一次（尾端读回），此后再无人引用，当场释放；
            // 链沿 o[0] 从新到旧串起，先存 next 再 delete。g_freeSink 让分配不可被 -O2
            // 对消（SPEC §0 坑 1），它不参与任何输出。
            while (h != nullptr) {
                Node* next = h->o[0];
                g_freeSink = h;
                delete h;
                h = next;
            }
#endif
        }
    }
    std::printf("nodes=%lld edges=%lld walk=%lld churn=%lld sum=%lld tailv=%lld\n",
                (long long)m, (long long)(m * OUTDEG), (long long)w, (long long)churn,
                (long long)sum, (long long)tailv);
    std::printf("outdeg=4 cap=1048576 churn_every=256 churn=4\n");
    return 0;
}

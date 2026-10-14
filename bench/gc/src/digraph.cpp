// bench/gc 形状 7：digraph —— 现代 C++ 两个档位（naive / careful）。
// 与 bench/gc/src/digraph.extc **逐字同算法/同 PRNG/同顺序**，输出必须逐字节一致。
//
//   naive  ：所有引用都是 std::shared_ptr（回指 back 也是）⇒ 环状垃圾收不掉。
//   careful：回指 back 用 std::weak_ptr，丢弃分量时显式断开（first/ord/back 全清）
//            ⇒ 环被打破，引用计数级联归零。careful 的额外成本 = 断开那一趟。
//
// 构建（无 new / 无 delete / 无 free）：
//   g++ -O2 -std=c++17 -DDIGRAPH_NAIVE   digraph.cpp -o digraph_cpp_naive
//   g++ -O2 -std=c++17 -DDIGRAPH_CAREFUL digraph.cpp -o digraph_cpp_careful
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int64_t MASK = 4611686018427387903LL;

struct Node;
struct Edge;
struct Handle;

#if defined(DIGRAPH_CAREFUL)
using BackRef = std::weak_ptr<Node>;   // 回指：弱引用
#else
using BackRef = std::shared_ptr<Node>; // 回指：强引用 ⇒ 环收不掉
#endif

struct Edge {
    std::shared_ptr<Node> to;
    std::shared_ptr<Edge> next;
    int64_t kind = 0;
};

struct Node {
    int64_t id = 0;
    int64_t gid = 0;
    int64_t payload = 0;
    std::shared_ptr<Edge> first;
    std::shared_ptr<Node> ord;   // 组内祖先链（新 -> 老），结构用
    BackRef back;                // 命名回指（naive 强 / careful 弱）
    int64_t mark = 0;
    int64_t cmark = 0;
    std::shared_ptr<Node> snext; // 显式栈/临时链（用完即清）
    bool gpin = false;
};

struct Handle {
    std::shared_ptr<Node> target;
    std::shared_ptr<Node> newest;
    int64_t size = 0;
    std::shared_ptr<Handle> prev;
    std::shared_ptr<Handle> next;
};

struct Ctx {
    std::shared_ptr<Handle> head;
    std::shared_ptr<Handle> tail;
    int64_t n = 0;
    std::shared_ptr<Node> hubs[8];
    int64_t hubCount = 0;
};

int64_t gstate = 1;
int64_t nextId = 0;
int64_t gidCounter = 0;
int64_t backedges = 0;
int64_t nodesM = 0;
int64_t groupsM = 0;
int64_t droppedG = 0;
int64_t droppedN = 0;
int64_t rejected = 0;
int64_t cyclesDropped = 0;
int64_t acc = 0;
int64_t xacc = 0;
int64_t lastNodes = 0;
int64_t lastEdges = 0;
int64_t epoch = 0;
int64_t cepoch = 0;

int64_t nxt() {
    gstate = (gstate * 1103515245 + 12345) & 2147483647;
    return gstate;
}

int64_t mixv(int64_t a, int64_t b) {
    int64_t x = (a * 2654435761LL + b * 40503LL + 12345LL) & MASK;
    x = x ^ (x >> 12);
    return x & MASK;
}

int64_t parseNum(const char* s, int64_t def) {
    if (s == nullptr || *s == '\0') return def;
    int64_t v = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return def;
        v = v * 10 + (*p - '0');
    }
    return v;
}

std::shared_ptr<Handle> handleAt(const Ctx& c, int64_t idx) {
    std::shared_ptr<Handle> h = c.head;
    for (int64_t i = 0; i < idx && h; ++i) h = h->next;
    return h;
}

std::shared_ptr<Node> nodeInGroup(const std::shared_ptr<Handle>& h, int64_t j) {
    std::shared_ptr<Node> x = h->newest;
    for (int64_t i = 0; i < j && x; ++i) x = x->ord;
    return x;
}

void addEdge(const std::shared_ptr<Node>& s, const std::shared_ptr<Node>& t, int64_t k) {
    auto e = std::make_shared<Edge>();
    e->to = t;
    e->kind = k;
    e->next = s->first;
    s->first = e;
}

// 组内"更老"（id 更小）的一个随机祖先；没有就退回 hub
std::shared_ptr<Node> olderInGroup(const std::shared_ptr<Node>& s,
                                   const std::shared_ptr<Node>& hub) {
    int64_t c1 = 0;
    for (auto y = s->ord; y; y = y->ord) ++c1;
    if (c1 == 0) return hub;
    int64_t j = nxt() % c1;
    auto z = s->ord;
    for (int64_t i2 = 0; z; ++i2, z = z->ord) {
        if (i2 == j) return z;
    }
    return hub;
}

// 组内"更新"（id 更大）的一个随机后代；没有就退回 hub
std::shared_ptr<Node> newerInGroup(const std::shared_ptr<Node>& s,
                                   const std::shared_ptr<Node>& newest,
                                   const std::shared_ptr<Node>& hub) {
    int64_t c2 = 0;
    for (auto y = newest; y; y = y->ord) {
        if (y->id > s->id) ++c2;
    }
    if (c2 == 0) return hub;
    int64_t j = nxt() % c2;
    auto z = newest;
    int64_t i2 = 0;
    for (; z; z = z->ord) {
        if (z->id > s->id) {
            if (i2 == j) return z;
            ++i2;
        }
    }
    return hub;
}

// 建组时的弦边目标：60% 组内更老 / 20% 组内更新 / 10% 组内 hub / 10% 组内长程随机
std::shared_ptr<Node> pickChordTarget(const std::shared_ptr<Node>& s,
                                      const std::shared_ptr<Node>& hub,
                                      const std::shared_ptr<Node>& newest, int64_t size) {
    int64_t r = nxt() % 100;
    if (r < 60) return olderInGroup(s, hub);
    if (r < 80) return newerInGroup(s, newest, hub);
    if (r < 90) return hub;
    int64_t j3 = nxt() % size;
    auto z3 = newest;
    for (int64_t i4 = 0; z3; ++i4, z3 = z3->ord) {
        if (i4 == j3) return z3;
    }
    return hub;
}

// 组内是否存在有向环（丢弃时现场验证；只用组内边，独立于"构造上必有环"）
bool groupHasCycle(const std::shared_ptr<Node>& newest, int64_t gid) {
    for (auto u = newest; u; u = u->ord) {
        ++cepoch;
        const int64_t ep = cepoch;
        std::shared_ptr<Node> top;
        for (auto e = u->first; e; e = e->next) {
            auto t = e->to;
            if (t->gid == gid) {
                if (t->id == u->id) return true;
                if (t->cmark != ep) {
                    t->cmark = ep;
                    t->snext = top;
                    top = t;
                }
            }
        }
        bool found = false;
        while (top) {
            auto x = top;
            top = x->snext;
            x->snext.reset();
            if (x->id == u->id) found = true;
            if (!found) {
                for (auto e2 = x->first; e2; e2 = e2->next) {
                    auto t2 = e2->to;
                    if (t2->gid == gid && t2->cmark != ep) {
                        t2->cmark = ep;
                        t2->snext = top;
                        top = t2;
                    }
                }
            }
        }
        if (found) return true;
    }
    return false;
}

#if defined(DIGRAPH_CAREFUL)
// careful 档的"工程费"：摘组前把该组显式断开（清 first / ord / back）。
// 用 std::vector 收集强引用，防止清链过程中结点被释放。
void breakGroup(const std::shared_ptr<Node>& newest) {
    std::vector<std::shared_ptr<Node>> list;
    for (auto x = newest; x; x = x->ord) list.push_back(x);
    for (auto& x : list) {
        x->first.reset();
        x->ord.reset();
        x->back.reset();
    }
}
int64_t breakNs = 0;
int64_t breakCalls = 0;
bool breakStats = false;
// 逐次断开的时长分布（门控 DIGRAPH_BREAK_STATS=1；只写 stderr）。
// 既有 `break_ns=/break_calls=` 两行原样保留（DIGRAPH.md §6.2 读了它），分布**追加**一行。
constexpr int64_t kBreakRing = 262144;  // 覆盖本形状最大样本数（1M 档 219025 次）⇒ 不截断
int64_t breakRing[kBreakRing];
int64_t breakRingN = 0;
int64_t breakRingOver = 0;
int64_t breakSumNs = 0;
void breakRecord(int64_t ns) {
    breakSumNs += ns;
    if (breakRingN < kBreakRing) breakRing[breakRingN++] = ns;
    else ++breakRingOver;
}
void breakDumpDist() {
    if (!breakStats) return;
    int64_t n = breakRingN;
    int64_t b100 = 0, b200 = 0, b500 = 0, b1us = 0, b4us = 0, b16us = 0, bge = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t d = breakRing[i];
        if (d < 100) ++b100;
        if (d < 200) ++b200;
        if (d < 500) ++b500;
        if (d < 1000) ++b1us;
        if (d < 4000) ++b4us;
        if (d < 16000) ++b16us;
        if (d >= 16000) ++bge;
    }
    std::sort(breakRing, breakRing + n);
    auto pct = [&](int p) -> double {
        if (n == 0) return 0.0;
        return (double)breakRing[(n - 1) * p / 100];
    };
    std::fprintf(stderr,
                 "BREAK_DIST source=cpp n=%lld overflow=%lld p50_ns=%.0f p90_ns=%.0f p99_ns=%.0f "
                 "max_ns=%.0f total_ms=%.3f b_lt100ns=%lld b_lt200ns=%lld b_lt500ns=%lld b_lt1us=%lld "
                 "b_lt4us=%lld b_lt16us=%lld b_ge16us=%lld\n",
                 (long long)n, (long long)breakRingOver, pct(50), pct(90), pct(99),
                 n ? (double)breakRing[n - 1] : 0.0, (double)breakSumNs / 1e6, (long long)b100,
                 (long long)b200, (long long)b500, (long long)b1us, (long long)b4us,
                 (long long)b16us, (long long)bge);
}
#endif

void createGroup(Ctx& c, int64_t k, bool pin, bool isHub) {
    const int64_t gid = gidCounter++;
    ++groupsM;

    auto hub = std::make_shared<Node>();
    hub->id = nextId;
    hub->gid = gid;
    hub->payload = nxt() % 1000;
    hub->gpin = pin;
    ++nextId;
    ++nodesM;
    if (isHub) {
        c.hubs[0] = hub;
        c.hubCount = 1;
    }

    auto prev = hub;
    for (int64_t cnt = 1; cnt < k; ++cnt) {
        auto n = std::make_shared<Node>();
        n->id = nextId;
        n->gid = gid;
        n->payload = nxt() % 1000;
        n->ord = prev;
        n->back = prev;
        n->gpin = pin;
        ++nextId;
        ++nodesM;
        if (isHub) {
            c.hubs[cnt] = n;
            c.hubCount = cnt + 1;
        }
        addEdge(prev, n, 1);
        prev = n;
    }

    addEdge(prev, hub, 1);
    ++backedges;

    for (auto x = prev; x; x = x->ord) {
        int64_t d = nxt() % 3;
        for (int64_t j = 0; j < d; ++j) {
            auto tgt = pickChordTarget(x, hub, prev, k);
            addEdge(x, tgt, 2);
        }
    }

    auto h = std::make_shared<Handle>();
    h->target = hub;
    h->newest = prev;
    h->size = k;
    h->prev = c.tail;
    if (c.tail) c.tail->next = h;
    else c.head = h;
    c.tail = h;
    ++c.n;
}

// 丢一个组：(c) 步。优先摘无跨组入边（gpin==false）的组；顺手做现场环验证与计数。
void tryDrop(Ctx& c) {
    if (c.n == 0) return;
    const int64_t base = nxt() % c.n;
    for (int64_t probe = 0; probe < 8; ++probe) {
        int64_t idx = base + probe;
        while (idx >= c.n) idx -= c.n;
        auto h = handleAt(c, idx);
        int64_t cnt = 0;
        bool anypin = false;
        for (auto x = h->newest; x; x = x->ord) {
            ++cnt;
            if (x->gpin) anypin = true;
        }
        if (anypin) {
            ++rejected;
            continue;
        }
        ++droppedG;
        droppedN += cnt;
        if (groupHasCycle(h->newest, h->target->gid)) ++cyclesDropped;
#if defined(DIGRAPH_CAREFUL)
        // 显式断开本组 ⇒ 环被打破，引用计数级联归零。
        // DIGRAPH_BREAK_STATS=1 时把这一趟的时间累计起来（只写 stderr，stdout 不变）。
        if (breakStats) {
            auto bt0 = std::chrono::steady_clock::now();
            breakGroup(h->newest);
            auto bt1 = std::chrono::steady_clock::now();
            const int64_t d = std::chrono::duration_cast<std::chrono::nanoseconds>(bt1 - bt0).count();
            breakNs += d;
            breakRecord(d);
            ++breakCalls;
        } else {
            breakGroup(h->newest);
        }
#endif
#if defined(DIGRAPH_ASSERT_REFS)
        // 诊断档（`-DDIGRAPH_ASSERT_REFS`）专用的不变量断言：断开后本组结点只应由
        // handle（target/newest）与本函数的两个局部 shared_ptr 持有 —— `gpin` 不变量的实测版。
        {
            const long tc = h->target.use_count();
            const long nc = h->newest.use_count();
            const long exp = (h->target == h->newest) ? 2 : 1;  // handle 的两个槽 + 无局部副本
            if (tc != exp || nc != exp) {
                std::fprintf(stderr, "ASSERT-REFS FAIL gid=%lld target_count=%ld newest_count=%ld expect=%ld\n",
                             (long long)h->target->gid, tc, nc, exp);
                std::abort();
            }
        }
#endif
        auto hp = h->prev;
        auto hn = h->next;
        if (hp) hp->next = hn;
        else c.head = hn;
        if (hn) hn->prev = hp;
        else c.tail = hp;
        h->prev.reset();
        h->next.reset();
        --c.n;
        return;
    }
}

// (d) 步：从 handle 表出发对存活部分做一次遍历（显式栈，用 snext 串），累加校验和。
void traverse(Ctx& c, int64_t ep) {
    std::shared_ptr<Node> top;
    for (auto h = c.head; h; h = h->next) {
        auto t = h->target;
        if (t->mark != ep) {
            t->mark = ep;
            t->snext = top;
            top = t;
        }
    }
    int64_t rn = 0;
    int64_t re = 0;
    int64_t radd = 0;
    int64_t rxor = 0;
    while (top) {
        auto n = top;
        top = n->snext;
        n->snext.reset();
        ++rn;
        radd = (radd + mixv(n->id, n->payload)) & MASK;
        for (auto e = n->first; e; e = e->next) {
            ++re;
            auto t2 = e->to;
            rxor = rxor ^ mixv(n->id, t2->id) ^ (e->kind * 4099);
            if (t2->mark != ep) {
                t2->mark = ep;
                t2->snext = top;
                top = t2;
            }
        }
    }
    acc = (acc + radd + rn * 1000003 + re * 31) & MASK;
    xacc = (xacc ^ (rxor ^ (rn * 1000003 + re))) & MASK;
    lastNodes = rn;
    lastEdges = re;
}

} // namespace

int main(int argc, char** argv) {
    int64_t N = 200000;
    int64_t sd = 20261013;
    if (argc > 1) N = parseNum(argv[1], N);
    if (argc > 2) sd = parseNum(argv[2], sd);
    if (N < 256) N = 256;
    gstate = sd & 2147483647;
#if defined(DIGRAPH_CAREFUL)
    breakStats = (std::getenv("DIGRAPH_BREAK_STATS") != nullptr);
#endif

    Ctx c;
    createGroup(c, 8, true, true);

    const int64_t R = 16 + N / 4096;
    int64_t B = N / R;
    if (B < 24) B = 24;

    for (int64_t round = 0; round < R; ++round) {
        // (a) 建
        int64_t created = 0;
        int64_t groupsThisRound = 0;
        while (created < B) {
            int64_t k = 3 + nxt() % 4;
            createGroup(c, k, false, false);
            created += k;
            ++groupsThisRound;
        }
        // (b) 改：删头边 + 按 60/20/10/10 加边
        const int64_t M = 2 + nxt() % 3;
        for (int64_t m = 0; m < M; ++m) {
            if (c.n > 0) {
                int64_t idx = nxt() % c.n;
                auto h = handleAt(c, idx);
                auto s = nodeInGroup(h, nxt() % h->size);
                auto f = s->first;
                if (f && f->next) s->first = f->next;
                int64_t r = nxt() % 100;
                std::shared_ptr<Node> tgt;
                if (r < 60) {
                    tgt = olderInGroup(s, h->target);
                } else if (r < 80) {
                    tgt = newerInGroup(s, h->newest, h->target);
                } else if (r < 90) {
                    if (c.hubCount > 0) {
                        int64_t hj = nxt() % c.hubCount;
                        tgt = c.hubs[hj];
                    }
                } else {
                    int64_t idx2 = nxt() % c.n;
                    auto h2 = handleAt(c, idx2);
                    tgt = nodeInGroup(h2, nxt() % h2->size);
                }
                if (tgt) {
                    if (tgt->gid != s->gid) tgt->gpin = true;
                    addEdge(s, tgt, 3);
                }
            }
        }
        // (c) 丢
        for (int64_t d = 0; d < groupsThisRound; ++d) tryDrop(c);
        // (d) 遍历
        ++epoch;
        traverse(c, epoch);
    }

    const int64_t total = (acc + xacc) & MASK;
    std::printf("nodes=%lld edges=%lld sum=%lld\n", (long long)lastNodes,
                (long long)lastEdges, (long long)total);
    std::printf("backedges=%lld groups_made=%lld nodes_made=%lld dropped_groups=%lld "
                "dropped_nodes=%lld rejected_external=%lld cycles_dropped=%lld\n",
                (long long)backedges, (long long)groupsM, (long long)nodesM,
                (long long)droppedG, (long long)droppedN, (long long)rejected,
                (long long)cyclesDropped);
#if defined(DIGRAPH_CAREFUL)
    if (breakStats) {
        std::fprintf(stderr, "break_ns=%lld break_calls=%lld\n", (long long)breakNs,
                     (long long)breakCalls);
        breakDumpDist();
    }
#endif
    return 0;
}

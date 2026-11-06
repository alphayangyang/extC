// 同一份功能，用**标准 C++ RAII**：std::shared_ptr 共享所有权 + 容器管理生命周期。
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <deque>
#include <chrono>
constexpr int BUCKETS = 64, BUCKET_MAX = 8, CACHE = 1024, INDEXMAX = 200000;
struct Node { long key = 0, val = 0; std::shared_ptr<Node> next; };
int main(int argc, char** argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    std::vector<std::deque<std::shared_ptr<Node>>> buckets(BUCKETS);
    std::vector<std::shared_ptr<Node>> cache(CACHE);
    std::vector<std::shared_ptr<Node>> idx; idx.reserve(INDEXMAX + 1);
    long sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < n; i++) {
        int b = int(i % BUCKETS);
        sink += i + (i+1) + (i+2);                      // 解析：栈上的算术，不分配
        auto nd = std::make_shared<Node>();             // RAII：控制块与对象一次分配
        nd->key = i; nd->val = i*3;
        nd->next = buckets[b].empty() ? nullptr : buckets[b].front();
        buckets[b].push_front(nd);
        cache[i % CACHE] = nd;
        if (i < INDEXMAX) idx.push_back(nd);
        for (auto q = buckets[b].begin(); q != buckets[b].end(); ++q)
            if ((*q)->key == i-7) { (*q)->val++; break; }
        if (int(buckets[b].size()) > BUCKET_MAX) buckets[b].pop_back();   // 最后一个持有者消失即释放
    }
    auto t1 = std::chrono::steady_clock::now();
    printf("cpp      耗时=%.1fms sink=%ld idx=%zu\n",
           std::chrono::duration<double, std::milli>(t1-t0).count(), sink, idx.size());
    return 0;
}

/* 静态区间第 k 小（主席树 / 可持久化线段树，P3834）—— **C++ 实现**
 *
 * 跟 `p3834.c` / `p3834.rs` / `p3834.extc` **同一套算法**（逐行对应）。
 *
 * 写法上刻意**用地道的 C++**（主人要求"尽量不用 C 风格的东西"）：
 *   · 池子是 `std::vector<Node>`（不是 `Node pool[…]` + `realloc`）
 *   · 数组是 `std::vector<int32_t>`，大小写在构造参数里（不是宏 + 定长数组）
 *   · 没有宏、没有 `typedef struct`、没有手动 `malloc/free`
 *   · 输出用 `std::cout`（不是 `printf` + 格式串）
 *
 * ⭐ **用足 C++ 的容器**（主人要求）：池子就是 `std::vector<Node>` —— `reserve` 一次到位、
 *    `push_back` 长节点，**不写 malloc / free / memset** ✓
 *    （`reserve` 只借内存**不清零** ⇒ 跟 C 的 `malloc` 一样；那 252MB 是按需触页的）
 * ⚠️ `-DGROW`：池子**不预留**，靠 `push_back` 自己长 ⇒ 对照组（"不估大小"的代价）✓
 */
#include <cstdint>
#include <iostream>
#include <vector>

#ifndef N
#define N 1000000
#endif
#ifndef Q
#define Q 1000000
#endif
#ifndef V
#define V 1000000
#endif
#ifndef DEPTH
#define DEPTH 21
#endif

namespace {

struct Node {
    std::int32_t l = 0, r = 0, sum = 0;
};

constexpr std::int64_t kPoolSize = static_cast<std::int64_t>(N) * DEPTH + 2;

class Pcg32 {
public:
    Pcg32(std::uint64_t seed, std::uint64_t stream) : state_(0), inc_((stream << 1) | 1) {
        next();
        state_ += seed;
        next();
    }
    std::uint64_t next() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const std::uint64_t x = (((old >> 18) ^ old) >> 27) & 0xFFFFFFFFULL;
        const std::uint64_t rot = old >> 59;
        return ((x >> rot) | (x << ((32 - rot) & 31))) & 0xFFFFFFFFULL;
    }
    std::uint64_t bounded(std::uint64_t bound) {
        if (bound == 0) return 0;
        const std::uint64_t threshold = (0ULL - bound) % bound;
        std::uint64_t x = next();
        while (x < threshold) x = next();
        return x % bound;
    }

private:
    std::uint64_t state_, inc_;
};

class PersistentTree {
public:
    PersistentTree() {
#ifdef GROW
        pool_.reserve(64);              /* 对照组：不预估，让 vector 自己长 */
#else
        pool_.reserve(static_cast<std::size_t>(kPoolSize));   /* 正常写法：一次到位（**不**清零）*/
#endif
        pool_.push_back(Node{});        /* 0 号空节点（全零）—— 两种模式都要 ✓ */
    }

    /* 建版本：`prev` 那条路径整条复制，其余指针共享 */
    std::int32_t update(std::int32_t prev, std::int32_t lo, std::int32_t hi, std::int32_t pos) {
        /* ⚠️ 先把旧节点拷成一份**局部值**再 push ✓
         * `pool_.push_back(pool_[prev])` = "把容器自己元素的引用交给 push_back" ——
         * 标准上不保证安全（扩容可能把那块内存换掉）✗。实测本平台（libstdc++ 15）
         * 两种写法答案**一样**（先构造再释放旧块 ✓）⇒ 这是**防御性**写法，
         * 不是"修了一个实测出来的错" ✓ */
        const Node copy = pool_[static_cast<std::size_t>(prev)];
        const std::int32_t rt = static_cast<std::int32_t>(pool_.size());
        pool_.push_back(copy);
        pool_[static_cast<std::size_t>(rt)].sum += 1;
        if (lo < hi) {
            const std::int32_t mid = lo + (hi - lo) / 2;
            if (pos <= mid) pool_[rt].l = update(pool_[prev].l, lo, mid, pos);
            else            pool_[rt].r = update(pool_[prev].r, mid + 1, hi, pos);
        }
        return rt;
    }

    std::int32_t query(std::int32_t u, std::int32_t v, std::int32_t lo, std::int32_t hi,
                       std::int32_t k) const {
        if (lo == hi) return lo;
        const std::int32_t mid = lo + (hi - lo) / 2;
        const std::int32_t leftCount =
            pool_[pool_[v].l].sum - pool_[pool_[u].l].sum;
        if (k <= leftCount) return query(pool_[u].l, pool_[v].l, lo, mid, k);
        return query(pool_[u].r, pool_[v].r, mid + 1, hi, k - leftCount);
    }

    std::int32_t count() const { return static_cast<std::int32_t>(pool_.size()); }

private:
    std::vector<Node> pool_;
};

}  // namespace

int main() {
    Pcg32 rng(20260922ULL, 54);

    std::vector<std::int32_t> a(static_cast<std::size_t>(N) + 1);
    for (std::int64_t i = 1; i <= N; ++i) a[i] = static_cast<std::int32_t>(rng.bounded(V)) + 1;

    std::uint64_t inSum = 0;
    for (std::int64_t i = 1; i <= N; ++i) inSum = inSum * 1000003ULL + static_cast<std::uint64_t>(a[i]);

    PersistentTree tree;
    std::vector<std::int32_t> root(static_cast<std::size_t>(N) + 1);
    for (std::int64_t i = 1; i <= N; ++i)
        root[i] = tree.update(root[i - 1], 1, V, a[i]);

    std::uint64_t ansSum = 0;
    for (std::int64_t j = 0; j < Q; ++j) {
        const std::int64_t l = 1 + static_cast<std::int64_t>(rng.bounded(N));
        const std::int64_t r = l + static_cast<std::int64_t>(rng.bounded(N - l + 1));
        const std::int64_t k = 1 + static_cast<std::int64_t>(rng.bounded(r - l + 1));
        const std::int32_t ans = tree.query(root[l - 1], root[r], 1, V, static_cast<std::int32_t>(k));
        ansSum = ansSum * 1000009ULL + static_cast<std::uint64_t>(ans);
    }

    std::cout << "in    = " << inSum << "\n";
    std::cout << "n     = " << N << "\n";
    std::cout << "q     = " << Q << "\n";
    std::cout << "nodes = " << tree.count() << "\n";
    std::cout << "ans   = " << ansSum << "\n";
    return 0;
}

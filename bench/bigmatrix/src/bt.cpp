/* Shape 3: bt -- C++ port of src/bt.c.
 *
 * Read `maxdepth bigdepth iters`; build perfect binary trees bottom-up,
 * count nodes; then count one big tree `iters` times.  Same as bench/heavy/bt.c.
 * The trees are owned by std::unique_ptr, so each round's tree is torn down by
 * its destructor where the C reference called freeTree(). */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

/* Fast stdin: 1 MiB fread buffer + hand-written integer parsing, mirroring
 * src/fastio.h, so every shape reads stdin the same way. */
class StdinReader {
public:
    /* Signed parse (fastio.h bm_i64): skips ' ', '\n', '\t', '\r'. */
    std::int64_t readI64() noexcept {
        int c = get();
        bool neg = false;
        std::int64_t v = 0;
        while (c == ' ' || c == '\n' || c == '\t' || c == '\r') c = get();
        if (c == '-') {
            neg = true;
            c = get();
        }
        while (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            c = get();
        }
        return neg ? -v : v;
    }

    /* Unsigned parse (fastio.h bm_u64): skips anything that is not a digit and
     * yields 0 at end of input. */
    std::uint64_t readU64() noexcept {
        int c = get();
        std::uint64_t v = 0;
        while (c < '0' || c > '9') {
            if (c < 0) return 0;
            c = get();
        }
        while (c >= '0' && c <= '9') {
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
            c = get();
        }
        return v;
    }

private:
    static constexpr std::size_t kBufSize = 1u << 20;

    /* Returns the next byte, or -1 at end of input. */
    int get() noexcept {
        if (pos_ == len_) {
            len_ = std::fread(buf_.get(), 1, kBufSize, stdin);
            pos_ = 0;
            if (len_ == 0) return -1;
        }
        return buf_[pos_++];
    }

    /* new[] rather than std::make_unique<unsigned char[]>: make_unique
     * value-initialises, i.e. memsets all 1 MiB that fread overwrites anyway. */
    std::unique_ptr<unsigned char[]> buf_{new unsigned char[kBufSize]};
    std::size_t pos_ = 0;
    std::size_t len_ = 0;
};

struct Tree {
    std::unique_ptr<Tree> left;
    std::unique_ptr<Tree> right;
};

using TreePtr = std::unique_ptr<Tree>;

/* Perfect tree built bottom-up: a depth-0 node is a leaf with null children. */
TreePtr bottomUp(std::int64_t depth) {
    auto t = std::make_unique<Tree>(); /* value-init: children start out null */
    if (depth > 0) {
        t->left = bottomUp(depth - 1);
        t->right = bottomUp(depth - 1);
    }
    return t;
}

/* Perfect tree, so a null left child means a leaf -- exactly the C check(). */
std::int64_t countNodes(const Tree *t) {
    if (!t->left) return 1;
    return 1 + countNodes(t->left.get()) + countNodes(t->right.get());
}

}  // namespace

int main() {
    StdinReader in;
    const std::int64_t maxdepth = in.readI64();
    const std::int64_t bigdepth = in.readI64();
    const std::int64_t iters = in.readI64();

    std::int64_t total = 0;
    for (std::int64_t d = 4; d <= maxdepth; ++d) {
        const TreePtr t = bottomUp(d);
        total += countNodes(t.get());
    } /* t goes out of scope here and frees the tree, like the C freeTree() */

    TreePtr big = bottomUp(bigdepth);
    for (std::int64_t i = 0; i < iters; ++i) total += countNodes(big.get());

    /* The C reference never frees this one.  Releasing it keeps the C++ run
     * doing the same work instead of paying for a teardown the C side skips. */
    [[maybe_unused]] Tree *const leaked = big.release();

    std::printf("trees=%lld\n", static_cast<long long>(total));
    return 0;
}

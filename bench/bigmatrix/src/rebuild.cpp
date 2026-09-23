/* Shape 5: rebuild -- C++ port of src/rebuild.c (one allocator round-trip per
 * node).
 *
 * Read `rounds k`; each round builds a k-node chain, sums it, then frees all of
 * it.  The allocator must stay opaque to the optimizer: gcc proves a plain
 * new/delete (or malloc/free) pair away otherwise, and then the shape measures
 * nothing.  Hence the volatile function pointers -- the same defence the C
 * reference uses, and the reason the chain is owned by an explicit RAII handle
 * rather than by a per-node unique_ptr (which would also work, but only by
 * value-initialising each node's link first, i.e. by doing work the C side
 * does not do). */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

namespace {

/* Fast stdin: 1 MiB fread buffer + hand-written integer parsing, mirroring
 * src/fastio.h (the C references all read stdin this way). */
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

struct Node {
    std::int64_t v;
    Node *next;
};

/* Allocation and release go through volatile pointers, so the compiler may not
 * assume anything about them: neither the per-node allocation nor its matching
 * release can be optimised away. */
void *(*volatile v_alloc)(std::size_t) = [](std::size_t n) -> void * { return ::operator new(n); };
void (*volatile v_free)(void *) noexcept = [](void *p) noexcept { ::operator delete(p); };

/* RAII ownership of a whole chain.  The deleter releases node by node, in the
 * same head-first order as the C reference's free loop, so exactly one
 * allocator round-trip per node happens either way. */
struct ChainDeleter {
    void operator()(Node *head) const noexcept {
        while (head) {
            Node *const next = head->next;
            v_free(head);
            head = next;
        }
    }
};
using ChainPtr = std::unique_ptr<Node, ChainDeleter>;

}  // namespace

int main() {
    StdinReader in;
    const std::int64_t rounds = in.readI64();
    const std::int64_t k = in.readI64();

    std::int64_t acc = 0;
    for (std::int64_t r = 0; r < rounds; ++r) {
        ChainPtr head; /* the round's chain lives exactly one iteration */
        for (std::int64_t i = 0; i < k; ++i) {
            auto *const raw = static_cast<Node *>(v_alloc(sizeof(Node)));
            raw->v = i;
            raw->next = head.release(); /* hand the chain over before re-owning */
            head.reset(raw);
        }
        for (const Node *p = head.get(); p; p = p->next) acc += p->v;
    } /* head's deleter releases the whole chain, like the C free loop */

    std::printf("acc=%lld\n", static_cast<long long>(acc));
    return 0;
}

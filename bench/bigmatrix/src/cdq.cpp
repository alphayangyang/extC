/* Shape 2: cdq -- C++ port of src/cdq.c (3D dominance, CDQ + weight BIT).
 *
 * Read n, then n triples (a b c) with values in [1,10^6].
 *   1. sort by (a,b,c)   2. collapse duplicates, keeping cnt
 *   3. CDQ over b, BIT over c: ans[j] += cnt[i] for every i<j that dominates j
 *      The BIT is indexed by c itself: the generator emits c >= 1, so c is a
 *      valid 1-based index and no +1 shift is needed.  The array is V+1 wide
 *      (indices 0..V), and bitAdd's loop guard `i <= kV` keeps every write
 *      in range even for c = 10^6, which lands on index V exactly.
 *   4. f = ans + cnt (itself), reported answer = f - 1
 *   print sum = SUM cnt_i * ans_i (u64 wrap) and max = max ans_i
 * Verified against O(n^2) brute force by check_brute.c on the small input. */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

namespace {

constexpr std::int32_t kV = 1000000; /* coordinate ceiling */

/* Fast stdin: 1 MiB fread buffer + hand-written integer parsing, mirroring
 * src/fastio.h.  scanf / formatted std::cin extraction over 6*10^6 integers
 * would cost more than the sort itself, so it is not an option. */
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

struct Pt {
    std::int32_t a, b, c, cnt, ans;
};

/* Weight BIT over c plus the CDQ recursion over b.  `tmp` is the same
 * full-size merge scratch buffer the C reference allocates up front, and the
 * deduplicated points are owned here (moved in, not copied). */
class Cdq {
public:
    explicit Cdq(std::vector<Pt> pts)
        : p_(std::move(pts)), tmp_(p_.size()), bit_(static_cast<std::size_t>(kV) + 1, 0) {}

    void solve() {
        if (p_.size() > 1) cdq(p_);
    }

    const std::vector<Pt> &points() const noexcept { return p_; }

private:
    void bitAdd(std::int64_t i, std::int32_t v) {
        /* i is a c coordinate in [1, kV], so every bit_[i] written here is a
         * valid 1-based index into a kV+1 element array. */
        for (; i <= kV; i += i & (-i)) bit_[static_cast<std::size_t>(i)] += v;
    }

    std::int32_t bitSum(std::int64_t i) const {
        std::int32_t s = 0;
        for (; i > 0; i -= i & (-i)) s += bit_[static_cast<std::size_t>(i)];
        return s;
    }

    /* v is the current [l..r] window; the left half is [0, size/2) rounded up,
     * exactly the C split mid = l + (r - l) / 2. */
    void cdq(std::span<Pt> v) {
        if (v.size() < 2) return;
        const std::size_t mid = (v.size() + 1) / 2;
        cdq(v.first(mid));
        cdq(v.subspan(mid));

        /* Cross step: v[0..mid) is sorted by b after its recursive call, so the
         * cursor i walks forward monotonically; a point with b <= b_j and c <=
         * c_j contributes cnt to j. */
        std::size_t i = 0;
        for (std::size_t j = mid; j < v.size(); ++j) {
            while (i < mid && v[i].b <= v[j].b) {
                bitAdd(v[i].c, v[i].cnt);
                ++i;
            }
            v[j].ans += bitSum(v[j].c);
        }
        for (std::size_t t = 0; t < i; ++t) bitAdd(v[t].c, -v[t].cnt); /* undo */

        /* Stable merge by b (left half wins ties, like the C reference's
         * `p[i].b <= p[j].b`), through this level's slice of the scratch. */
        std::merge(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid),
                   v.begin() + static_cast<std::ptrdiff_t>(mid), v.end(), tmp_.begin(),
                   [](const Pt &x, const Pt &y) { return x.b < y.b; });
        std::copy_n(tmp_.begin(), v.size(), v.begin());
    }

    std::vector<Pt> p_;
    std::vector<Pt> tmp_;
    std::vector<std::int32_t> bit_;
};

bool sameKey(const Pt &x, const Pt &y) { return x.a == y.a && x.b == y.b && x.c == y.c; }

}  // namespace

int main() {
    StdinReader in;
    const auto n = static_cast<std::size_t>(in.readI64());

    std::vector<Pt> raw(n);
    for (Pt &q : raw) {
        q.a = static_cast<std::int32_t>(in.readU64());
        q.b = static_cast<std::int32_t>(in.readU64());
        q.c = static_cast<std::int32_t>(in.readU64());
        q.cnt = 1;
        q.ans = 0;
    }

    std::sort(raw.begin(), raw.end(), [](const Pt &x, const Pt &y) {
        return std::tuple{x.a, x.b, x.c} < std::tuple{y.a, y.b, y.c};
    });

    /* Collapse equal triples into one vertex weighted by its multiplicity.
     * Reserve(n) mirrors the C reference, which sizes p[] by n as well. */
    std::vector<Pt> pts;
    pts.reserve(raw.size());
    for (auto it = raw.begin(); it != raw.end();) {
        const auto runEnd = std::find_if(it, raw.end(), [&](const Pt &q) { return !sameKey(q, *it); });
        Pt vertex = *it;
        vertex.cnt = static_cast<std::int32_t>(runEnd - it);
        vertex.ans = 0;
        pts.push_back(vertex);
        it = runEnd;
    }

    Cdq solver(std::move(pts));
    solver.solve();

    std::uint64_t sum = 0;
    std::int64_t mx = 0;
    for (const Pt &q : solver.points()) {
        const std::int64_t ans = static_cast<std::int64_t>(q.ans) + static_cast<std::int64_t>(q.cnt) - 1;
        sum += static_cast<std::uint64_t>(q.cnt) * static_cast<std::uint64_t>(ans);
        if (ans > mx) mx = ans;
    }
    std::printf("sum=%llu max=%lld\n", static_cast<unsigned long long>(sum),
                static_cast<long long>(mx));
    return 0;
}

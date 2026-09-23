/* Shape 1: radix -- C++ port of src/radix.c.
 *
 * Read n and n u32 values from stdin, LSD radix sort (4 passes x 8 bits),
 * print sorted/first/last.  Same shape as bench/heavy/rs.c, data from stdin.
 *
 * Still a radix sort on purpose: swapping in std::sort would measure a
 * different shape.  What is idiomatic here is only the storage -- std::vector
 * with RAII instead of malloc/free -- while the 4 counting passes and the
 * per-pass write-back stay exactly the passes the C reference runs. */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

/* Fast stdin: 1 MiB fread buffer + hand-written integer parsing, mirroring
 * src/fastio.h.  scanf / formatted std::cin extraction would cost far more
 * than the sort itself on a 10^7-number input, so it is not an option. */
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

}  // namespace

int main() {
    StdinReader in;
    const std::int64_t n = in.readI64();
    const auto count = static_cast<std::size_t>(n);

    std::vector<std::uint32_t> a(count);
    std::vector<std::uint32_t> b(count);
    for (std::uint32_t &v : a) v = static_cast<std::uint32_t>(in.readU64());

    for (unsigned pass = 0; pass < 4; ++pass) {
        const unsigned shift = pass * 8u;

        std::array<std::int64_t, 256> cnt{};
        for (const std::uint32_t v : a) ++cnt[(v >> shift) & 255u];

        std::int64_t sum = 0;
        for (std::int64_t &c : cnt) {
            const std::int64_t bucket = c;
            c = sum;
            sum += bucket;
        }

        for (const std::uint32_t v : a)
            b[static_cast<std::size_t>(cnt[(v >> shift) & 255u]++)] = v;

        /* The C reference copies the whole scratch array back into `a` every
         * pass; std::copy is that same O(n) pass, lowered to memcpy. */
        std::copy(b.begin(), b.end(), a.begin());
    }

    const bool sorted = std::is_sorted(a.begin(), a.end());
    std::printf("sorted=%d first=%u last=%u\n", sorted ? 1 : 0,
                static_cast<unsigned>(a.front()), static_cast<unsigned>(a.back()));
    return 0;
}

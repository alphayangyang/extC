/* Shape 4: mandel -- C++ port of src/mandel.c.
 *
 * Read `w h maxiter`; same loop as bench/heavy/mb.c (double, escape at |z|>2).
 * The arithmetic is deliberately written with the reference's exact types and
 * operation order: the escape test sits on a knife edge, so reassociating or
 * promoting anything here would change the iteration counts. */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

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

}  // namespace

int main() {
    StdinReader in;
    const std::int64_t w = in.readI64();
    const std::int64_t h = in.readI64();
    const std::int64_t maxiter = in.readI64();

    std::vector<std::uint8_t> img(static_cast<std::size_t>(w * h));
    std::int64_t acc = 0;

    for (std::int64_t y = 0; y < h; ++y) {
        for (std::int64_t x = 0; x < w; ++x) {
            const double cr = static_cast<double>(x) / static_cast<double>(w) * 3.5 - 2.5;
            const double ci = static_cast<double>(y) / static_cast<double>(h) * 2.0 - 1.0;
            double zr = 0.0, zi = 0.0;
            std::int64_t it = 0;
            while (it < maxiter) {
                const double t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                if (zr * zr + zi * zi > 4.0) break;
                ++it;
            }
            img[static_cast<std::size_t>(y * w + x)] = static_cast<std::uint8_t>(it & 255);
            acc += it;
        }
    }

    std::printf("mandel=%lld img0=%d\n", static_cast<long long>(acc),
                static_cast<int>(img[0]));
    return 0;
}

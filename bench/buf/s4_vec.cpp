// 形状 S4 的对照：std::vector<rec>，元素 24 字节。
#include <cstdio>
#include <vector>

struct rec { long long a, b, c; };

int main() {
    std::vector<rec> v;
    long long moves = 0, cap = 0;
    for (long long i = 0; i < 2000000; ++i) {
        v.push_back(rec{i, i * 3, -i});
        if (v.capacity() != cap) { cap = (long long)v.capacity(); ++moves; }
    }
    long long s = 0, bad = 0;
    for (const rec &r : v) { if (r.b != r.a * 3) ++bad; s += r.a; }
    std::printf("n=%lld sum=%lld bad=%lld moves=%lld\n", (long long)v.size(), s, bad, moves);
    return 0;
}
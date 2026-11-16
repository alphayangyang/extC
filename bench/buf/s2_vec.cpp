// 形状 S2 的对照：每轮一个 std::vector<long long>，reserve(64) 之后 push 64 个，随后析构。
#include <cstdio>
#include <vector>

int main() {
    long long s = 0;
    for (long long i = 0; i < 1000000; ++i) {
        std::vector<long long> t;
        t.reserve(64);
        for (long long j = 0; j < 64; ++j) t.push_back(i * 64 + j);
        for (long long x : t) s += x;
    }
    std::printf("n=1000000 sum=%lld leak=0\n", s);
    return 0;
}
// 形状 S1 的对照：std::vector<long long> 从 empty 开始 push_back（23 次 realloc，几何 2 倍）。
#include <cstdio>
#include <vector>

int main() {
    std::vector<long long> v;
    long long moves = 0, cap = 0;
    for (long long i = 0; i < 8000000; ++i) {
        v.push_back(i);
        if (v.capacity() != cap) { cap = (long long)v.capacity(); ++moves; }
    }
    long long s = 0;
    for (long long x : v) s += x;
    std::printf("n=%lld sum=%lld moves=%lld\n", (long long)v.size(), s, moves);
    return 0;
}
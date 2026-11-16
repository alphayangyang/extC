// 形状 S3 的对照：resize(4096) + shrink_to_fit() 把存储真的还掉（会重新分配），再填回去。
#include <cstdio>
#include <vector>

int main() {
    std::vector<long long> v;
    v.reserve(8000000);
    long long moves = 0, cap = 0;
    for (long long i = 0; i < 8000000; ++i) {
        v.push_back(i);
        if (v.capacity() != cap) { cap = (long long)v.capacity(); ++moves; }
    }
    v.resize(4096);
    v.shrink_to_fit();
    for (long long i = 4096; i < 8000000; ++i) {
        v.push_back(i);
        if (v.capacity() != cap) { cap = (long long)v.capacity(); ++moves; }
    }
    long long s = 0;
    for (long long x : v) s += x;
    std::printf("n=%lld sum=%lld moves=%lld cap=%lld\n", (long long)v.size(), s, moves, (long long)v.capacity());
    return 0;
}
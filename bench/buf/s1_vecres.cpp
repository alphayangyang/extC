// 形状 S1 的第二个对照：知道终值 ⇒ reserve(8e6) 之后 push_back（一次分配，零 realloc）。
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
    long long s = 0;
    for (long long x : v) s += x;
    std::printf("n=%lld sum=%lld moves=%lld\n", (long long)v.size(), s, moves);
    return 0;
}
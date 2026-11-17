// 形状 V1 的对照：std::vector<long long> 从空开始 push_back（几何增长）。
#include <cstdio>
#include <vector>

int main() {
    std::vector<long long> v;
    for (long long i = 0; i < 8000000; ++i) v.push_back(i);
    long long s = 0;
    for (long long x : v) s += x;
    std::printf("n=%lld sum=%lld\n", (long long)v.size(), s);
    return 0;
}
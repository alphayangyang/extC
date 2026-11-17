// 形状 V3 的对照。
#include <cstdio>
#include <vector>

int main() {
    std::vector<long long> v;
    long long s = 0;
    for (int round = 0; round < 2; ++round) {
        for (long long i = 0; i < 8000000; ++i) v.push_back(i);
        for (long long x : v) s += x;
        v.clear();
    }
    std::printf("n=%lld sum=%lld\n", (long long)v.size(), s);
    return 0;
}
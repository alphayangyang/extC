// 形状 V2 的对照：拷贝构造 5 次。
#include <cstdio>
#include <vector>

int main() {
    std::vector<long long> a;
    for (long long i = 0; i < 2000000; ++i) a.push_back(i * 3);
    long long s = 0;
    for (int k = 0; k < 5; ++k) {
        std::vector<long long> b = a;
        s += (long long)b.size() + b[b.size() - 1];
    }
    std::printf("n=%lld sum=%lld\n", (long long)a.size(), s);
    return 0;
}
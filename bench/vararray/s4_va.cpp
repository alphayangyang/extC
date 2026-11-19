// 形状 S4 的对照：一个容器长到 1e6（这里该看 RSS）。
#include <cstdio>
#include <vector>

int main() {
    std::vector<long long> a;
    a.reserve(4);
    for (long long i = 0; i < 10000000; ++i) a.push_back(i);
    std::printf("sum=%lld\n", (long long)a[999999]);
    return 0;
}

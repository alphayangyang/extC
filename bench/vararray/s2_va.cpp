// 形状 S2 的对照：只构造/销毁，不填（把建容器的固定开销单独拎出来）。
#include <cstdio>
#include <vector>

int main() {
    long long sum = 0;
    for (long long r = 0; r < 20000000; ++r) {
        std::vector<int> a;
        a.reserve(4);
        sum += (long long)a.size();
    }
    std::printf("sum=%lld\n", sum);
    return 0;
}

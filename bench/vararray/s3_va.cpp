// 形状 S3 的对照：每轮长到 4096 再丢（每轮几代扩容）。
#include <cstdio>
#include <vector>

int main() {
    long long sum = 0;
    for (long long r = 0; r < 20000; ++r) {
        std::vector<long long> a;
        a.reserve(4);
        for (long long i = 0; i < 4096; ++i) a.push_back(i);
        sum += a[4095];
    }
    std::printf("sum=%lld\n", sum);
    return 0;
}

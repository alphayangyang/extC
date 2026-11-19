// 形状 S1 的对照：每轮新建 vector<int>，填 4 个，丢弃（C++ 版本 reserve(4)）。
#include <cstdio>
#include <vector>

int main() {
    long long sum = 0;
    for (long long r = 0; r < 20000000; ++r) {
        std::vector<int> a;
        a.reserve(4);
        a.push_back(1); a.push_back(2); a.push_back(3); a.push_back(4);
        sum += a[3];
    }
    std::printf("sum=%lld\n", sum);
    return 0;
}

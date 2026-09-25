// 与 bench/stl/vector.extc 同算法同参数（std::vector）
#include <cstdint>
#include <cstdio>
#include <vector>
const int64_t N = 10000000;
int main() {
    std::vector<int32_t> v;
    v.reserve(N);
    int64_t sum = 0, popped = 0;
    for (int64_t i = 0; i < N; i++) v.push_back((int32_t)i);
    for (size_t i = 0; i < v.size(); i++) sum += v[i];
    while (!v.empty()) { popped += v.back(); v.pop_back(); }
    printf("len=%lld cap=%lld sum=%lld popped=%lld\n", (long long)v.size(), (long long)v.capacity(),
           (long long)sum, (long long)popped);
    return 0;
}

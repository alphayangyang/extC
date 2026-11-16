// 形状 S5 的对照：std::vector<long long> 同样顺序扫 + 随机索引扫。
#include <cstdio>
#include <vector>

static unsigned long long st = 1234567;
static unsigned long long nx() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }

int main() {
    std::vector<long long> v;
    v.reserve(16000000);
    for (long long i = 0; i < 16000000; ++i) v.push_back(i * 3);
    long long seq = 0;
    for (long long x : v) seq += x;
    long long idx = 12345, rnd = 0;
    for (long long k = 0; k < 16000000; ++k) { idx = (idx * 1103515245 + 12345) % 16000000; rnd += v[idx]; }
    std::printf("n=%lld seq=%lld rnd=%lld\n", (long long)v.size(), seq, rnd);
    return 0;
}
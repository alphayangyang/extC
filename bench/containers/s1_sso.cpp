// S1 的对照：std::string 短串（libstdc++ 的 SSO 也是 15 字节）。
#include <cstdio>
#include <string>

int main() {
    long long sum = 0;
    for (long long i = 0; i < 10000000; ++i) {
        std::string s;
        s += "abcdefgh";
        s += "ijklmno";
        sum += (long long)s.size();
    }
    std::printf("n=%lld i=%lld\n", sum, 10000000LL);
    return 0;
}

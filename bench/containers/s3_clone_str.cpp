// S3 的对照：std::string 把一条 8 MiB 的串复制 20 次。
#include <cstdio>
#include <string>

int main() {
    std::string base;
    char chunk[1024];
    for (int j = 0; j < 1024; ++j) chunk[j] = (char)(97 + j % 26);
    for (int r = 0; r < 8192; ++r) base.append(chunk, 1024);
    long long sum = (long long)base.size();
    for (int k = 0; k < 20; ++k) {
        std::string c = base;
        sum += (long long)c.size();
    }
    std::printf("n=%lld base=%lld\n", sum, (long long)base.size());
    return 0;
}

// 与 bench/app/route.extc 同算法同参数（std::map，红黑树）
//
// 场景 B：路由表。① 插入/更新 ops 条 ② 前驱查找 ops 次 ③ 区间扫描 ops/4 次 ④ 删除 ops/2 条。
// 用法：route <ops>   （默认 2000000）
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>

static int64_t toInt(const char* s, int64_t dflt) {
    if (s == nullptr || *s == '\0') return dflt;
    int64_t v = 0;
    for (const char* p = s; *p != '\0'; p++) {
        int64_t b = (int64_t)(unsigned char)*p;
        if (b < 48 || b > 57) return dflt;
        v = v * 10 + (b - 48);
    }
    return v;
}

int main(int argc, char** argv) {
    int64_t ops = 2000000;
    if (argc > 1) ops = toInt(argv[1], ops);

    std::map<int64_t, int32_t> routes;
    uint64_t u = 12345;
    int64_t ins = 0;
    int64_t lkp = 0;
    int64_t scan = 0;
    int64_t del = 0;

    // ① 插入 / 更新：键是 /24 前缀（低 8 位清零）
    for (int64_t i = 0; i < ops; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t key = (int64_t)(u & 0xFFFFFF00ULL);
        auto r = routes.insert({key, (int32_t)(key % 1024)});
        if (r.second) ins++;
    }

    // ② 前驱查找：最后一个 ≤ q 的前缀（upper_bound(q) 的前一个）
    u = 12345;
    for (int64_t i = 0; i < ops; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t q = (int64_t)(u & 0xFFFFFFFFULL);
        auto it = routes.upper_bound(q);
        if (it == routes.begin()) {
            lkp -= 1;
        } else {
            --it;
            lkp += (int64_t)it->second;
        }
    }

    // ③ 区间扫描：数 [lo, lo+65536) 里的路由条数
    u = 12345;
    for (int64_t i = 0; i < ops / 4; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t lo = (int64_t)(u & 0xFFFF0000ULL);
        int64_t hi = lo + 65536;
        scan += (int64_t)std::distance(routes.lower_bound(lo), routes.lower_bound(hi));
    }

    // ④ 删除一半
    u = 12345;
    for (int64_t i = 0; i < ops / 2; i++) {
        u = u * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t key = (int64_t)(u & 0xFFFFFF00ULL);
        if (routes.erase(key)) del++;
    }

    printf("b.ins=%lld b.lkp=%lld b.scan=%lld b.del=%lld b.len=%lld\n",
           (long long)ins, (long long)(lkp % 1000000007), (long long)scan,
           (long long)del, (long long)routes.size());
    return 0;
}

// 与 bench/app/session.extc 同算法同参数（std::unordered_map）
//
// 场景 A：会话 / 连接表。查找(60%) + 建/更(20%) + 删(20%)，键为 i64 连接 id。
// 用法：session <ops> <active>   （默认 2000000 / 65536）
#include <cstdint>
#include <cstdio>
#include <unordered_map>

struct Session {
    int64_t user;
    int32_t state;
    int64_t bytes;
    int64_t last;
};

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
    int64_t active = 65536;
    if (argc > 1) ops = toInt(argv[1], ops);
    if (argc > 2) active = toInt(argv[2], active);

    std::unordered_map<int64_t, Session> m;
    m.reserve((size_t)active);
    int64_t acc = 0;
    int64_t hits = 0;
    for (int64_t i = 0; i < ops; i++) {
        int64_t k = i % active;   // 活跃集
        int64_t op = i % 977;     // 与 2 的幂互质 ⇒ 相位逐轮错开
        int64_t ph = op % 5;      // 0,1,2 = 查；3 = 建/更；4 = 删
        if (ph < 3) {
            // 必须用 find，不能用 m[k]（后者会插入）
            auto it = m.find(k);
            int64_t b = (it == m.end()) ? 0 : it->second.bytes;
            acc += b;
            if (b > 0) hits++;
        } else if (ph == 3) {
            Session s{k % 1024, 1, i + k, i};
            m[k] = s;
        } else {
            if (m.erase(k)) acc += 1;
        }
    }
    printf("a.len=%lld a.hits=%lld a.acc=%lld\n",
           (long long)m.size(), (long long)hits, (long long)(acc % 1000000007));
    return 0;
}

// 同一形状的 C++ 基线：一个 vector<pair<int,int>>，每轮塞 per 条、然后清掉。
// A 每轮重建（析构 + 重新构造）· B clear()（保留容量，O(1)）· C 用 swap 释放再建
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <utility>
#include <cstring>
using namespace std;
int main(int argc, char **argv) {
    long rounds = (argc > 1) ? atol(argv[1]) : 200000;
    long per    = (argc > 2) ? atol(argv[2]) : 8;
    char mode   = (argc > 3) ? argv[3][0] : 'A';
    long sink = 0;
    vector<pair<int,int>> *p = nullptr;
    if (mode != 'A') { p = new vector<pair<int,int>>(); p->reserve(4096); }
    for (long r = 0; r < rounds; r++) {
        if (mode == 'A') {
            vector<pair<int,int>> v; v.reserve(4096);
            for (long i = 0; i < per; i++) v.push_back({(int)i, (int)i});
            for (long i = 0; i < per; i++) if (v[i].second) sink += v[i].first;
        } else {
            for (long i = 0; i < per; i++) p->push_back({(int)i,(int)i});
            for (long i = 0; i < per; i++) if ((*p)[i].second) sink += (*p)[i].first;
            if (mode == 'B') p->clear();                       /* 保容量，O(1) */
            else { vector<pair<int,int>> e; e.reserve(4096); p->swap(e); }  /* C */
        }
    }
    printf("cpp mode=%c rounds=%ld per=%ld sink=%ld\n", mode, rounds, per, sink);
    if (p) delete p;
    return 0;
}

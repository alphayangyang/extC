/* tests/frozen/mirror.c —— C 侧的**镜像**：一个 C 程序员为同一份数据会写下的结构体。
 *
 * 它拼在产物**后面**（run.sh 干这件事），于是产物里的 `inner` / `mixed` / `mixed2` 都在作用域里。
 * 断言的正是承诺本身：`@frozen` 说"布局就是 C 的布局"（`C-ABI.md` §9.8），那就拿 C 的布局来对 ——
 * `sizeof` 相等、**每一个字段**的 `offsetof` 相等。这是这一格唯一能真正验"布局对得上"的办法：
 * 产物里那两条 `_Static_assert` 只能钉住"我们不重排、不加隐藏字段"，钉不住"与 C 想的一样"。
 *
 * 编译期断言为主（编译不过就是判据红），运行期再打印一遍，好在日志里留下一行证据。 */
#undef main

struct mirror_inner {
    int64_t x;
    int64_t y;
};

struct mirror_mixed {
    int8_t   a;
    int64_t  b;
    double   f;
    bool     ok;
    int64_t *p;
    uint8_t  arr[4];
    struct mirror_inner inr;
};

struct mirror_mixed2 {
    int64_t quot;
    int64_t rem;
};

_Static_assert(sizeof(mixed) == sizeof(struct mirror_mixed),
               "@frozen: `mixed` must be exactly as large as its C mirror");
_Static_assert(sizeof(inner) == sizeof(struct mirror_inner),
               "@frozen: `inner` must be exactly as large as its C mirror");
_Static_assert(sizeof(mixed2) == sizeof(struct mirror_mixed2),
               "@frozen: `mixed2` must be exactly as large as its C mirror");

#define SAME(T, M, f)                                                              \
    _Static_assert(offsetof(T, f) == offsetof(M, f),                               \
                   "@frozen: field `" #f "` must sit at the C offset")
SAME(mixed, struct mirror_mixed, a);
SAME(mixed, struct mirror_mixed, b);
SAME(mixed, struct mirror_mixed, f);
SAME(mixed, struct mirror_mixed, ok);
SAME(mixed, struct mirror_mixed, p);
SAME(mixed, struct mirror_mixed, arr);
SAME(mixed, struct mirror_mixed, inr);
SAME(inner, struct mirror_inner, x);
SAME(inner, struct mirror_inner, y);
SAME(mixed2, struct mirror_mixed2, quot);
SAME(mixed2, struct mirror_mixed2, rem);
#undef SAME

int main(void) {
    printf("layout ok: mixed=%zu inner=%zu mixed2=%zu (b@%zu p@%zu inr@%zu)\n",
           sizeof(mixed), sizeof(inner), sizeof(mixed2),
           offsetof(mixed, b), offsetof(mixed, p), offsetof(mixed, inr));
    return 0;
}

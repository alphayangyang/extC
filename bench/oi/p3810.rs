// 三维偏序（CDQ 分治 + 树状数组）—— Rust 实现
//
// 跟 `p3810.c` / `p3810.cpp` / `p3810.extc` **同一套算法**。
//
// ⚠️ 排序分组同 C++ 版：
//   · 主组（默认）⇒ 四个语言都手写同一套归并排序（苹果对苹果）
//   · 对照组（`--cfg use_std_sort`）⇒ 用 Rust 自己的 `sort_unstable_by`
//
// 数组用 `Vec`（跟 C/extC 的全局数组对应；`vec![default; N]` 一次分配、零填充）

#[derive(Clone, Copy, Default)]
struct Pt {
    a: i32,
    b: i32,
    c: i32,
    cnt: i32,
    ans: i32,
}

const N: usize = 4_000_000;
const V: usize = 1_000_000;

// ---------------- pcg32（跟 extC prelude 里那份逐位一致）----------------
struct Pcg32 {
    state: u64,
    inc: u64,
}

impl Pcg32 {
    #[inline]
    fn next(&mut self) -> u64 {
        let old = self.state;
        self.state = old.wrapping_mul(6364136223846793005).wrapping_add(self.inc);
        let x = (((old >> 18) ^ old) >> 27) & 0xFFFF_FFFF;
        let rot = old >> 59;
        ((x >> rot) | (x << ((32 - rot) & 31))) & 0xFFFF_FFFF
    }

    fn seeded(seed: u64, stream: u64) -> Pcg32 {
        let mut r = Pcg32 { state: 0, inc: (stream << 1) | 1 };
        let _ = r.next();
        r.state = r.state.wrapping_add(seed);
        let _ = r.next();
        r
    }

    fn bounded(&mut self, bound: u64) -> u64 {
        if bound == 0 {
            return 0;
        }
        let threshold = (0u64).wrapping_sub(bound) % bound;
        let mut x = self.next();
        while x < threshold {
            x = self.next();
        }
        x % bound
    }
}

// ---------------- 求解器 ----------------
struct Solver {
    p: Vec<Pt>,
    tmp: Vec<Pt>,
    bit: Vec<i32>,
}

impl Solver {
    // 权值树状数组（1-indexed：调用点一律 c+1）
    #[inline]
    fn bit_add(&mut self, i: usize, v: i32) {
        let mut x = i;
        while x <= V {
            self.bit[x] += v;
            x += x & x.wrapping_neg();
        }
    }

    #[inline]
    fn bit_sum(&self, i: usize) -> i32 {
        let mut s = 0i32;
        let mut x = i;
        while x > 0 {
            s += self.bit[x];
            x -= x & x.wrapping_neg();
        }
        s
    }

    fn cdq(&mut self, l: usize, r: usize) {
        if l >= r {
            return;
        }
        let mid = l + (r - l) / 2;
        self.cdq(l, mid);
        self.cdq(mid + 1, r);

        // 左半 → 右半：两边都按 b 有序 ⇒ 双指针，c 进树状数组
        let mut i = l;
        let mut j = mid + 1;
        while j <= r {
            while i <= mid && self.p[i].b <= self.p[j].b {
                let (c, cnt) = (self.p[i].c, self.p[i].cnt);
                self.bit_add(c as usize + 1, cnt);
                i += 1;
            }
            let cj = self.p[j].c;
            let s = self.bit_sum(cj as usize + 1);
            self.p[j].ans += s;
            j += 1;
        }
        for t in l..i {
            let (c, cnt) = (self.p[t].c, self.p[t].cnt);
            self.bit_add(c as usize + 1, -cnt);
        }

        // 按 b 归并
        let (mut i, mut j, mut k) = (l, mid + 1, l);
        while i <= mid && j <= r {
            if self.p[i].b <= self.p[j].b {
                self.tmp[k] = self.p[i];
                i += 1;
            } else {
                self.tmp[k] = self.p[j];
                j += 1;
            }
            k += 1;
        }
        while i <= mid {
            self.tmp[k] = self.p[i];
            i += 1;
            k += 1;
        }
        while j <= r {
            self.tmp[k] = self.p[j];
            j += 1;
            k += 1;
        }
        for q in l..=r {
            self.p[q] = self.tmp[q];
        }
    }
}

// ---------------- 手写归并排序（四个语言同一套）----------------
#[inline]
fn less_pt(x: &Pt, y: &Pt) -> bool {
    if x.a != y.a {
        x.a < y.a
    } else if x.b != y.b {
        x.b < y.b
    } else {
        x.c < y.c
    }
}

fn merge_sort(src: &mut [Pt], buf: &mut [Pt], lo: usize, hi: usize) {
    if hi - lo < 2 {
        return;
    }
    let mid = lo + (hi - lo) / 2;
    merge_sort(src, buf, lo, mid);
    merge_sort(src, buf, mid, hi);
    let (mut i, mut j, mut k) = (lo, mid, lo);
    while i < mid && j < hi {
        if less_pt(&src[i], &src[j]) {
            buf[k] = src[i];
            i += 1;
        } else {
            buf[k] = src[j];
            j += 1;
        }
        k += 1;
    }
    while i < mid {
        buf[k] = src[i];
        i += 1;
        k += 1;
    }
    while j < hi {
        buf[k] = src[j];
        j += 1;
        k += 1;
    }
    src[lo..hi].copy_from_slice(&buf[lo..hi]);
}

fn main() {
    let mut r = Pcg32::seeded(20260922, 54);

    let mut sol = Solver {
        p: vec![Pt::default(); N],
        tmp: vec![Pt::default(); N],
        bit: vec![0i32; V + 1],
    };

    for i in 0..N {
        sol.p[i].a = r.bounded(V as u64) as i32;
        sol.p[i].b = r.bounded(V as u64) as i32;
        sol.p[i].c = r.bounded(V as u64) as i32;
        sol.p[i].cnt = 1;
        sol.p[i].ans = 0;
    }

    // 输入本身的校验和 —— 四语言必须一致，不然就是 PRNG 抄错了
    let mut in_sum: u64 = 0;
    for i in 0..N {
        in_sum = in_sum
            .wrapping_mul(1000003)
            .wrapping_add(sol.p[i].a as u64 * 3)
            .wrapping_add(sol.p[i].b as u64 * 5)
            .wrapping_add(sol.p[i].c as u64 * 7);
    }

    // ② 排序
    #[cfg(use_std_sort)]
    sol.p.sort_unstable_by(|x, y| {
        (x.a, x.b, x.c).cmp(&(y.a, y.b, y.c))
    });
    #[cfg(not(use_std_sort))]
    {
        let mut tmp = std::mem::take(&mut sol.tmp);
        merge_sort(&mut sol.p, &mut tmp, 0, N);
        sol.tmp = tmp;
    }

    // ③ 相邻去重
    let mut m = 0usize;
    for i in 0..N {
        if m > 0
            && sol.p[m - 1].a == sol.p[i].a
            && sol.p[m - 1].b == sol.p[i].b
            && sol.p[m - 1].c == sol.p[i].c
        {
            sol.p[m - 1].cnt += 1;
        } else {
            sol.p[m] = sol.p[i];
            sol.p[m].cnt = 1;
            sol.p[m].ans = 0;
            m += 1;
        }
    }

    // ④ 分治
    sol.cdq(0, m - 1);

    // ⑤ 校验和
    let mut sum: u64 = 0;
    let mut max_ans: u64 = 0;
    for i in 0..m {
        let ans = sol.p[i].ans + sol.p[i].cnt - 1;
        sum = sum.wrapping_add(sol.p[i].cnt as u64 * ans as u64);
        if ans as u64 > max_ans {
            max_ans = ans as u64;
        }
    }

    println!("in   = {}", in_sum);
    println!("m    = {}", m);
    println!("max  = {}", max_ans);
    println!("sum  = {}", sum);
}

// Shape 2: cdq -- Rust port of cdq.c (3D dominance, CDQ + weight BIT).
//
// Read n, then n triples (a b c) with values in [1,10^6].
//   1. sort by (a,b,c)   2. collapse duplicates, keeping cnt
//   3. CDQ over b, BIT over c: ans[j] += cnt[i] for every i<j that dominates j
//   4. f = ans + cnt (itself), reported answer = f - 1
//   print sum = SUM cnt_i * ans_i (u64 wrap) and max = max ans_i

use std::io::{self, Read};

/* ---------- fast stdin: a port of fastio.h ---------- */

/// 1 MiB read buffer plus hand-written integer parsing.  `BufRead::lines()`
/// followed by `str::parse` would dominate a run that feeds 10^7 numbers.
struct FastIn<R: Read> {
    r: R,
    buf: Box<[u8]>,
    len: usize,
    pos: usize,
}

impl<R: Read> FastIn<R> {
    fn new(r: R) -> Self {
        FastIn {
            r,
            buf: vec![0u8; 1 << 20].into_boxed_slice(),
            len: 0,
            pos: 0,
        }
    }

    /// One byte, or -1 at end of input (bm_get).
    #[inline]
    fn get(&mut self) -> i32 {
        if self.pos >= self.len {
            self.len = self.r.read(&mut self.buf).unwrap_or(0);
            self.pos = 0;
            if self.len == 0 {
                return -1;
            }
        }
        let c = self.buf[self.pos] as i32;
        self.pos += 1;
        c
    }

    /// Signed integer, skipping ' ', '\n', '\t', '\r' (bm_i64).
    #[inline]
    fn i64(&mut self) -> i64 {
        let mut c = self.get();
        let mut neg = false;
        while c == b' ' as i32 || c == b'\n' as i32 || c == b'\t' as i32 || c == b'\r' as i32 {
            c = self.get();
        }
        if c == b'-' as i32 {
            neg = true;
            c = self.get();
        }
        let mut v: i64 = 0;
        while is_digit(c) {
            v = v.wrapping_mul(10).wrapping_add((c - b'0' as i32) as i64);
            c = self.get();
        }
        if neg {
            -v
        } else {
            v
        }
    }

    /// Unsigned integer, skipping any non-digit (bm_u64).
    #[inline]
    fn u64(&mut self) -> u64 {
        let mut c = self.get();
        let mut v: u64 = 0;
        while !is_digit(c) {
            if c < 0 {
                return 0;
            }
            c = self.get();
        }
        while is_digit(c) {
            v = v.wrapping_mul(10).wrapping_add((c - b'0' as i32) as u64);
            c = self.get();
        }
        v
    }
}

#[inline]
fn is_digit(c: i32) -> bool {
    (b'0' as i32..=b'9' as i32).contains(&c)
}

/* ---------- shape ---------- */

const V: usize = 1_000_000;

/// 5 x i32 = 20 bytes, the same point record layout as the C reference.
#[derive(Clone, Copy, Default)]
struct Pt {
    a: i32,
    b: i32,
    c: i32,
    cnt: i32,
    ans: i32,
}

/// Weight BIT over c, 1-based: c is in [1, V], the array is V+1 wide, and the
/// loop keeps `i <= V`, so every access is in bounds (c = 10^6 -> index V).
#[inline]
fn bit_add(bit: &mut [i32], mut i: usize, v: i32) {
    while i <= V {
        bit[i] += v;
        i += i & i.wrapping_neg();
    }
}

#[inline]
fn bit_sum(bit: &[i32], mut i: usize) -> i32 {
    let mut s = 0i32;
    while i > 0 {
        s += bit[i];
        i -= i & i.wrapping_neg();
    }
    s
}

struct Cdq {
    p: Vec<Pt>,
    tmp: Vec<Pt>,
    bit: Vec<i32>,
}

impl Cdq {
    /// CDQ over b with a weight BIT over c.  Indices stay i64 so that the
    /// empty range of an empty point set (l = 0, r = -1) behaves as in C.
    fn solve(&mut self, l: i64, r: i64) {
        if l >= r {
            return;
        }
        let mid = l + (r - l) / 2;
        self.solve(l, mid);
        self.solve(mid + 1, r);

        let mut i = l;
        let mut j = mid + 1;
        while j <= r {
            while i <= mid && self.p[i as usize].b <= self.p[j as usize].b {
                let (c, cnt) = (self.p[i as usize].c, self.p[i as usize].cnt);
                bit_add(&mut self.bit, c as usize, cnt);
                i += 1;
            }
            let c = self.p[j as usize].c;
            self.p[j as usize].ans += bit_sum(&self.bit, c as usize);
            j += 1;
        }
        for t in l..i {
            let (c, cnt) = (self.p[t as usize].c, self.p[t as usize].cnt);
            bit_add(&mut self.bit, c as usize, -cnt); // undo
        }

        let mut i = l;
        let mut j = mid + 1;
        let mut k = l;
        while i <= mid && j <= r {
            if self.p[i as usize].b <= self.p[j as usize].b {
                self.tmp[k as usize] = self.p[i as usize];
                i += 1;
            } else {
                self.tmp[k as usize] = self.p[j as usize];
                j += 1;
            }
            k += 1;
        }
        while i <= mid {
            self.tmp[k as usize] = self.p[i as usize];
            i += 1;
            k += 1;
        }
        while j <= r {
            self.tmp[k as usize] = self.p[j as usize];
            j += 1;
            k += 1;
        }
        let (l, r) = (l as usize, r as usize);
        self.p[l..=r].copy_from_slice(&self.tmp[l..=r]);
    }
}

fn main() {
    let mut r = FastIn::new(io::stdin().lock());
    let n = r.i64() as usize;

    let mut raw: Vec<Pt> = (0..n)
        .map(|_| Pt {
            a: r.u64() as i32,
            b: r.u64() as i32,
            c: r.u64() as i32,
            cnt: 1,
            ans: 0,
        })
        .collect();

    raw.sort_unstable_by(|x, y| (x.a, x.b, x.c).cmp(&(y.a, y.b, y.c)));

    // Collapse duplicates; the slice is sorted, so equals are adjacent.
    let mut p: Vec<Pt> = Vec::new();
    let mut i = 0usize;
    while i < n {
        let q = raw[i];
        let j = i
            + raw[i..]
                .iter()
                .take_while(|z| z.a == q.a && z.b == q.b && z.c == q.c)
                .count();
        p.push(Pt {
            cnt: (j - i) as i32,
            ..q
        });
        i = j;
    }
    let m = p.len() as i64;

    let mut solver = Cdq {
        p,
        // Zero-initialised on purpose: Rust (like extC) guarantees initialised
        // scratch, so this ~40 MB memset is a deliberate language-level cost
        // and should not be "optimised" away with an unsafe set_len.
        tmp: vec![Pt::default(); m as usize],
        bit: vec![0i32; V + 1],
    };
    solver.solve(0, m - 1);

    let mut sum: u64 = 0;
    let mut mx: i64 = 0;
    for q in &solver.p {
        let ans = q.ans as i64 + q.cnt as i64 - 1;
        sum = sum.wrapping_add((q.cnt as u64).wrapping_mul(ans as u64));
        if ans > mx {
            mx = ans;
        }
    }
    println!("sum={} max={}", sum, mx);
}

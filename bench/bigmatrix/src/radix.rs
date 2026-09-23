// Shape 1: radix -- Rust port of radix.c.
// Read n and n u32 values from stdin, LSD radix sort (4 passes x 8 bits),
// print sorted/first/last.  Same shape as bench/heavy/rs.c, data from stdin.

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

fn main() {
    let mut r = FastIn::new(io::stdin().lock());
    let n = r.i64() as usize;

    let mut a: Vec<u32> = (0..n).map(|_| r.u64() as u32).collect();
    let mut b: Vec<u32> = vec![0u32; n];

    // LSD radix sort: 4 passes x 8 bits, counting sort per byte.  The scatter
    // always goes a -> b, then b is copied back (what gcc compiles the C
    // reference's scalar copy loop into anyway).
    for pass in 0..4u32 {
        let sh = pass * 8;
        let mut cnt = [0i64; 256];
        for &v in &a {
            cnt[((v >> sh) & 255) as usize] += 1;
        }
        let mut sum = 0i64;
        for c in cnt.iter_mut() {
            let k = *c;
            *c = sum;
            sum += k;
        }
        for &v in &a {
            let d = ((v >> sh) & 255) as usize;
            b[cnt[d] as usize] = v;
            cnt[d] += 1;
        }
        a.copy_from_slice(&b);
    }

    let ok = i32::from(!a.windows(2).any(|w| w[0] > w[1]));
    println!("sorted={} first={} last={}", ok, a[0], a[n - 1]);
}

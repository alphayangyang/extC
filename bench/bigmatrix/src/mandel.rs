// Shape 4: mandel -- Rust port of mandel.c.
// Read `w h maxiter`; same loop as bench/heavy/mb.c (f64, escape at |z| > 2).

use std::io::{self, Read};

/* ---------- fast stdin: a port of fastio.h ---------- */

/// 1 MiB read buffer plus hand-written integer parsing.
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
        while (b'0' as i32..=b'9' as i32).contains(&c) {
            v = v.wrapping_mul(10).wrapping_add((c - b'0' as i32) as i64);
            c = self.get();
        }
        if neg {
            -v
        } else {
            v
        }
    }
}

/* ---------- shape ---------- */

fn main() {
    let mut r = FastIn::new(io::stdin().lock());
    let w = r.i64();
    let h = r.i64();
    let maxiter = r.i64();

    let mut img = vec![0u8; (w * h) as usize];
    let mut acc = 0i64;
    for y in 0..h {
        for x in 0..w {
            let cr = x as f64 / w as f64 * 3.5 - 2.5;
            let ci = y as f64 / h as f64 * 2.0 - 1.0;
            let (mut zr, mut zi) = (0.0f64, 0.0f64);
            let mut it = 0i64;
            while it < maxiter {
                let t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                if zr * zr + zi * zi > 4.0 {
                    break;
                }
                it += 1;
            }
            img[(y * w + x) as usize] = (it & 255) as u8;
            acc += it;
        }
    }
    println!("mandel={} img0={}", acc, img[0]);
}

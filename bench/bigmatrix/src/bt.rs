// Shape 3: bt -- Rust port of bt.c.
// Read `maxdepth bigdepth iters`; build perfect binary trees bottom-up,
// count nodes; then count one big tree `iters` times. Same as bench/heavy/bt.c.

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

/// The C `struct tree`: one heap node per tree node, leaves have no children.
struct Tree {
    left: Option<Box<Tree>>,
    right: Option<Box<Tree>>,
}

/// Perfect binary tree of the given depth, built bottom-up: allocate the node
/// first, then recurse, exactly like the C reference's calloc + recursion.
fn bottom_up(depth: i64) -> Box<Tree> {
    let mut t = Box::new(Tree {
        left: None,
        right: None,
    });
    if depth > 0 {
        t.left = Some(bottom_up(depth - 1));
        t.right = Some(bottom_up(depth - 1));
    }
    t
}

/// Node count.  The tree is perfect, so a node either has both children or is
/// a leaf -- the same test as the C reference's `if (!t->left) return 1`.
fn check(t: &Tree) -> i64 {
    match (&t.left, &t.right) {
        (Some(l), Some(r)) => 1 + check(l) + check(r),
        _ => 1,
    }
}

fn main() {
    let mut r = FastIn::new(io::stdin().lock());
    let maxdepth = r.i64();
    let bigdepth = r.i64();
    let iters = r.i64();

    let mut total = 0i64;
    for d in 4..=maxdepth {
        let t = bottom_up(d);
        total += check(&t);
        // `t` is dropped here: ownership replaces the C freeTree(t) call.
    }
    let big = bottom_up(bigdepth);
    for _ in 0..iters {
        total += check(&big);
    }
    // The C reference deliberately never frees the depth-18 tree (see
    // bench/heavy/bt.c): the shape is "build + count", and process exit is what
    // reclaims the memory.  Dropping it here would make Rust do ~524k frees the
    // C side never does -- a cleanup asymmetry, not the shape.  So it leaks on
    // purpose, exactly like the reference.
    std::mem::forget(big);
    println!("trees={}", total);
}

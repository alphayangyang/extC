// Shape 5: rebuild -- Rust port of rebuild.c (build + drop per node).
//
// Read `rounds k`; each round builds a k-node chain, sums it, then drops all of
// it.  The chain is a real `Box` allocation per node and each round's chain is
// released at the end of that round, so the allocator sees the same
// alloc/free pairs as the C reference's malloc/free.

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

/// The C `struct node` (i64 + pointer), one heap allocation per node.
struct Node {
    v: i64,
    next: Option<Box<Node>>,
}

/// Fresh chain of k nodes, newest first -- the same push loop as the C version.
fn build(k: i64) -> Option<Box<Node>> {
    let mut head = None;
    for i in 0..k {
        head = Some(Box::new(Node { v: i, next: head }));
    }
    head
}

/// Walk the chain, head to tail, and sum it.
fn sum_chain(head: Option<&Node>) -> i64 {
    let mut acc = 0i64;
    let mut p = head;
    while let Some(n) = p {
        acc += n.v;
        p = n.next.as_deref();
    }
    acc
}

fn main() {
    let mut r = FastIn::new(io::stdin().lock());
    let rounds = r.i64();
    let k = r.i64();

    let mut acc = 0i64;
    for _ in 0..rounds {
        // Each round's chain is its own binding and is released at the end of
        // the round, so the allocator stays under the same pressure as in C.
        let head = build(k);
        acc += sum_chain(head.as_deref());
        drop(head);
    }
    println!("acc={}", acc);
}

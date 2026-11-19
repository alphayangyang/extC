// 形状 HS1 的 Rust 对照：HashSet 先 with_capacity(N)。
use std::collections::HashSet;
const SEED0: u64 = 0x2545F4914F6CDD1D;
const N: i64 = 4000000;
const SPACE: u64 = 1000000007;
struct Rng { s: u64 }
fn next(r: &mut Rng) -> u64 {
    let mut x = r.s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; r.s = x; x
}
fn main() {
    let mut s: HashSet<i64> = HashSet::with_capacity(N as usize);
    let mut r = Rng { s: SEED0 };
    let mut added: i64 = 0;
    for _i in 0..N {
        let k = (next(&mut r) % SPACE) as i64;
        if s.insert(k) { added += 1; }
    }
    let mut r2 = Rng { s: SEED0 };
    let mut hits: i64 = 0;
    for _i in 0..N {
        let k = (next(&mut r2) % SPACE) as i64;
        if s.contains(&k) { hits += 1; }
    }
    println!("n={} added={} hits={}", s.len(), added, hits);
}

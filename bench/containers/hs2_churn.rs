// 形状 HS2 的 Rust 对照：HashSet 在 1e6 个键上做 8e6 次 insert/remove/contains。
use std::collections::HashSet;
const SEED0: u64 = 0x2545F4914F6CDD1D;
const OPS: i64 = 8000000;
const SPACE: u64 = 1000000;
struct Rng { s: u64 }
fn next(r: &mut Rng) -> u64 {
    let mut x = r.s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; r.s = x; x
}
fn main() {
    let mut s: HashSet<i64> = HashSet::new();
    let mut r = Rng { s: SEED0 };
    let mut added: i64 = 0; let mut rem: i64 = 0; let mut hits: i64 = 0;
    for i in 0..OPS {
        let k = (next(&mut r) % SPACE) as i64;
        let op = i % 3;
        if op == 0 { if s.insert(k) { added += 1; } }
        if op == 1 { if s.remove(&k) { rem += 1; } }
        if op == 2 { if s.contains(&k) { hits += 1; } }
    }
    println!("n={} added={} rem={} hits={}", s.len(), added, rem, hits);
}

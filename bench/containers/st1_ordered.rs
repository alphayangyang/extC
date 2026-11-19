// 形状 ST1 的 Rust 对照：BTreeSet 插入 1e6 条、1e6 次查询、再按有序序走一遍。
use std::collections::BTreeSet;
const SEED0: u64 = 0x2545F4914F6CDD1D;
const N: i64 = 1000000;
const SPACE: u64 = 1000000007;
struct Rng { s: u64 }
fn next(r: &mut Rng) -> u64 {
    let mut x = r.s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; r.s = x; x
}
fn main() {
    let mut s: BTreeSet<i64> = BTreeSet::new();
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
    let mut sum: i64 = 0;
    for x in s.iter() { sum += *x % 1000; }
    println!("n={} added={} hits={} walk={}", s.len(), added, hits, sum);
}

// 形状 H1 的 Rust 对照：HashMap 先 with_capacity(N)。
use std::collections::HashMap;
const SEED0: u64 = 0x2545F4914F6CDD1D;
const N: i64 = 4000000;
const SPACE: u64 = 1000000007;
struct Rng { s: u64 }
fn next(r: &mut Rng) -> u64 {
    let mut x = r.s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; r.s = x; x
}
fn main() {
    let mut m: HashMap<i64, i64> = HashMap::with_capacity(N as usize);
    let mut r = Rng { s: SEED0 };
    for _i in 0..N {
        let k = (next(&mut r) % SPACE) as i64;
        m.insert(k, k * 3 + 1);
    }
    let mut r2 = Rng { s: SEED0 };
    let mut sum: i64 = 0; let mut hits: i64 = 0;
    for _i in 0..N {
        let k = (next(&mut r2) % SPACE) as i64;
        let v = *m.get(&k).unwrap_or(&0);
        if v != 0 { hits += 1; }
        sum += v;
    }
    println!("n={} hits={} sum={}", m.len(), hits, sum);
}
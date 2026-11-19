// 形状 H5 的 Rust 对照：4e6 条之后 8e6 次不命中查询。
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
    let mut r2 = Rng { s: SEED0 ^ 0x5DEECE66D };
    let mut misses: i64 = 0; let mut sum: i64 = 0;
    for _i in 0..N * 2 {
        let k = (next(&mut r2) % SPACE) as i64;
        let v = *m.get(&k).unwrap_or(&-1);
        if v == -1 { misses += 1; } else { sum += v; }
    }
    println!("n={} misses={} sum={}", m.len(), misses, sum);
}
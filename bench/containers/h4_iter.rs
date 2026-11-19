// 形状 H4 的 Rust 对照：2e6 条之后两遍全表遍历。
use std::collections::HashMap;
const SEED0: u64 = 0x2545F4914F6CDD1D;
const N: i64 = 2000000;
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
    let mut vsum: i64 = 0; let mut ksum: i64 = 0;
    for (_k, v) in &m { vsum += *v; }
    for (k, _v) in &m { ksum += *k; }
    let dn = m.len();
    println!("n={} dn={} vsum={} ksum={}", m.len(), dn, vsum, ksum);
}
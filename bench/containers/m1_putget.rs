// 形状 M1 的 Rust 对照：BTreeMap 插入 1e6 条再查 1e6 次。
use std::collections::BTreeMap;
const SEED0: u64 = 0x2545F4914F6CDD1D;
const N: i64 = 1000000;
const SPACE: u64 = 1000000007;
struct Rng { s: u64 }
fn next(r: &mut Rng) -> u64 {
    let mut x = r.s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; r.s = x; x
}
fn main() {
    let mut m: BTreeMap<i64, i64> = BTreeMap::new();
    let mut r = Rng { s: SEED0 };
    let mut added: i64 = 0;
    for _i in 0..N {
        let k = (next(&mut r) % SPACE) as i64;
        match m.get_mut(&k) {
            Some(slot) => { *slot = k * 3 + 1; }
            None => { m.insert(k, k * 3 + 1); added += 1; }
        }
    }
    let mut r2 = Rng { s: SEED0 };
    let mut sum: i64 = 0; let mut hits: i64 = 0;
    for _i in 0..N {
        let k = (next(&mut r2) % SPACE) as i64;
        let v = *m.get(&k).unwrap_or(&0);
        if v != 0 { hits += 1; }
        sum += v;
    }
    println!("n={} added={} hits={} sum={}", m.len(), added, hits, sum);
}

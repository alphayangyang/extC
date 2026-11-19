// 形状 H3 的 Rust 对照：同一条插入/查询/删除动作序列。
use std::collections::HashMap;
const SEED0: u64 = 0x2545F4914F6CDD1D;
const OPS: i64 = 8000000;
const SPACE: u64 = 1000000;
struct Rng { s: u64 }
fn next(r: &mut Rng) -> u64 {
    let mut x = r.s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; r.s = x; x
}
fn main() {
    let mut m: HashMap<i64, i64> = HashMap::with_capacity(1024);
    let mut r = Rng { s: SEED0 };
    let mut sum: i64 = 0; let mut hits: i64 = 0;
    let mut removed: i64 = 0; let mut repl: i64 = 0;
    for _i in 0..OPS {
        let x = next(&mut r);
        let k = (x % SPACE) as i64;
        let op = (x >> 40) % 4;
        if op < 2 {
            if m.insert(k, (x % 1000003) as i64).is_some() { repl += 1; }
        } else if op == 2 {
            let v = *m.get(&k).unwrap_or(&-1);
            if v != -1 { hits += 1; }
            sum += v;
        } else {
            if m.remove(&k).is_some() { removed += 1; }
        }
    }
    println!("n={} hits={} removed={} repl={} sum={}", m.len(), hits, removed, repl, sum);
}
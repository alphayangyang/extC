// 形状 LS1 的 Rust 对照：Vec<i64> + 线性成员检查。
const SEED0: u64 = 0x2545F4914F6CDD1D;
const N: i64 = 1024;
const Q: i64 = 1000000;
const SPACE: u64 = 2048;
struct Rng { s: u64 }
fn next(r: &mut Rng) -> u64 {
    let mut x = r.s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; r.s = x; x
}
fn has(v: &Vec<i64>, k: i64) -> bool {
    for x in v.iter() { if *x == k { return true; } }
    false
}
fn main() {
    let mut s: Vec<i64> = Vec::with_capacity(N as usize);
    let mut r = Rng { s: SEED0 };
    let mut added: i64 = 0;
    for _i in 0..N {
        let k = (next(&mut r) % SPACE) as i64;
        if !has(&s, k) { s.push(k); added += 1; }
    }
    let mut r2 = Rng { s: SEED0 };
    let mut hits: i64 = 0;
    for _i in 0..Q {
        let k = (next(&mut r2) % SPACE) as i64;
        if has(&s, k) { hits += 1; }
    }
    println!("n={} added={} hits={}", s.len(), added, hits);
}

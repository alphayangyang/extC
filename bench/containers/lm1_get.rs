// 形状 LM1 的 Rust 对照：Vec<(i64,i64)> + 线性查找。
const SEED0: u64 = 0x2545F4914F6CDD1D;
const N: i64 = 1024;
const Q: i64 = 1000000;
const SPACE: u64 = 2048;
struct Rng { s: u64 }
fn next(r: &mut Rng) -> u64 {
    let mut x = r.s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; r.s = x; x
}
fn find_in(v: &Vec<(i64, i64)>, k: i64) -> i64 {
    for i in 0..v.len() { if v[i].0 == k { return i as i64; } }
    -1
}
fn main() {
    let mut m: Vec<(i64, i64)> = Vec::with_capacity(N as usize);
    let mut r = Rng { s: SEED0 };
    for _i in 0..N {
        let k = (next(&mut r) % SPACE) as i64;
        let at = find_in(&m, k);
        if at >= 0 { m[at as usize].1 = k * 3 + 1; }
        else { m.push((k, k * 3 + 1)); }
    }
    let mut r2 = Rng { s: SEED0 };
    let mut sum: i64 = 0; let mut hits: i64 = 0;
    for _i in 0..Q {
        let k = (next(&mut r2) % SPACE) as i64;
        let at = find_in(&m, k);
        let v = if at < 0 { 0 } else { m[at as usize].1 };
        if v != 0 { hits += 1; }
        sum += v;
    }
    println!("n={} hits={} sum={}", m.len(), hits, sum);
}

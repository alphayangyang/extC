// 形状 LM2 的 Rust 对照：Vec<(i64,i64)> + 线性查找 + swap_remove。
const SEED0: u64 = 0x2545F4914F6CDD1D;
const N: i64 = 1024;
const OPS: i64 = 1000000;
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
    let mut rem: i64 = 0; let mut hits: i64 = 0; let mut sum: i64 = 0;
    for i in 0..OPS {
        let k = (next(&mut r) % SPACE) as i64;
        let op = i % 3;
        let at = find_in(&m, k);
        if op == 0 {
            if at >= 0 { m[at as usize].1 = k * 3 + 1; }
            else { m.push((k, k * 3 + 1)); }
        }
        if op == 1 {
            if at >= 0 { m.swap_remove(at as usize); rem += 1; }
        }
        if op == 2 {
            let v = if at < 0 { 0 } else { m[at as usize].1 };
            if v != 0 { hits += 1; }
            sum += v;
        }
    }
    println!("n={} rem={} hits={} sum={}", m.len(), rem, hits, sum);
}

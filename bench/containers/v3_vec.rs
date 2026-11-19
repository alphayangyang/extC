// 形状 V3 的 Rust 对照：装满 8e6、clear()、再装满一次。
fn main() {
    let mut v: Vec<i64> = Vec::new();
    let mut s: i64 = 0;
    for _round in 0..2 {
        for i in 0..8000000i64 { v.push(i); }
        for x in &v { s += *x; }
        v.clear();
    }
    println!("n={} sum={}", v.len(), s);
}
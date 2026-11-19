// 形状 V1 的 Rust 对照：Vec<long long> 从空开始 push（几何增长）。
fn main() {
    let mut v: Vec<i64> = Vec::new();
    for i in 0..8000000i64 { v.push(i); }
    let mut s: i64 = 0;
    for x in &v { s += *x; }
    println!("n={} sum={}", v.len(), s);
}
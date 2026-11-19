// 形状 V2 的 Rust 对照：装满 2e6 后连做 5 次 clone。
fn main() {
    let mut a: Vec<i64> = Vec::new();
    for i in 0..2000000i64 { a.push(i * 3); }
    let mut s: i64 = 0;
    for _k in 0..5 {
        let b = a.clone();
        s += b.len() as i64 + b[b.len() - 1];
    }
    println!("n={} sum={}", a.len(), s);
}
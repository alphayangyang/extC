// 形状 V4 的 Rust 对照：20 万个小向量（容量 8、装 8 个 i64）的生死循环。
fn main() {
    let mut total: i64 = 0;
    for r in 0..200000i64 {
        let mut v: Vec<i64> = Vec::with_capacity(8);
        for i in 0..8i64 { v.push(r + i); }
        for x in &v { total += *x; }
    }
    println!("n={} sum={}", 8, total);
}
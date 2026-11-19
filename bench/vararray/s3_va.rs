// 形状 S3 的 Rust 对照：每轮长到 4096 再丢。
fn main() {
    let mut sum: i64 = 0;
    for _r in 0..20000 {
        let mut a: Vec<i64> = Vec::with_capacity(4);
        for i in 0..4096i64 { a.push(i); }
        sum += a[4095];
    }
    println!("sum={}", sum);
}

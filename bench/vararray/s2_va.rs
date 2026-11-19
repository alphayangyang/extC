// 形状 S2 的 Rust 对照：只构造/销毁，不填。
fn main() {
    let mut sum: i64 = 0;
    for _r in 0..20000000 {
        let a: Vec<i32> = Vec::with_capacity(4);
        sum += a.len() as i64;
    }
    println!("sum={}", sum);
}

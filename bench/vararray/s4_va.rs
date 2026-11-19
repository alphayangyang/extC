// 形状 S4 的 Rust 对照：一个容器长到 1e6。
fn main() {
    let mut a: Vec<i64> = Vec::with_capacity(4);
    for i in 0..10000000i64 { a.push(i); }
    println!("sum={}", a[999999]);
}

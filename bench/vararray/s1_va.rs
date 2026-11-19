// 形状 S1 的 Rust 对照：每轮新建 Vec<i32>（with_capacity(4)），填 4 个，丢弃。
fn main() {
    let mut sum: i64 = 0;
    for _r in 0..20000000 {
        let mut a: Vec<i32> = Vec::with_capacity(4);
        a.push(1); a.push(2); a.push(3); a.push(4);
        sum += a[3] as i64;
    }
    println!("sum={}", sum);
}

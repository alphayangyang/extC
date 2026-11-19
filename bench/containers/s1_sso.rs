// 形状 S1 的 Rust 对照：String 短串周转 1000 万次。
fn main() {
    let mut sum: i64 = 0;
    for _i in 0..10000000i64 {
        let mut s = String::new();
        s.push_str("abcdefgh");
        s.push_str("ijklmno");
        sum += s.len() as i64;
    }
    println!("n={} i={}", sum, 10000000i64);
}
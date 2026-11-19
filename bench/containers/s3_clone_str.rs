// 形状 S3 的 Rust 对照：8 MiB 串 clone 20 次。
fn main() {
    let mut chunk = [0u8; 1024];
    for j in 0..1024 { chunk[j] = (97 + j % 26) as u8; }
    let piece = std::str::from_utf8(&chunk).unwrap();
    let mut base = String::new();
    for _r in 0..8192 { base.push_str(piece); }
    let mut sum: i64 = base.len() as i64;
    for _k in 0..20 {
        let c = base.clone();
        sum += c.len() as i64;
    }
    println!("n={} base={}", sum, base.len());
}
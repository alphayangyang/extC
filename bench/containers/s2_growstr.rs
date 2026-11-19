// 形状 S2 的 Rust 对照：16 条串各 append 64 字节块 65536 次。
fn main() {
    let mut sum: i64 = 0;
    let mut chunk = [0u8; 64];
    for j in 0..64 { chunk[j] = (65 + j % 26) as u8; }
    let piece = std::str::from_utf8(&chunk).unwrap();
    for _k in 0..16 {
        let mut s = String::new();
        for _r in 0..65536 { s.push_str(piece); }
        sum += s.len() as i64;
    }
    println!("n={} cap={}", sum, 1);
}
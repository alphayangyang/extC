/* Rust · 输入：读整块 + `split_whitespace` + `parse`（惯用的快写法 ✓）
 * 契约：`in <文件|->` ⇒ 打印总和 ✓ */
use std::fs;
use std::io::{self, Read};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut s = String::new();
    if args[1] == "-" {
        io::stdin().read_to_string(&mut s).unwrap();
    } else {
        s = fs::read_to_string(&args[1]).unwrap();
    }
    let mut total: i64 = 0;
    for w in s.split_whitespace() {
        total += w.parse::<i64>().unwrap();
    }
    println!("{}", total);
}

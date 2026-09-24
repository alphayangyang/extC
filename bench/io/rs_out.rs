/* Rust · 输出：`BufWriter` + `writeln!` ✓ */
use std::fs::File;
use std::io::{self, BufWriter, Write};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let n: i64 = args[2].parse().unwrap();
    if args[1] == "-" {
        let out = io::stdout();
        let mut w = BufWriter::new(out.lock());
        for i in 0..n { writeln!(w, "{} {}", i, i * i).unwrap(); }
    } else {
        let f = File::create(&args[1]).unwrap();
        let mut w = BufWriter::new(f);
        for i in 0..n { writeln!(w, "{} {}", i, i * i).unwrap(); }
    }
}

// 形状 1：churn（Rust）—— 与 churn.extc 逐字同语义。
struct Node { v: i64, #[allow(dead_code)] next: Option<Box<Node>> }

fn main() {
    const ROUNDS: i64 = 32_000_000;
    let mut acc: i64 = 0;
    for i in 0..ROUNDS {
        let mut a: Option<Box<Node>> = None;
        let mut b: Option<Box<Node>> = None;
        let p = Box::new(Node { v: i, next: None });
        if i & 1 == 0 {
            if let Some(old) = a.take() { acc += old.v; }
            a = Some(p);
        } else {
            if let Some(old) = b.take() { acc += old.v; }
            b = Some(p);
        }
        if let Some(n) = &a { acc += n.v; }
        if let Some(n) = &b { acc += n.v; }
    }
    println!("{}", acc);
}

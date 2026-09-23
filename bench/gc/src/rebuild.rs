// GC 敏感形状 2：每轮重建（Rust，Box 链，每轮自动 drop ⇒ 递归释放）。
struct Node { v: i64, next: Option<Box<Node>> }

fn main() {
    const ROUNDS: i64 = 20000;
    const K: i64 = 1000;
    let mut acc: i64 = 0;
    for _ in 0..ROUNDS {
        let mut h: Option<Box<Node>> = None;
        for i in 0..K {
            h = Some(Box::new(Node { v: i, next: h }));
        }
        let mut p = &h;
        while let Some(n) = p {
            acc += n.v;
            p = &n.next;
        }
    }
    println!("{}", acc);
}

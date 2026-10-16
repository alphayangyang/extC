// v2 G1 promote —— 分代假设：每轮 9 个短命节点（下轮即垃圾），每 10 轮提升 1 个长命节点
// 进 65536 槽定长表，每轮再走一个长命桶（最多 8 个）。
// 与 bench/gc/v2/src/promote.extc 同算法 / 同 PRNG / 同调用顺序；stdout 必须逐字节相同。
//
// 构建（run.sh 的两条臂）：
//   rustc -O --edition 2021               src/promote.rs -o b/promote.rs.bin
//   rustc -O --edition 2021 --cfg careful src/promote.rs -o b/promote.rs_careful.bin

const MASK: i64 = 4_611_686_018_427_387_903; // 2^62 - 1
const TABLE: usize = 65536;
const SHORT: i64 = 9;
const EVERY: i64 = 10;
const WALKCAP: i64 = 8;

struct Node {
    #[allow(dead_code)]
    k: i64,
    v: i64,
    next: Option<Box<Node>>,
}

struct Rng {
    state: i64,
}

impl Rng {
    fn nxt(&mut self) -> i64 {
        self.state = (self.state * 1_103_515_245 + 12345) & 2147483647;
        self.state
    }
}

fn parse_i64(s: &str) -> i64 {
    let mut v: i64 = 0;
    for c in s.bytes() {
        if !(b'0'..=b'9').contains(&c) {
            break;
        }
        v = v * 10 + (c - b'0') as i64;
    }
    v
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let mut n: i64 = 500_000;
    if argv.len() > 1 {
        n = parse_i64(&argv[1]);
    }
    let mut sd: i64 = 20261013;
    if argv.len() > 2 {
        sd = parse_i64(&argv[2]);
    }
    let mut rng = Rng { state: sd & 2147483647 };

    // 定长表：只按下标读写（SPEC §0 允许 Vec 当定长数组用）。
    let mut table: Vec<Option<Box<Node>>> = Vec::with_capacity(TABLE);
    table.resize_with(TABLE, || None);

    let mut sum: i64 = 0;
    let mut walksum: i64 = 0;
    let mut live: i64 = 0;
    let mut tailv: i64 = 0;

    for i in 0..n {
        let mut head: Option<Box<Node>> = None;
        for j in 0..SHORT {
            let s = Box::new(Node {
                k: i * SHORT + j,
                v: rng.nxt() % 1000,
                next: head.take(),
            });
            sum = (sum + s.v) & MASK;
            head = Some(s);
        }
        // 尾端读回：短命链最后一次的头值进 stdout。分配必须真的发生（SPEC §0）。
        if let Some(h) = &head {
            tailv = h.v;
        }

        if i % EVERY == 0 {
            let mut p = Box::new(Node { k: i, v: rng.nxt() % 1000, next: None });
            let b = (i % TABLE as i64) as usize;
            p.next = table[b].take();
            table[b] = Some(p);
            live += 1;
        }

        let b2 = ((i * 40503) % TABLE as i64) as usize;
        let mut c = table[b2].as_deref();
        let mut cnt: i64 = 0;
        while let Some(node) = c {
            if cnt >= WALKCAP {
                break;
            }
            walksum = (walksum + node.v) & MASK;
            c = node.next.as_deref();
            cnt += 1;
        }
    }

    println!("rounds={} promoted={} walksum={} sum={} tailv={}", n, live, walksum, sum, tailv);
    println!("table=65536 short=9 every=10 walkcap=8");
}

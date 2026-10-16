// v2 G3 chase —— 大活集指针追逐 + churn。
// 两趟：先建 M 个节点的图，再连边（建边时从图里取目标）；然后随机游走 W 步，
// 每 256 步顺手分配 4 个即抛节点。
// 与 bench/gc/v2/src/chase.extc 同算法 / 同 PRNG / 同调用顺序；stdout 必须逐字节相同。
//
// 节点间是多对多引用（4 条出边 + 图数组），Rust 的自然写法就是 Rc<RefCell<Node>>
// （与 bench/gc/src/digraph.rs 同一套约定）。默认/--cfg careful 两臂源码相同。

use std::cell::RefCell;
use std::rc::Rc;

const MASK: i64 = 4_611_686_018_427_387_903; // 2^62 - 1
const CAP: usize = 1_048_576;
const OUTDEG: usize = 4;
const CHURN_EVERY: i64 = 256;
const CHURN: i64 = 4;

type NodeRef = Rc<RefCell<Node>>;

struct Node {
    #[allow(dead_code)]
    k: i64,
    v: i64,
    o: [Option<NodeRef>; OUTDEG],
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

fn pick(rng: &mut Rng, k: i64) -> i64 {
    (rng.nxt() / 1024) % k
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
    let mut m: i64 = 1_048_576;
    if argv.len() > 1 {
        m = parse_i64(&argv[1]);
    }
    let mut sd: i64 = 20261013;
    if argv.len() > 2 {
        sd = parse_i64(&argv[2]);
    }
    let mut w: i64 = 4_000_000;
    if argv.len() > 3 {
        w = parse_i64(&argv[3]);
    }
    let mut rng = Rng { state: sd & 2147483647 };
    assert!(m as usize <= CAP);

    // 图数组：定长 CAP 槽（Vec 当定长数组用）。
    let mut graph: Vec<Option<NodeRef>> = vec![None; CAP];

    // 第一趟：建 M 个节点。
    for i in 0..m {
        let x = Rc::new(RefCell::new(Node {
            k: i,
            v: rng.nxt() % 1000,
            o: [None, None, None, None],
        }));
        graph[i as usize] = Some(x);
    }

    // 第二趟：连 4 条出边。
    for i in 0..m {
        let src = graph[i as usize].clone().expect("graph slot empty");
        for j in 0..OUTDEG {
            let t = (rng.nxt() % m) as usize;
            let tgt = graph[t].clone().expect("graph slot empty");
            src.borrow_mut().o[j] = Some(tgt);
        }
    }

    let mut cur = graph[0].clone().expect("graph slot empty");
    let mut sum: i64 = 0;
    let mut churn: i64 = 0;
    let mut tailv: i64 = 0;
    let mut step: i64 = 0;
    while step < w {
        sum = (sum + cur.borrow().v) & MASK;

        let r = pick(&mut rng, OUTDEG as i64) as usize;
        let nx = cur.borrow().o[r].clone();
        if let Some(x) = nx {
            cur = x;
        }

        if step % CHURN_EVERY == 0 {
            // 4 个即抛节点串成短链，本轮结束即整体回收（尾端读回 h.v）。
            let mut h: Option<NodeRef> = None;
            for _ in 0..CHURN {
                let s = Rc::new(RefCell::new(Node {
                    k: step,
                    v: rng.nxt() % 1000,
                    o: [h.take(), None, None, None],
                }));
                h = Some(s);
            }
            if let Some(x) = &h {
                tailv = x.borrow().v;
            }
            churn += CHURN;
        }
        step += 1;
    }

    println!(
        "nodes={} edges={} walk={} churn={} sum={} tailv={}",
        m, m * OUTDEG as i64, w, churn, sum, tailv
    );
    println!("outdeg=4 cap=1048576 churn_every=256 churn=4");
}

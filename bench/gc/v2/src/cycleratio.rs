// v2 G4 cycleratio —— 环垃圾占比 f。
// 每轮造 k(3..6) 个节点、以 f% 概率闭成环，放进 256 槽窗口；窗口满后每组顶掉一组旧组。
// 成环的那组被顶掉后是不可达环：Rc 引用计数收不掉（naive 臂），追踪式 GC 能收。
// 与 bench/gc/v2/src/cycleratio.extc 同算法 / 同 PRNG / 同调用顺序；stdout 必须逐字节相同。
//
//   naive  ：环用强引用闭合 ⇒ 被顶掉后自持，泄漏（RSS 随规模涨）。
//   careful：顶掉前显式断环（走到组尾把 next 置空）⇒ 强计数级联归零。
// 两臂 stdout 完全相同，差的只是内存行为。

use std::cell::RefCell;
use std::rc::Rc;

const MASK: i64 = 4_611_686_018_427_387_903; // 2^62 - 1
const WINDOW: usize = 256;

type NodeRef = Rc<RefCell<Node>>;

struct Node {
    #[allow(dead_code)]
    k: i64,
    v: i64,
    next: Option<NodeRef>,
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

fn mk_node(k: i64, v: i64) -> NodeRef {
    Rc::new(RefCell::new(Node { k, v, next: None }))
}

// 顶掉一组旧节点。naive 直接丢引用（成环的那组自持 ⇒ 泄漏）；
// careful 先顺着 next 走到组尾、把 next 置空，环随即被强计数级联回收。
fn retire(old: Option<NodeRef>, k: i64) {
    let Some(head) = old else { return };
    #[cfg(careful)]
    {
        let mut cur = head.clone();
        for _ in 0..k.saturating_sub(1) {
            let nxt = cur.borrow().next.clone();
            match nxt {
                Some(x) => cur = x,
                None => break,
            }
        }
        cur.borrow_mut().next = None;
    }
    #[cfg(not(careful))]
    let _ = k; // naive 臂用不到组尾节点数
    drop(head);
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let mut g: i64 = 400_000;
    if argv.len() > 1 {
        g = parse_i64(&argv[1]);
    }
    let mut sd: i64 = 20261013;
    if argv.len() > 2 {
        sd = parse_i64(&argv[2]);
    }
    let mut f: i64 = 100;
    if argv.len() > 3 {
        f = parse_i64(&argv[3]);
    }
    let mut rng = Rng { state: sd & 2147483647 };

    // 窗口：256 槽，每槽 (头节点, 该组节点数)。
    let mut win: Vec<Option<NodeRef>> = vec![None; WINDOW];
    let mut win_k = [0i64; WINDOW];

    let mut sum: i64 = 0;
    let mut made: i64 = 0;
    let mut evicted: i64 = 0;
    let mut dropnodes: i64 = 0;
    let mut rings: i64 = 0;

    for i in 0..g {
        let k = 3 + pick(&mut rng, 4);
        let is_ring = pick(&mut rng, 100) < f;

        let mut first: Option<NodeRef> = None;
        let mut prev: Option<NodeRef> = None;
        for _ in 0..k {
            let s = mk_node(i, rng.nxt() % 1000);
            if first.is_none() {
                first = Some(s.clone());
            }
            if let Some(p) = &prev {
                p.borrow_mut().next = Some(s.clone());
            }
            prev = Some(s.clone());
            sum = (sum + s.borrow().v) & MASK;
        }
        if is_ring {
            rings += 1;
            if let Some(p) = &prev {
                p.borrow_mut().next = first.clone();
            }
        }
        made += k;

        let b = (i % WINDOW as i64) as usize;
        let old = win[b].take();
        if old.is_some() {
            evicted += 1;
            dropnodes += win_k[b];
        }
        retire(old, win_k[b]);
        win[b] = first;
        win_k[b] = k;
    }

    println!(
        "groups={} made={} evicted={} dropnodes={} sum={}",
        g, made, evicted, dropnodes, sum
    );
    println!("f={} window=256 kmin=3 kmax=6 rings={}", f, rings);
}

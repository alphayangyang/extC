// bench/gc 形状 7：digraph —— Rust 两个档位（naive / careful），无 cargo。
// 与 bench/gc/src/digraph.extc **逐字同算法/同 PRNG/同顺序**，输出必须逐字节一致。
//
//   naive  ：所有引用都是 Rc<RefCell<Node>>（回指 back 也是）⇒ 环状垃圾收不掉。
//   careful：回指 back 用 Weak，丢弃分量时显式断开（first / ord / back 全清）
//            ⇒ 环被打破，强计数级联归零。careful 的额外成本 = 断开那一趟。
//
// 构建：
//   rustc -O --edition 2021               digraph.rs -o digraph_rs_naive
//   rustc -O --edition 2021 --cfg careful digraph.rs -o digraph_rs_careful
use std::cell::RefCell;
use std::rc::Rc;
#[cfg(careful)]
use std::rc::Weak;

const MASK: i64 = 4611686018427387903;

type NodeRef = Rc<RefCell<Node>>;
type HandleRef = Rc<RefCell<Handle>>;

#[cfg(careful)]
type BackRef = Weak<RefCell<Node>>;
#[cfg(not(careful))]
type BackRef = NodeRef;

#[cfg(careful)]
fn make_back(p: &NodeRef) -> BackRef {
    Rc::downgrade(p)
}
#[cfg(not(careful))]
fn make_back(p: &NodeRef) -> BackRef {
    Rc::clone(p)
}

struct Edge {
    to: NodeRef,
    next: Option<Rc<Edge>>,
    kind: i64,
}

struct Node {
    id: i64,
    gid: i64,
    payload: i64,
    first: Option<Rc<Edge>>,
    ord: Option<NodeRef>,
    back: Option<BackRef>,
    mark: i64,
    cmark: i64,
    snext: Option<NodeRef>,
    gpin: bool,
}

impl Node {
    fn new() -> Node {
        Node {
            id: 0,
            gid: 0,
            payload: 0,
            first: None,
            ord: None,
            back: None,
            mark: 0,
            cmark: 0,
            snext: None,
            gpin: false,
        }
    }
}

struct Handle {
    target: NodeRef,
    newest: NodeRef,
    size: i64,
    prev: Option<HandleRef>,
    next: Option<HandleRef>,
}

struct Ctx {
    head: Option<HandleRef>,
    tail: Option<HandleRef>,
    n: i64,
    hubs: [Option<NodeRef>; 8],
    hub_count: i64,
}

struct G {
    #[allow(dead_code)]
    break_stats: bool,
    #[allow(dead_code)]
    break_ns: i64,
    #[allow(dead_code)]
    break_calls: i64,
    state: i64,
    next_id: i64,
    gid_counter: i64,
    backedges: i64,
    nodes_m: i64,
    groups_m: i64,
    dropped_g: i64,
    dropped_n: i64,
    rejected: i64,
    cycles_dropped: i64,
    acc: i64,
    xacc: i64,
    last_nodes: i64,
    last_edges: i64,
    epoch: i64,
    cepoch: i64,
}

fn nxt(g: &mut G) -> i64 {
    g.state = (g.state * 1103515245 + 12345) & 2147483647;
    g.state
}

fn mixv(a: i64, b: i64) -> i64 {
    let mut x = (a * 2654435761 + b * 40503 + 12345) & MASK;
    x ^= x >> 12;
    x & MASK
}

fn parse_num(s: &str, def: i64) -> i64 {
    if s.is_empty() {
        return def;
    }
    let mut v: i64 = 0;
    for ch in s.bytes() {
        if !(b'0'..=b'9').contains(&ch) {
            return def;
        }
        v = v * 10 + (ch - b'0') as i64;
    }
    v
}

fn get_ord(n: &NodeRef) -> Option<NodeRef> {
    n.borrow().ord.clone()
}
fn get_first(n: &NodeRef) -> Option<Rc<Edge>> {
    n.borrow().first.clone()
}
fn get_gpin(n: &NodeRef) -> bool {
    n.borrow().gpin
}
fn get_id(n: &NodeRef) -> i64 {
    n.borrow().id
}
fn get_gid(n: &NodeRef) -> i64 {
    n.borrow().gid
}
fn get_mark(n: &NodeRef) -> i64 {
    n.borrow().mark
}
fn get_cmark(n: &NodeRef) -> i64 {
    n.borrow().cmark
}

fn handle_at(c: &Ctx, idx: i64) -> HandleRef {
    let mut h = c.head.clone();
    let mut i = 0i64;
    while i < idx {
        let cur = h.expect("handle index out of range");
        h = cur.borrow().next.clone();
        i += 1;
    }
    h.expect("handle index out of range")
}

fn node_in_group(h: &HandleRef, j: i64) -> NodeRef {
    let newest = h.borrow().newest.clone();
    let mut x = newest;
    let mut i = 0i64;
    while i < j {
        let nxt = get_ord(&x);
        match nxt {
            Some(y) => x = y,
            None => break,
        }
        i += 1;
    }
    x
}

fn add_edge(s: &NodeRef, t: &NodeRef, k: i64) {
    let e = Rc::new(Edge {
        to: Rc::clone(t),
        next: get_first(s),
        kind: k,
    });
    s.borrow_mut().first = Some(e);
}

// 组内"更老"（id 更小）的一个随机祖先；没有就退回 hub
fn older_in_group(g: &mut G, s: &NodeRef, hub: &NodeRef) -> NodeRef {
    let mut c1 = 0i64;
    let mut y = get_ord(s);
    while let Some(yy) = y {
        c1 += 1;
        y = get_ord(&yy);
    }
    if c1 == 0 {
        return Rc::clone(hub);
    }
    let j = nxt(g) % c1;
    let mut z = get_ord(s);
    let mut i2 = 0i64;
    while let Some(zz) = z {
        if i2 == j {
            return zz;
        }
        i2 += 1;
        z = get_ord(&zz);
    }
    Rc::clone(hub)
}

// 组内"更新"（id 更大）的一个随机后代；没有就退回 hub
fn newer_in_group(g: &mut G, s: &NodeRef, newest: &NodeRef, hub: &NodeRef) -> NodeRef {
    let sid = get_id(s);
    let mut c2 = 0i64;
    let mut y = Some(Rc::clone(newest));
    while let Some(yy) = y {
        if get_id(&yy) > sid {
            c2 += 1;
        }
        y = get_ord(&yy);
    }
    if c2 == 0 {
        return Rc::clone(hub);
    }
    let j = nxt(g) % c2;
    let mut z = Some(Rc::clone(newest));
    let mut i2 = 0i64;
    while let Some(zz) = z {
        if get_id(&zz) > sid {
            if i2 == j {
                return zz;
            }
            i2 += 1;
        }
        z = get_ord(&zz);
    }
    Rc::clone(hub)
}

// 建组时的弦边目标：60% 组内更老 / 20% 组内更新 / 10% 组内 hub / 10% 组内长程随机
fn pick_chord_target(
    g: &mut G,
    s: &NodeRef,
    hub: &NodeRef,
    newest: &NodeRef,
    size: i64,
) -> NodeRef {
    let r = nxt(g) % 100;
    if r < 60 {
        return older_in_group(g, s, hub);
    }
    if r < 80 {
        return newer_in_group(g, s, newest, hub);
    }
    if r < 90 {
        return Rc::clone(hub);
    }
    let j3 = nxt(g) % size;
    let mut z3 = Some(Rc::clone(newest));
    let mut i4 = 0i64;
    while let Some(zz) = z3 {
        if i4 == j3 {
            return zz;
        }
        i4 += 1;
        z3 = get_ord(&zz);
    }
    Rc::clone(hub)
}

// 组内是否存在有向环（丢弃时现场验证；只用组内边，独立于"构造上必有环"）
fn group_has_cycle(g: &mut G, newest: &NodeRef, gid: i64) -> bool {
    let mut u = Some(Rc::clone(newest));
    while let Some(uu) = u {
        u = get_ord(&uu);
        g.cepoch += 1;
        let ep = g.cepoch;
        let uid = get_id(&uu);
        let mut top: Option<NodeRef> = None;
        let mut e = get_first(&uu);
        while let Some(ee) = e {
            let t = Rc::clone(&ee.to);
            if get_gid(&t) == gid {
                if get_id(&t) == uid {
                    return true;
                }
                if get_cmark(&t) != ep {
                    t.borrow_mut().cmark = ep;
                    t.borrow_mut().snext = top.take();
                    top = Some(t);
                }
            }
            e = ee.next.clone();
        }
        let mut found = false;
        while let Some(x) = top.take() {
            top = x.borrow().snext.clone();
            x.borrow_mut().snext = None;
            if get_id(&x) == uid {
                found = true;
            }
            if !found {
                let mut e2 = get_first(&x);
                while let Some(ee2) = e2 {
                    let t2 = Rc::clone(&ee2.to);
                    if get_gid(&t2) == gid && get_cmark(&t2) != ep {
                        t2.borrow_mut().cmark = ep;
                        t2.borrow_mut().snext = top.take();
                        top = Some(t2);
                    }
                    e2 = ee2.next.clone();
                }
            }
        }
        if found {
            return true;
        }
    }
    false
}

// careful 档的"工程费"：摘组前把该组显式断开（清 first / ord / back）。
// 用 Vec 收集强引用，防止清链过程中结点被释放。
#[cfg(careful)]
const BREAK_RING: usize = 262144;  // 覆盖最大样本数（1M 档 219025 次）⇒ 不截断
#[cfg(careful)]
static mut BREAK_RING_V: [i64; BREAK_RING] = [0; BREAK_RING];

// 逐次断开时长分布（门控 DIGRAPH_BREAK_STATS=1；只写 stderr）。既有 break_ns/break_calls 行原样保留。
#[cfg(careful)]
fn break_record(ns: i64) {
    unsafe {
        let g = &mut *std::ptr::addr_of_mut!(BREAK_RING_V);
        let n = (*std::ptr::addr_of!(BREAK_CTX)).n;
        if n < BREAK_RING as i64 {
            g[n as usize] = ns;
            (*std::ptr::addr_of_mut!(BREAK_CTX)).n = n + 1;
        } else {
            (*std::ptr::addr_of_mut!(BREAK_CTX)).over += 1;
        }
    }
}

#[cfg(careful)]
struct BreakCtx {
    n: i64,
    over: i64,
}
#[cfg(careful)]
static mut BREAK_CTX: BreakCtx = BreakCtx { n: 0, over: 0 };

#[cfg(careful)]
fn break_dump_dist() {
    unsafe {
        let ctx = &*std::ptr::addr_of!(BREAK_CTX);
        let n = ctx.n as usize;
        let g = &mut *std::ptr::addr_of_mut!(BREAK_RING_V);
        let vals = &mut g[..n];
        vals.sort_unstable();
        let pct = |p: usize| -> f64 {
            if n == 0 { 0.0 } else { vals[(n - 1) * p / 100] as f64 }
        };
        let (mut b100, mut b200, mut b500, mut b1us, mut b4us, mut b16us, mut bge) =
            (0i64, 0i64, 0i64, 0i64, 0i64, 0i64, 0i64);
        let mut sum: i64 = 0;
        for &d in vals.iter() {
            sum += d;
            if d < 100 { b100 += 1; }
            if d < 200 { b200 += 1; }
            if d < 500 { b500 += 1; }
            if d < 1000 { b1us += 1; }
            if d < 4000 { b4us += 1; }
            if d < 16000 { b16us += 1; }
            if d >= 16000 { bge += 1; }
        }
        eprintln!(
            "BREAK_DIST source=rust n={} overflow={} p50_ns={:.0} p90_ns={:.0} p99_ns={:.0} max_ns={:.0} total_ms={:.3} b_lt100ns={} b_lt200ns={} b_lt500ns={} b_lt1us={} b_lt4us={} b_lt16us={} b_ge16us={}",
            n, ctx.over, pct(50), pct(90), pct(99),
            if n == 0 { 0.0 } else { vals[n - 1] as f64 }, sum as f64 / 1e6,
            b100, b200, b500, b1us, b4us, b16us, bge
        );
    }
}

#[cfg(careful)]
fn break_group(newest: &NodeRef) {
    let mut list: Vec<NodeRef> = Vec::new();
    let mut y = Some(Rc::clone(newest));
    while let Some(yy) = y {
        y = get_ord(&yy);
        list.push(yy);
    }
    for x in &list {
        let mut b = x.borrow_mut();
        b.first = None;
        b.ord = None;
        b.back = None;
    }
}

fn create_group(g: &mut G, c: &mut Ctx, k: i64, pin: bool, is_hub: bool) {
    let gid = g.gid_counter;
    g.gid_counter += 1;
    g.groups_m += 1;

    let hub = Rc::new(RefCell::new(Node::new()));
    {
        let mut h = hub.borrow_mut();
        h.id = g.next_id;
        h.gid = gid;
        h.payload = nxt(g) % 1000;
        h.gpin = pin;
    }
    g.next_id += 1;
    g.nodes_m += 1;
    if is_hub {
        c.hubs[0] = Some(Rc::clone(&hub));
        c.hub_count = 1;
    }

    let mut prev = Rc::clone(&hub);
    let mut cnt = 1i64;
    while cnt < k {
        let n = Rc::new(RefCell::new(Node::new()));
        {
            let mut nn = n.borrow_mut();
            nn.id = g.next_id;
            nn.gid = gid;
            nn.payload = nxt(g) % 1000;
            nn.ord = Some(Rc::clone(&prev));
            nn.back = Some(make_back(&prev));
            nn.gpin = pin;
        }
        g.next_id += 1;
        g.nodes_m += 1;
        if is_hub {
            c.hubs[cnt as usize] = Some(Rc::clone(&n));
            c.hub_count = cnt + 1;
        }
        add_edge(&prev, &n, 1);
        prev = n;
        cnt += 1;
    }

    add_edge(&prev, &hub, 1);
    g.backedges += 1;

    let mut x = Some(Rc::clone(&prev));
    while let Some(xx) = x {
        let d = nxt(g) % 3;
        for _ in 0..d {
            let tgt = pick_chord_target(g, &xx, &hub, &prev, k);
            add_edge(&xx, &tgt, 2);
        }
        x = get_ord(&xx);
    }

    let h = Rc::new(RefCell::new(Handle {
        target: Rc::clone(&hub),
        newest: Rc::clone(&prev),
        size: k,
        prev: c.tail.clone(),
        next: None,
    }));
    match c.tail.clone() {
        Some(t) => {
            t.borrow_mut().next = Some(Rc::clone(&h));
        }
        None => {
            c.head = Some(Rc::clone(&h));
        }
    }
    c.tail = Some(h);
    c.n += 1;
}

// 丢一个组：(c) 步。优先摘无跨组入边（gpin==false）的组；顺手做现场环验证与计数。
fn try_drop(g: &mut G, c: &mut Ctx) {
    if c.n == 0 {
        return;
    }
    let base = nxt(g) % c.n;
    let mut probe = 0i64;
    while probe < 8 {
        let mut idx = base + probe;
        while idx >= c.n {
            idx -= c.n;
        }
        let h = handle_at(c, idx);
        let newest = h.borrow().newest.clone();
        let target = h.borrow().target.clone();
        let hgid = get_gid(&target);

        let mut cnt = 0i64;
        let mut anypin = false;
        let mut x = Some(Rc::clone(&newest));
        while let Some(xx) = x {
            cnt += 1;
            if get_gpin(&xx) {
                anypin = true;
            }
            x = get_ord(&xx);
        }
        if anypin {
            g.rejected += 1;
            probe += 1;
            continue;
        }
        g.dropped_g += 1;
        g.dropped_n += cnt;
        if group_has_cycle(g, &newest, hgid) {
            g.cycles_dropped += 1;
        }

        #[cfg(careful)]
        {
            // 显式断开本组 ⇒ 环被打破，强计数级联归零。
            // DIGRAPH_BREAK_STATS=1 时把这一趟的时间累计起来（只写 stderr，stdout 不变）。
            if g.break_stats {
                let bt0 = std::time::Instant::now();
                break_group(&newest);
                let d = bt0.elapsed().as_nanos() as i64;
                g.break_ns += d;
                break_record(d);
                g.break_calls += 1;
            } else {
                break_group(&newest);
            }
        }

        #[cfg(assert_refs)]
        {
            // 诊断档（`--cfg assert_refs`）专用的**不变量断言**：断开后，本组结点只应由
            // handle（target/newest）与本函数的两个局部强引用持有 —— 这正是 `gpin`
            // 不变量（"无跨组入边"）的**实测**版本。若有任何外部强引用残留 ⇒ 立即 abort。
            let tc = Rc::strong_count(&target);
            let nc = Rc::strong_count(&newest);
            let same = Rc::ptr_eq(&target, &newest);
            let exp = if same { 4 } else { 2 };  // handle 两个槽 + 两个局部 clone
            if tc != exp || nc != exp {
                eprintln!("ASSERT-REFS FAIL gid={} target_count={} newest_count={} expect={}",
                          hgid, tc, nc, exp);
                std::process::abort();
            }
        }
        let hp = h.borrow().prev.clone();
        let hn = h.borrow().next.clone();
        match &hp {
            Some(p) => {
                p.borrow_mut().next = hn.clone();
            }
            None => {
                c.head = hn.clone();
            }
        }
        match &hn {
            Some(nn) => {
                nn.borrow_mut().prev = hp.clone();
            }
            None => {
                c.tail = hp.clone();
            }
        }
        {
            let mut hb = h.borrow_mut();
            hb.prev = None;
            hb.next = None;
        }
        c.n -= 1;
        return;
    }
}

// (d) 步：从 handle 表出发对存活部分做一次遍历（显式栈，用 snext 串），累加校验和。
fn traverse(g: &mut G, c: &Ctx, ep: i64) {
    let mut top: Option<NodeRef> = None;
    let mut h = c.head.clone();
    while let Some(hh) = h {
        let t = hh.borrow().target.clone();
        if get_mark(&t) != ep {
            t.borrow_mut().mark = ep;
            t.borrow_mut().snext = top.take();
            top = Some(t);
        }
        h = hh.borrow().next.clone();
    }
    let mut rn = 0i64;
    let mut re = 0i64;
    let mut radd = 0i64;
    let mut rxor = 0i64;
    while let Some(n) = top.take() {
        top = n.borrow().snext.clone();
        n.borrow_mut().snext = None;
        rn += 1;
        let nid = get_id(&n);
        radd = (radd + mixv(nid, n.borrow().payload)) & MASK;
        let mut e = get_first(&n);
        while let Some(ee) = e {
            re += 1;
            rxor ^= mixv(nid, get_id(&ee.to)) ^ (ee.kind * 4099);
            let t2 = Rc::clone(&ee.to);
            if get_mark(&t2) != ep {
                t2.borrow_mut().mark = ep;
                t2.borrow_mut().snext = top.take();
                top = Some(t2);
            }
            e = ee.next.clone();
        }
    }
    g.acc = (g.acc + radd + rn * 1000003 + re * 31) & MASK;
    g.xacc = (g.xacc ^ (rxor ^ (rn * 1000003 + re))) & MASK;
    g.last_nodes = rn;
    g.last_edges = re;
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let mut n_size: i64 = 200000;
    let mut sd: i64 = 20261013;
    if argv.len() > 1 {
        n_size = parse_num(&argv[1], n_size);
    }
    if argv.len() > 2 {
        sd = parse_num(&argv[2], sd);
    }
    if n_size < 256 {
        n_size = 256;
    }

    let mut g = G {
        break_stats: std::env::var("DIGRAPH_BREAK_STATS").is_ok(),
        break_ns: 0,
        break_calls: 0,
        state: sd & 2147483647,
        next_id: 0,
        gid_counter: 0,
        backedges: 0,
        nodes_m: 0,
        groups_m: 0,
        dropped_g: 0,
        dropped_n: 0,
        rejected: 0,
        cycles_dropped: 0,
        acc: 0,
        xacc: 0,
        last_nodes: 0,
        last_edges: 0,
        epoch: 0,
        cepoch: 0,
    };
    let mut c = Ctx {
        head: None,
        tail: None,
        n: 0,
        hubs: Default::default(),
        hub_count: 0,
    };

    create_group(&mut g, &mut c, 8, true, true);

    let rounds = 16 + n_size / 4096;
    let mut batch = n_size / rounds;
    if batch < 24 {
        batch = 24;
    }

    for _ in 0..rounds {
        // (a) 建
        let mut created = 0i64;
        let mut groups_this_round = 0i64;
        while created < batch {
            let k = 3 + nxt(&mut g) % 4;
            create_group(&mut g, &mut c, k, false, false);
            created += k;
            groups_this_round += 1;
        }
        // (b) 改：删头边 + 按 60/20/10/10 加边
        let m_ops = 2 + nxt(&mut g) % 3;
        for _ in 0..m_ops {
            if c.n > 0 {
                let idx = nxt(&mut g) % c.n;
                let h = handle_at(&c, idx);
                let hsize = h.borrow().size;
                let s = node_in_group(&h, nxt(&mut g) % hsize);
                let f = get_first(&s);
                if let Some(ff) = f {
                    if ff.next.is_some() {
                        let nn = ff.next.clone();
                        s.borrow_mut().first = nn;
                    }
                }
                let r = nxt(&mut g) % 100;
                let mut tgt: Option<NodeRef> = None;
                if r < 60 {
                    let hub = h.borrow().target.clone();
                    tgt = Some(older_in_group(&mut g, &s, &hub));
                } else if r < 80 {
                    let hub = h.borrow().target.clone();
                    let newest = h.borrow().newest.clone();
                    tgt = Some(newer_in_group(&mut g, &s, &newest, &hub));
                } else if r < 90 {
                    if c.hub_count > 0 {
                        let hj = nxt(&mut g) % c.hub_count;
                        tgt = c.hubs[hj as usize].clone();
                    }
                } else {
                    let idx2 = nxt(&mut g) % c.n;
                    let h2 = handle_at(&c, idx2);
                    let h2size = h2.borrow().size;
                    tgt = Some(node_in_group(&h2, nxt(&mut g) % h2size));
                }
                if let Some(tg) = tgt {
                    if get_gid(&tg) != get_gid(&s) {
                        tg.borrow_mut().gpin = true;
                    }
                    add_edge(&s, &tg, 3);
                }
            }
        }
        // (c) 丢
        for _ in 0..groups_this_round {
            try_drop(&mut g, &mut c);
        }
        // (d) 遍历
        g.epoch += 1;
        let ep = g.epoch;
        traverse(&mut g, &c, ep);
    }

    let total = (g.acc + g.xacc) & MASK;
    println!("nodes={} edges={} sum={}", g.last_nodes, g.last_edges, total);
    println!(
        "backedges={} groups_made={} nodes_made={} dropped_groups={} dropped_nodes={} rejected_external={} cycles_dropped={}",
        g.backedges, g.groups_m, g.nodes_m, g.dropped_g, g.dropped_n, g.rejected, g.cycles_dropped
    );
    #[cfg(careful)]
    if g.break_stats {
        eprintln!("break_ns={} break_calls={}", g.break_ns, g.break_calls);
        break_dump_dist();
    }
}

// 同一份功能，用**最标准的 Rust 写法**：
//   · 节点被多处共享（桶链 / 缓存 / 索引）⇒ 共享可变所有权 = Rc<RefCell<Node>>
//   · 桶是有界队列 ⇒ VecDeque，淘汰就是 pop_back（最后一个持有者消失时自动释放）
//   · 解析的 3 个临时对象是栈值，不分配
// 这就是"手动引用计数 + 所有权"版的同一功能：内存自动有界，无 GC，无停顿。
use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;
use std::time::Instant;

#[derive(Default)]
struct Node { key: i64, val: i64 }
type NodeRef = Rc<RefCell<Node>>;

const BUCKETS: usize = 64;
const BUCKET_MAX: usize = 8;
const CACHE: usize = 1024;
const INDEXMAX: usize = 200000;

fn main() {
    let n: usize = std::env::args().nth(1).unwrap().parse().unwrap();
    let mut buckets: Vec<VecDeque<NodeRef>> = (0..BUCKETS).map(|_| VecDeque::with_capacity(16)).collect();
    let mut cache: Vec<Option<NodeRef>> = vec![None; CACHE];
    let mut index: Vec<NodeRef> = Vec::with_capacity(INDEXMAX);
    let mut sink: i64 = 0;
    let t0 = Instant::now();
    for i in 0..n {
        let b = i % BUCKETS;
        // 解析：3 个临时对象是栈值，不分配
        let t = [i as i64, i as i64 + 1, i as i64 + 2];
        sink += t.iter().sum::<i64>();
        // 插入桶（共享所有权）
        let nd: NodeRef = Rc::new(RefCell::new(Node { key: i as i64, val: i as i64 * 3 }));
        buckets[b].push_front(Rc::clone(&nd));
        cache[i % CACHE] = Some(Rc::clone(&nd));
        if i < INDEXMAX { index.push(Rc::clone(&nd)); }
        // 查询
        for q in buckets[b].iter() {
            let mut m = q.borrow_mut();
            if m.key == i as i64 - 7 { m.val += 1; break; }
        }
        // 淘汰：队列超限就弹尾（若别处还持有则对象存活，否则当场释放）
        if buckets[b].len() > BUCKET_MAX { let _ = buckets[b].pop_back(); }
    }
    let dt = t0.elapsed();
    println!("rust-rc 耗时={:.1}ms sink={} idx={}", dt.as_secs_f64() * 1000.0, sink, index.len() + cache[0].is_none() as usize);
}

// 同一份功能，但用 Rust 的写法：索引化 arena（Vec<Node> + u32 下标），无 GC、无回收。
// 注意两点根本差异：
//   1) 解析的 3 个临时对象是**局部栈值**，根本不分配（零成本抽象）；
//   2) 淘汰只从链上摘掉，槽位不复用 ⇒ arena 单调增长（对应"靠作用域/所有权兜底"的做法）。
use std::time::Instant;
const BUCKETS: usize = 64;
const BUCKET_MAX: i32 = 8;
const CACHE: usize = 1024;
const INDEXMAX: usize = 200000;
const NIL: u32 = u32::MAX;

struct Node { key: i64, val: i64, next: u32 }

fn main() {
    let n: usize = std::env::args().nth(1).unwrap().parse().unwrap();
    let mut nodes: Vec<Node> = Vec::with_capacity(n + 8);
    let mut head = [NIL; BUCKETS];
    let mut cache = vec![NIL; CACHE];
    let mut idx: Vec<(u32, u32)> = Vec::with_capacity(INDEXMAX + 1); // (next, target)
    let mut ihead: u32 = NIL;
    let mut sink: i64 = 0;
    let t0 = Instant::now();
    for i in 0..n {
        let b = i % BUCKETS;
        // 解析：3 个临时对象——Rust 里就是栈上的值，不分配
        let (t1, t2, t3) = (i as i64, i as i64 + 1, i as i64 + 2);
        let mut q = 0i64;
        for v in [t1, t2, t3] { q += v; }
        sink += q;
        // 插入桶
        let id = nodes.len() as u32;
        nodes.push(Node { key: i as i64, val: i as i64 * 3, next: head[b] });
        head[b] = id;
        cache[i % CACHE] = id;
        if i < INDEXMAX { idx.push((ihead, id)); ihead = (idx.len() - 1) as u32; }
        // 查询
        let mut c = head[b];
        while c != NIL {
            let nd = &mut nodes[c as usize];
            if nd.key == i as i64 - 7 { nd.val += 1; break; }
            c = nd.next;
        }
        // 淘汰尾节点（只摘链，槽位不复用）
        let mut prev = NIL; let mut cur = head[b]; let mut cnt = 0;
        while cur != NIL && nodes[cur as usize].next != NIL {
            prev = cur; cur = nodes[cur as usize].next; cnt += 1;
            if cnt > BUCKET_MAX { break; }
        }
        if cur != NIL && cnt >= BUCKET_MAX {
            if prev != NIL { nodes[prev as usize].next = NIL; } else { head[b] = NIL; }
        }
    }
    let dt = t0.elapsed();
    println!("rust    耗时={:.1}ms sink={} idx={}", dt.as_secs_f64() * 1000.0, sink, idx.len() + (ihead == NIL) as usize);
}

// 静态区间第 k 小（主席树 / 可持久化线段树，P3834）—— **Rust 实现**
//
// 跟 `p3834.c` / `p3834.cpp` / `p3834.extc` **同一套算法**（逐行对应）。
//
// 写法上用地道的 Rust：
//   · 节点池是 `Vec<Node>`（不是裸指针 + unsafe，也不用 `Box` 一节点一次堆分配）
//   · 下标用 `u32`（节点号）与 `usize`（池内位置）；`as` 转换写在该写的地方
//   · 用 `#[derive(Default)]` + `vec!`/`push`，没有宏、没有手写 memset
//   · `--cfg grow`：池子**不预留**（`Vec::new()`），靠 `push` 自己长 ⇒ 对照组 ✓
//
// ⚠️ 一个值得注意的点：这里**故意没有 `unsafe`** —— 持久化线段树的下标是
//    "自己维护的池子下标"，Rust 里写它完全不需要裸指针 ✓（安全 && 快，是可以同时有的）

#[derive(Clone, Copy, Default)]
struct Node {
    l: u32,
    r: u32,
    sum: u32,
}

const N: usize = 1_000_000;
const Q: usize = 1_000_000;
const V: u32 = 1_000_000;
const DEPTH: usize = 21; // 每个数开的新节点数 = ⌈log2 V⌉ + 1
const POOL_SIZE: usize = N * DEPTH + 2;

// ---------------- pcg32（跟 extC prelude 里那份逐位一致）----------------
struct Pcg32 {
    state: u64,
    inc: u64,
}

impl Pcg32 {
    #[inline]
    fn next(&mut self) -> u64 {
        let old = self.state;
        self.state = old.wrapping_mul(6364136223846793005).wrapping_add(self.inc);
        let x = (((old >> 18) ^ old) >> 27) & 0xFFFF_FFFF;
        let rot = old >> 59;
        ((x >> rot) | (x << ((32 - rot) & 31))) & 0xFFFF_FFFF
    }

    fn seeded(seed: u64, stream: u64) -> Pcg32 {
        let mut r = Pcg32 { state: 0, inc: (stream << 1) | 1 };
        let _ = r.next();
        r.state = r.state.wrapping_add(seed);
        let _ = r.next();
        r
    }

    #[inline]
    fn bounded(&mut self, bound: u64) -> u64 {
        if bound == 0 {
            return 0;
        }
        let threshold = (0u64.wrapping_sub(bound)) % bound;
        let mut x = self.next();
        while x < threshold {
            x = self.next();
        }
        x % bound
    }
}

// ---------------- 可持久化线段树 ----------------
struct Tree {
    pool: Vec<Node>,
}

impl Tree {
    fn new() -> Tree {
        let mut t = Tree { pool: Vec::new() };
        #[cfg(not(grow))]
        t.pool.reserve(POOL_SIZE);
        // 0 号是空节点（l = r = sum = 0）
        t.pool.push(Node::default());
        t
    }

    fn count(&self) -> u32 {
        self.pool.len() as u32
    }

    /// 建版本：`prev` 那条路径整条复制，其余指针共享
    fn update(&mut self, prev: u32, lo: u32, hi: u32, pos: u32) -> u32 {
        let rt = self.pool.len() as u32;
        let prev_node = self.pool[prev as usize];
        self.pool.push(Node { l: prev_node.l, r: prev_node.r, sum: prev_node.sum + 1 });
        if lo < hi {
            let mid = lo + (hi - lo) / 2;
            if pos <= mid {
                let child = self.pool[prev as usize].l;
                let nl = self.update(child, lo, mid, pos);
                self.pool[rt as usize].l = nl;
            } else {
                let child = self.pool[prev as usize].r;
                let nr = self.update(child, mid + 1, hi, pos);
                self.pool[rt as usize].r = nr;
            }
        }
        rt
    }

    fn query(&self, u: u32, v: u32, lo: u32, hi: u32, k: u32) -> u32 {
        if lo == hi {
            return lo;
        }
        let mid = lo + (hi - lo) / 2;
        let left_count = self.pool[self.pool[v as usize].l as usize].sum
            - self.pool[self.pool[u as usize].l as usize].sum;
        if k <= left_count {
            let (ul, vl) = (self.pool[u as usize].l, self.pool[v as usize].l);
            self.query(ul, vl, lo, mid, k)
        } else {
            let (ur, vr) = (self.pool[u as usize].r, self.pool[v as usize].r);
            self.query(ur, vr, mid + 1, hi, k - left_count)
        }
    }
}

fn main() {
    let mut rng = Pcg32::seeded(20260922, 54);

    let mut a = vec![0u32; N + 1];
    for i in 1..=N {
        a[i] = rng.bounded(V as u64) as u32 + 1;
    }

    let mut in_sum: u64 = 0;
    for i in 1..=N {
        in_sum = in_sum.wrapping_mul(1000003).wrapping_add(a[i] as u64);
    }

    let mut tree = Tree::new();
    let mut root = vec![0u32; N + 1];
    for i in 1..=N {
        root[i] = tree.update(root[i - 1], 1, V, a[i]);
    }

    let mut ans_sum: u64 = 0;
    for _ in 0..Q {
        let l = 1 + rng.bounded(N as u64) as usize;
        let r = l + rng.bounded((N - l + 1) as u64) as usize;
        let k = 1 + rng.bounded((r - l + 1) as u64) as usize;
        let ans = tree.query(root[l - 1], root[r], 1, V, k as u32);
        ans_sum = ans_sum.wrapping_mul(1000009).wrapping_add(ans as u64);
    }

    println!("in    = {}", in_sum);
    println!("n     = {}", N);
    println!("q     = {}", Q);
    println!("nodes = {}", tree.count());
    println!("ans   = {}", ans_sum);
}

// v2 G2 sizeratio —— 4 个尺寸类（32/64/96/128 字节）的对象池。
// 每轮分配一个随机尺寸类的对象，其中 1/8 存进该类 1024 槽的定长表（顶掉同槽旧对象），
// 其余 7/8 立即变垃圾。
// 与 bench/gc/v2/src/sizeratio.extc 同算法 / 同 PRNG / 同调用顺序；stdout 必须逐字节相同。

const MASK: i64 = 4_611_686_018_427_387_903; // 2^62 - 1
const SLOTS: usize = 1024;

struct C0 { k: i64, #[allow(dead_code)] p: [i64; 3] }
struct C1 { k: i64, #[allow(dead_code)] p: [i64; 7] }
struct C2 { k: i64, #[allow(dead_code)] p: [i64; 11] }
struct C3 { k: i64, #[allow(dead_code)] p: [i64; 15] }

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

// 定长表的自然初始化：Vec 只按下标读写。
fn slots<T>() -> Vec<Option<Box<T>>> {
    let mut v = Vec::with_capacity(SLOTS);
    v.resize_with(SLOTS, || None);
    v
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let mut n: i64 = 2_000_000;
    if argv.len() > 1 {
        n = parse_i64(&argv[1]);
    }
    let mut sd: i64 = 20261013;
    if argv.len() > 2 {
        sd = parse_i64(&argv[2]);
    }
    let mut rng = Rng { state: sd & 2147483647 };

    let mut a0: Vec<Option<Box<C0>>> = slots();
    let mut a1: Vec<Option<Box<C1>>> = slots();
    let mut a2: Vec<Option<Box<C2>>> = slots();
    let mut a3: Vec<Option<Box<C3>>> = slots();

    let mut sum: i64 = 0;
    let mut kept: i64 = 0;
    let mut livebytes: i64 = 0;

    for i in 0..n {
        let cls = pick(&mut rng, 4);
        match cls {
            0 => {
                let o = Box::new(C0 { k: i, p: [0; 3] });
                sum = (sum + o.k) & MASK;
                if i % 8 == 0 {
                    let b = ((i / 8) % SLOTS as i64) as usize;
                    if a0[b].take().is_some() { livebytes -= 32; } else { kept += 1; }
                    a0[b] = Some(o);
                    livebytes += 32;
                }
            }
            1 => {
                let o = Box::new(C1 { k: i, p: [0; 7] });
                sum = (sum + o.k) & MASK;
                if i % 8 == 0 {
                    let b = ((i / 8) % SLOTS as i64) as usize;
                    if a1[b].take().is_some() { livebytes -= 64; } else { kept += 1; }
                    a1[b] = Some(o);
                    livebytes += 64;
                }
            }
            2 => {
                let o = Box::new(C2 { k: i, p: [0; 11] });
                sum = (sum + o.k) & MASK;
                if i % 8 == 0 {
                    let b = ((i / 8) % SLOTS as i64) as usize;
                    if a2[b].take().is_some() { livebytes -= 96; } else { kept += 1; }
                    a2[b] = Some(o);
                    livebytes += 96;
                }
            }
            _ => {
                let o = Box::new(C3 { k: i, p: [0; 15] });
                sum = (sum + o.k) & MASK;
                if i % 8 == 0 {
                    let b = ((i / 8) % SLOTS as i64) as usize;
                    if a3[b].take().is_some() { livebytes -= 128; } else { kept += 1; }
                    a3[b] = Some(o);
                    livebytes += 128;
                }
            }
        }
    }

    println!("ops={} kept={} livebytes={} sum={}", n, kept, livebytes, sum);
    println!("classes=4 sizes=32..128 slots=1024");
}

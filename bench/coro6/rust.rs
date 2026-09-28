// Rust：std-only 最小 executor。async fn + `.await` 就是状态机（= 别的语言的协程），
// 每次 yield 走一次 Pending 再一次 Ready（两趟 poll），这点在表里注明。
use std::future::Future;
use std::pin::pin;
use std::task::{Context, Poll, Waker};
struct Yield { val: i64, polled: bool }
impl Future for Yield {
    type Output = i64;
    fn poll(mut self: std::pin::Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<i64> {
        if self.polled { Poll::Ready(self.val) } else { self.polled = true; Poll::Pending }
    }
}
async fn gen(id: i64, n: i64, salt: i64) -> i64 {
    let mut s = 0i64;
    for i in 0..n { s += Yield { val: (i % 65536) * 65521 + id * 40503 + salt, polled: false }.await; }
    s
}
fn block_on<F: Future>(f: F) -> F::Output {
    let mut f = pin!(f);
    let mut cx = Context::from_waker(Waker::noop());
    loop { if let Poll::Ready(v) = f.as_mut().poll(&mut cx) { return v; } }
}
fn run(k: i64, m: i64, salt: i64) -> i64 { let mut s = 0i64; for id in 0..k { s += block_on(gen(id, m, salt)); } s }
fn main() {
    let mut t = run(8, 2000, 0);
    for r in 0..R_ROUNDS { t += run(512, 20000, r as i64); }
    println!("{}", t);
}

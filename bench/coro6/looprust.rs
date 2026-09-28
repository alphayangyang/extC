fn run(k: i64, m: i64) -> i64 { let mut s = 0i64;
  for id in 0..k { for i in 0..m { s += (i % 65536) * 65521 + id * 40503; } } s }
fn main(){ let mut t = run(8,2000); for _ in 0..1 { t += run(512,20000); } println!("{}", t); }

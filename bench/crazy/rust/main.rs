// bench/crazy —— Rust 臂：同一份 TSV、同一行结果。
use std::collections::HashMap;
use std::time::Instant;

#[derive(Clone, Copy)]
struct Row {
    ts: i64,
    uid: i64,
    kind: i64,
    region: i64,
    cents: i64,
    ms: i64,
}

const MOD: i64 = 1_000_000_007;

fn main() {
    let threads: usize = std::env::var("CRAZY_THREADS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(1);

    let t0 = Instant::now();
    let buf = std::fs::read("bench/crazy/data/events.tsv").expect("read");
    let mut rows: Vec<Row> = Vec::with_capacity(1_600_000);
    let mut cur = Row { ts: 0, uid: 0, kind: 0, region: 0, cents: 0, ms: 0 };
    let mut field = 0;
    for &ch in buf.iter() {
        match ch {
            b'\n' => {
                rows.push(cur);
                cur = Row { ts: 0, uid: 0, kind: 0, region: 0, cents: 0, ms: 0 };
                field = 0;
            }
            b'\t' => field += 1,
            _ => {
                let d = (ch - b'0') as i64;
                match field {
                    0 => cur.ts = cur.ts * 10 + d,
                    1 => cur.uid = cur.uid * 10 + d,
                    2 => cur.kind = cur.kind * 10 + d,
                    3 => cur.region = cur.region * 10 + d,
                    4 => cur.cents = cur.cents * 10 + d,
                    _ => cur.ms = cur.ms * 10 + d,
                }
            }
        }
    }
    let n = rows.len();
    let parse_us = t0.elapsed().as_micros();

    let t0 = Instant::now();
    let mut groups: HashMap<i64, [i64; 4]> = HashMap::with_capacity(65_536);
    let mut kc = [0i64; 8];
    let mut rc = [0i64; 6];
    let mut slow = 0i64;
    for r in &rows {
        let g = groups.entry(r.uid).or_insert([r.uid, 0, 0, 0]);
        g[1] += 1;
        g[2] += r.cents;
        if r.ms > g[3] {
            g[3] = r.ms;
        }
        kc[r.kind as usize] += 1;
        rc[r.region as usize] += r.cents;
        if r.ms > 1000 {
            slow += 1;
        }
    }
    let agg_us = t0.elapsed().as_micros();

    let t0 = Instant::now();
    let mut gs: Vec<[i64; 4]> = groups.values().copied().collect();
    gs.sort_by(|a, b| b[2].cmp(&a[2]).then(a[0].cmp(&b[0])));
    let mut top = 0i64;
    for g in gs.iter().take(5) {
        top = (top * 1_000_003 + g[0] * 31 + g[2]) % MOD;
    }

    let mut json_bytes = 0i64;
    let mut json_cs = 0i64;
    let mut s = String::with_capacity(256);
    let mut i = 0usize;
    while i < n {
        let r = rows[i];
        s.clear();
        s.push_str(&format!(
            "{{\"ts\":{},\"uid\":{},\"kind\":{},\"region\":{},\"cents\":{},\"ms\":{}",
            r.ts, r.uid, r.kind, r.region, r.cents, r.ms
        ));
        if r.cents > 15000 {
            s.push_str(&format!(",\"big\":{}", r.cents * 2));
        }
        s.push('}');
        json_bytes += s.len() as i64;
        json_cs = (json_cs * 131 + *s.as_bytes().last().unwrap() as i64) % MOD;
        i += 16;
    }
    let json_us = t0.elapsed().as_micros();

    let t0 = Instant::now();
    let chunk = (n + threads - 1) / threads;
    let mut parts = vec![0i64; threads];
    std::thread::scope(|sc| {
        for (t, slot) in parts.iter_mut().enumerate() {
            let rows = &rows;
            sc.spawn(move || {
                let lo = t * chunk;
                let hi = std::cmp::min(lo + chunk, n);
                let mut acc = 0i64;
                for _ in 0..8 {
                    for r in &rows[lo..hi] {
                        acc += r.cents * (r.ms + 1);
                    }
                }
                acc %= MOD;
                *slot = acc;
            });
        }
    });
    let total: i64 = parts.iter().fold(0i64, |a, v| (a + v) % MOD);
    let par_us = t0.elapsed().as_micros();

    let mut hcs = 0i64;
    for v in kc.iter() {
        hcs = (hcs * 7 + v) % MOD;
    }
    for v in rc.iter() {
        hcs = (hcs * 7 + v) % MOD;
    }

    println!(
        "rows={} slow={} top={} hist={} json_bytes={} json_cs={} par={}",
        n, slow, top, hcs, json_bytes, json_cs, total
    );
    println!(
        "parse_us={} agg_us={} json_us={} par_us={}",
        parse_us, agg_us, json_us, par_us
    );
}

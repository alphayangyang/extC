// bench/realsvc/rust/main.rs — the Rust arm of the realsvc benchmark.
//
// Four workloads over one dataset, one checksum contract (see ../SPEC.md):
//   etl       single-threaded parse + aggregate
//   sessions  session store with a short TTL, one Box<Event> per record
//   serve     single-threaded epoll query server
//   parmix    parallel scoring pass (t threads, one checksum)
//
// Plain std only, no crates. The one piece std does not expose is epoll, so
// `serve` declares the three epoll syscalls; everything else (sockets, files,
// threads, containers) is std.
use std::collections::HashMap;
use std::io::{ErrorKind, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::fd::{AsRawFd, RawFd};
use std::time::Instant;

const FNV_BASIS: u64 = 14695981039346656037;
const FNV_PRIME: u64 = 1099511628211;
const SITES: usize = 16;
const STATUSES: usize = 9;
const STATUS_TABLE: [i64; STATUSES] = [200, 201, 204, 301, 400, 403, 404, 500, 502];

#[inline]
fn fnv(h: u64, v: u64) -> u64 {
    (h ^ v).wrapping_mul(FNV_PRIME)
}

#[inline]
fn ms_between(a: Instant, b: Instant) -> f64 {
    b.duration_since(a).as_secs_f64() * 1000.0
}

/// Read the whole file with ordinary buffered I/O.
fn read_file(path: &str) -> Vec<u8> {
    let mut f = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("open {}: {}", path, e);
            std::process::exit(1);
        }
    };
    let cap = f.metadata().map(|m| m.len() as usize + 1).unwrap_or(0);
    let mut buf = Vec::with_capacity(cap);
    if let Err(e) = f.read_to_end(&mut buf) {
        eprintln!("read {}: {}", path, e);
        std::process::exit(1);
    }
    buf
}

// ------------------------------------------------------------------- record

struct Record {
    ts: i64,
    uid: i64,
    site: i64,
    #[allow(dead_code)] // parsed and kept, but no workload aggregates it
    path: i64,
    status: i64,
    lat: i64,
    bytes: i64,
}

/// Parse the seven TAB-separated fields at `p`; returns the record and the
/// byte just past its '\n'.
#[inline]
fn parse_record(buf: &[u8], mut p: usize) -> (Record, usize) {
    let end = buf.len();
    let mut f = [0i64; 7];
    let mut k = 0;
    while k < 7 {
        let mut neg = false;
        if p < end && buf[p] == b'-' {
            neg = true;
            p += 1;
        }
        let mut v: i64 = 0;
        while p < end && buf[p].is_ascii_digit() {
            v = v * 10 + (buf[p] - b'0') as i64;
            p += 1;
        }
        f[k] = if neg { -v } else { v };
        if p < end {
            p += 1; // TAB, or '\n' after the last field
        }
        k += 1;
    }
    (
        Record { ts: f[0], uid: f[1], site: f[2], path: f[3], status: f[4], lat: f[5], bytes: f[6] },
        p,
    )
}

// ---------------------------------------------------------------------- etl

#[derive(Default, Clone, Copy)]
struct SiteAgg {
    count: i64,
    sum_lat: i64,
    max_lat: i64,
    sum_bytes: i64,
}

struct Etl {
    sites: [SiteAgg; SITES],
    status_count: [i64; STATUSES],
    lines: i64,
}

fn compute_etl(buf: &[u8]) -> Etl {
    let mut e = Etl { sites: [SiteAgg::default(); SITES], status_count: [0; STATUSES], lines: 0 };
    let mut p = 0usize;
    let end = buf.len();
    while p < end {
        let (r, np) = parse_record(buf, p);
        p = np;
        let a = &mut e.sites[r.site as usize];
        a.count += 1;
        a.sum_lat += r.lat;
        if r.lat > a.max_lat {
            a.max_lat = r.lat;
        }
        a.sum_bytes += r.bytes;
        for k in 0..STATUSES {
            if STATUS_TABLE[k] == r.status {
                e.status_count[k] += 1;
                break;
            }
        }
        e.lines += 1;
    }
    e
}

fn etl_checksum(e: &Etl) -> u64 {
    let mut h = FNV_BASIS;
    for s in 0..SITES {
        h = fnv(h, e.sites[s].count as u64);
        h = fnv(h, e.sites[s].sum_lat as u64);
        h = fnv(h, e.sites[s].max_lat as u64);
        h = fnv(h, e.sites[s].sum_bytes as u64);
    }
    for k in 0..STATUSES {
        h = fnv(h, e.status_count[k] as u64);
    }
    fnv(h, e.lines as u64)
}

fn work_etl(path: &str) -> i32 {
    let t0 = Instant::now();
    let buf = read_file(path);
    let t1 = Instant::now();
    let e = compute_etl(&buf);
    let t2 = Instant::now();
    let cs = etl_checksum(&e);
    let mut sum_lat: i64 = 0;
    let mut max_lat: i64 = 0;
    let mut sum_bytes: i64 = 0;
    for s in 0..SITES {
        sum_lat += e.sites[s].sum_lat;
        if e.sites[s].max_lat > max_lat {
            max_lat = e.sites[s].max_lat;
        }
        sum_bytes += e.sites[s].sum_bytes;
    }
    println!(
        "etl n={} read_ms={:.1} work_ms={:.1} cs={} count={} sumlat={} maxlat={} sumbytes={}",
        e.lines,
        ms_between(t0, t1),
        ms_between(t1, t2),
        cs,
        e.lines,
        sum_lat,
        max_lat,
        sum_bytes
    );
    0
}

// ----------------------------------------------------------------- sessions
//
// Two object lifetimes, on purpose: one `Box<Event>` materialised per record
// and dropped right away, and one `Box<Session>` per uid, created on first
// sight and removed when the TTL window drops it.

#[derive(Default)]
struct Session {
    last_ts: i64,
    count: i64,
    sum_idx: i64,
    sum_lat: i64,
}

fn work_sessions(path: &str) -> i32 {
    let t0 = Instant::now();
    let buf = read_file(path);
    let t1 = Instant::now();
    let mut sessions: HashMap<i64, Box<Session>> = HashMap::with_capacity(400_000);
    let end = buf.len();
    let mut p = 0usize;
    let mut lines: i64 = 0;
    let mut newest: i64 = 0;
    while p < end {
        let (rec, np) = parse_record(&buf, p);
        p = np;
        let ev = Box::new(rec);
        {
            let slot = sessions.entry(ev.uid).or_insert_with(|| Box::new(Session::default()));
            slot.count += 1;
            slot.sum_idx += lines;
            slot.sum_lat += ev.lat;
            slot.last_ts = ev.ts;
        }
        if ev.ts > newest {
            newest = ev.ts;
        }
        drop(ev);
        lines += 1;
    }
    let cutoff = newest - 300;
    // Evict: the entry is removed and its Box<Session> dropped here.
    sessions.retain(|_, s| s.last_ts >= cutoff);
    let mut keep: Vec<i64> = sessions.keys().copied().collect();
    keep.sort_unstable();
    let mut h = FNV_BASIS;
    let mut retained: i64 = 0;
    let mut liveoff: i64 = 0;
    for uid in keep {
        let s = &sessions[&uid];
        retained += 1;
        liveoff += s.count;
        h = fnv(h, uid as u64);
        h = fnv(h, s.count as u64);
        h = fnv(h, s.sum_idx as u64);
        h = fnv(h, s.sum_lat as u64);
    }
    let t2 = Instant::now();
    println!(
        "sessions n={} read_ms={:.1} work_ms={:.1} cs={} retained={} liveoff={}",
        lines,
        ms_between(t0, t1),
        ms_between(t1, t2),
        h,
        retained,
        liveoff
    );
    0
}

// -------------------------------------------------------------------- serve

// The four epoll syscall declarations: std::net gives sockets, but no readiness
// API. Values are the Linux ABI constants.
const EPOLL_CTL_ADD: i32 = 1;
const EPOLL_CTL_DEL: i32 = 2;
const EPOLL_CTL_MOD: i32 = 3;
const EPOLLIN: u32 = 0x1;
const EPOLLOUT: u32 = 0x4;

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct EpollEvent {
    events: u32,
    data: u64,
}

extern "C" {
    fn epoll_create1(flags: i32) -> i32;
    fn epoll_ctl(epfd: i32, op: i32, fd: i32, event: *mut EpollEvent) -> i32;
    fn epoll_wait(epfd: i32, events: *mut EpollEvent, maxevents: i32, timeout: i32) -> i32;
    fn close(fd: i32) -> i32;
}

/// Copy the kernel's `data` word out of a packed event (no reference may be
/// taken to a packed field).
#[inline]
fn event_data(e: &EpollEvent) -> u64 {
    unsafe { std::ptr::read_unaligned(std::ptr::addr_of!(e.data)) }
}

fn set_epoll(epfd: i32, op: i32, fd: RawFd, events: Option<u32>) {
    let mut ev = EpollEvent { events: events.unwrap_or(0), data: fd as u64 };
    let ptr = if events.is_some() { &mut ev as *mut EpollEvent } else { std::ptr::null_mut() };
    unsafe {
        epoll_ctl(epfd, op, fd, ptr);
    }
}

/// One connection = one entry in the epoll set plus two buffers.
struct Conn {
    stream: TcpStream,
    fd: RawFd,
    want: u32,
    inbuf: Vec<u8>,
    outbuf: Vec<u8>,
    out_off: usize,
    consumed: usize,
    eof: bool,
}

impl Conn {
    fn new(stream: TcpStream) -> Conn {
        let fd = stream.as_raw_fd();
        Conn {
            stream,
            fd,
            want: 0,
            inbuf: Vec::with_capacity(1024),
            outbuf: Vec::with_capacity(1024),
            out_off: 0,
            consumed: 0,
            eof: false,
        }
    }
}

/// Append `v` in decimal to `buf` at `pos`.
#[inline]
fn push_i64(buf: &mut [u8; 96], mut pos: usize, v: i64) -> usize {
    let mut tmp = [0u8; 20];
    let mut n = 0;
    let mut x = v as u64;
    if x == 0 {
        tmp[0] = b'0';
        n = 1;
    } else {
        while x > 0 {
            tmp[n] = b'0' + (x % 10) as u8;
            x /= 10;
            n += 1;
        }
    }
    while n > 0 {
        n -= 1;
        buf[pos] = tmp[n];
        pos += 1;
    }
    pos
}

struct Server<'a> {
    listen: TcpListener,
    epfd: i32,
    etl: &'a Etl,
    read_ms: f64,
    lines: i64,
    start: Option<Instant>,
    served: i64,
    conns: HashMap<RawFd, Conn>,
}

impl<'a> Server<'a> {
    fn new(port: i32, etl: &'a Etl, read_ms: f64, lines: i64) -> Server<'a> {
        let addr = format!("127.0.0.1:{}", port);
        let listen = match TcpListener::bind(&addr) {
            Ok(l) => l,
            Err(e) => {
                eprintln!("bind {}: {}", addr, e);
                std::process::exit(2);
            }
        };
        if listen.set_nonblocking(true).is_err() {
            eprintln!("listen: cannot set non-blocking");
            std::process::exit(2);
        }
        let epfd = unsafe { epoll_create1(0) };
        if epfd < 0 {
            eprintln!("epoll_create1 failed");
            std::process::exit(2);
        }
        let lfd = listen.as_raw_fd();
        set_epoll(epfd, EPOLL_CTL_ADD, lfd, Some(EPOLLIN));
        Server {
            listen,
            epfd,
            etl,
            read_ms,
            lines,
            start: None,
            served: 0,
            conns: HashMap::with_capacity(128),
        }
    }

    fn run(&mut self) {
        println!("serve_ready");
        let _ = std::io::stdout().flush();
        let mut events = [EpollEvent { events: 0, data: 0 }; 128];
        loop {
            let n = unsafe { epoll_wait(self.epfd, events.as_mut_ptr(), 128, 1000) };
            if n < 0 {
                if std::io::Error::last_os_error().kind() == ErrorKind::Interrupted {
                    continue;
                }
                break;
            }
            for i in 0..n as usize {
                let raw = event_data(&events[i]);
                let lfd = self.listen.as_raw_fd();
                if raw as i32 == lfd {
                    self.accept_all();
                    continue;
                }
                let fd = raw as RawFd;
                if !self.conns.contains_key(&fd) {
                    continue;
                }
                if !self.drain_in(fd) || !self.serve_lines(fd) {
                    self.drop_conn(fd);
                    continue;
                }
                if !self.flush_out(fd) {
                    self.drop_conn(fd);
                    continue;
                }
                {
                    let c = &self.conns[&fd];
                    if c.eof && c.outbuf.is_empty() {
                        self.drop_conn(fd);
                        continue;
                    }
                }
                self.update_interest(fd);
            }
            if self.conns.is_empty() && self.served > 0 {
                break;
            }
        }
        let ms = match self.start {
            Some(s) => ms_between(s, Instant::now()),
            None => 0.0,
        };
        let mut h = FNV_BASIS;
        for s in 0..SITES {
            h = fnv(h, s as u64);
            h = fnv(h, self.etl.sites[s].count as u64);
            h = fnv(h, self.etl.sites[s].sum_lat as u64);
            h = fnv(h, self.etl.sites[s].max_lat as u64);
            h = fnv(h, self.etl.sites[s].sum_bytes as u64);
        }
        println!(
            "serve n={} read_ms={:.1} req={} secs={:.1} cs={}",
            self.lines, self.read_ms, self.served, ms, h
        );
        let _ = std::io::stdout().flush();
        self.close_all();
    }

    fn close_all(&mut self) {
        self.conns.clear();
        unsafe {
            close(self.epfd);
        }
        self.epfd = -1;
    }

    fn accept_all(&mut self) {
        loop {
            match self.listen.accept() {
                Ok((stream, _)) => {
                    let _ = stream.set_nonblocking(true);
                    let _ = stream.set_nodelay(true);
                    let conn = Conn::new(stream);
                    let fd = conn.fd;
                    set_epoll(self.epfd, EPOLL_CTL_ADD, fd, Some(EPOLLIN));
                    self.conns.insert(fd, Conn { want: EPOLLIN, ..conn });
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock => return,
                Err(_) => return,
            }
        }
    }

    /// Returns false when the connection is broken and must be dropped.
    fn drain_in(&mut self, fd: RawFd) -> bool {
        let mut buf = [0u8; 16384];
        let c = self.conns.get_mut(&fd).unwrap();
        loop {
            match c.stream.read(&mut buf) {
                Ok(0) => {
                    c.eof = true;
                    return true;
                }
                Ok(got) => {
                    c.inbuf.extend_from_slice(&buf[..got]);
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock => return true,
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(_) => return false,
            }
        }
    }

    /// Consume every complete line, appending one reply per line. Always true:
    /// a request that cannot be read answers from site 0, like the C++ arm.
    fn serve_lines(&mut self, fd: RawFd) -> bool {
        let etl = self.etl;
        let c = self.conns.get_mut(&fd).unwrap();
        let mut line = [0u8; 96];
        loop {
            let mut nl = None;
            for k in c.consumed..c.inbuf.len() {
                if c.inbuf[k] == b'\n' {
                    nl = Some(k);
                    break;
                }
            }
            let nl = match nl {
                Some(k) => k,
                None => break,
            };
            if self.start.is_none() {
                self.start = Some(Instant::now());
            }
            let mut site = 0usize;
            let mut ok = nl >= c.consumed + 2 && c.inbuf[c.consumed] == b'G';
            let mut k = c.consumed + 2;
            while ok && k < nl {
                let ch = c.inbuf[k];
                if !ch.is_ascii_digit() {
                    ok = false;
                    break;
                }
                site = site * 10 + (ch - b'0') as usize;
                k += 1;
            }
            let a = &etl.sites[if ok && site < SITES { site } else { 0 }];
            let mut pos = 0usize;
            pos = push_i64(&mut line, pos, a.count);
            line[pos] = b' ';
            pos += 1;
            pos = push_i64(&mut line, pos, a.sum_lat);
            line[pos] = b' ';
            pos += 1;
            pos = push_i64(&mut line, pos, a.max_lat);
            line[pos] = b' ';
            pos += 1;
            pos = push_i64(&mut line, pos, a.sum_bytes);
            line[pos] = b'\n';
            pos += 1;
            c.outbuf.extend_from_slice(&line[..pos]);
            c.consumed = nl + 1;
            self.served += 1;
        }
        if c.consumed == c.inbuf.len() {
            c.inbuf.clear();
            c.consumed = 0;
        } else if c.consumed > (1 << 17) {
            c.inbuf.drain(..c.consumed);
            c.consumed = 0;
        }
        true
    }

    /// Returns false when the write failed in a way that kills the connection.
    fn flush_out(&mut self, fd: RawFd) -> bool {
        let c = self.conns.get_mut(&fd).unwrap();
        while c.out_off < c.outbuf.len() {
            match c.stream.write(&c.outbuf[c.out_off..]) {
                Ok(0) => break,
                Ok(n) => c.out_off += n,
                Err(e) if e.kind() == ErrorKind::WouldBlock => break,
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(_) => return false,
            }
        }
        if c.out_off > 0 {
            c.outbuf.drain(..c.out_off);
            c.out_off = 0;
        }
        true
    }

    fn update_interest(&mut self, fd: RawFd) {
        let c = self.conns.get_mut(&fd).unwrap();
        let mut want: u32 = if c.eof { 0 } else { EPOLLIN };
        if !c.outbuf.is_empty() {
            want |= EPOLLOUT;
        }
        if want == 0 || want == c.want {
            return;
        }
        set_epoll(self.epfd, EPOLL_CTL_MOD, fd, Some(want));
        c.want = want;
    }

    fn drop_conn(&mut self, fd: RawFd) {
        set_epoll(self.epfd, EPOLL_CTL_DEL, fd, None);
        // Dropping the TcpStream closes the descriptor.
        self.conns.remove(&fd);
    }
}

fn work_serve(path: &str, port: i32) -> i32 {
    let t0 = Instant::now();
    let buf = read_file(path);
    let t1 = Instant::now();
    let e = compute_etl(&buf);
    let mut srv = Server::new(port, &e, ms_between(t0, t1), e.lines);
    srv.run();
    0
}

// ------------------------------------------------------------------- parmix

#[inline]
fn mix_record(r: &Record) -> u64 {
    let mut x = (r.ts as u64) ^ ((r.uid as u64) << 17) ^ ((r.lat as u64) << 33) ^ (r.bytes as u64);
    for _ in 0..64 {
        x ^= x >> 33;
        x = x.wrapping_mul(0xff51afd7ed558ccd);
        x ^= x >> 29;
        x = x.wrapping_mul(0xc4ceb9fe1a85ec53);
    }
    x
}

fn work_parmix(path: &str, threads: usize) -> i32 {
    let t0 = Instant::now();
    let buf = read_file(path);
    let t1 = Instant::now();
    // Offsets are sequential work: each worker parses only its own records.
    let mut offs: Vec<u32> = Vec::with_capacity(buf.len() / 40 + 16);
    {
        let end = buf.len();
        let mut p = 0usize;
        while p < end {
            offs.push(p as u32);
            while p < end && buf[p] != b'\n' {
                p += 1;
            }
            if p < end {
                p += 1;
            }
        }
    }
    let n = offs.len();
    let mut out = vec![0u64; n];
    let t = if threads < 1 { 1 } else { threads };
    let work = |lo: usize, slice: &mut [u64]| {
        for (k, o) in slice.iter_mut().enumerate() {
            let (r, _) = parse_record(&buf, offs[lo + k] as usize);
            *o = mix_record(&r);
        }
    };
    if t == 1 {
        work(0, &mut out);
    } else {
        let chunk = n.div_ceil(t);
        std::thread::scope(|sc| {
            let mut lo = 0usize;
            for slice in out.chunks_mut(chunk) {
                let at = lo;
                lo += slice.len();
                sc.spawn(move || work(at, slice));
            }
        });
    }
    let t2 = Instant::now();
    let mut h = FNV_BASIS;
    let mut sum: u64 = 0;
    for v in &out {
        sum = sum.wrapping_add(*v);
        h = fnv(h, *v);
    }
    h = fnv(h, n as u64);
    println!(
        "parmix n={} t={} read_ms={:.1} work_ms={:.1} cs={} sum={}",
        n,
        t,
        ms_between(t0, t1),
        ms_between(t1, t2),
        h,
        sum
    );
    0
}

// --------------------------------------------------------------------- main

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!(
            "usage: realsvc_rust <etl|sessions|serve|parmix> <log.tsv> [args]\n\
             \x20 serve  <port> [requests]\n\
             \x20 parmix [threads]"
        );
        std::process::exit(2);
    }
    let mode = args[1].as_str();
    let path = args[2].as_str();
    let code = match mode {
        "etl" => work_etl(path),
        "sessions" => work_sessions(path),
        "serve" => {
            let port: i32 = if args.len() > 3 { args[3].parse().unwrap_or(9100) } else { 9100 };
            work_serve(path, port)
        }
        "parmix" => {
            let threads: usize = if args.len() > 3 { args[3].parse().unwrap_or(8) } else { 8 };
            work_parmix(path, threads)
        }
        _ => {
            eprintln!("unknown mode {}", mode);
            2
        }
    };
    if code != 0 {
        std::process::exit(code);
    }
    let _ = std::io::stdout().flush();
}

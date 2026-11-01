// bench/realsvc/cpp/main.cpp — the C++ arm of the realsvc benchmark.
//
// Four workloads, one dataset, one checksum contract (see ../SPEC.md):
//   etl       single-threaded parse + aggregate
//   sessions  session store with a one-hour TTL
//   serve     single-threaded epoll query server
//   parmix    parallel scoring pass (t threads, one checksum)
//
// Plain C++17 and pthreads only: no allocator tricks, no intrinsics, no
// dependencies. The point is what a working C++ service would do.
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint64_t kFnvBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr int kSites = 16;
constexpr int kStatuses = 9;
constexpr int kStatusTable[kStatuses] = {200, 201, 204, 301, 400, 403, 404, 500, 502};
constexpr long kUids = 200000;

inline uint64_t fnv(uint64_t h, uint64_t v) { return (h ^ v) * kFnvPrime; }

double nowMs() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

// Read the whole file with ordinary buffered I/O, then hand the buffer back.
std::string readFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::perror("open");
        std::exit(1);
    }
    std::string buf;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        long n = std::ftell(f);
        if (n > 0) buf.reserve(static_cast<size_t>(n));
        std::rewind(f);
    }
    char chunk[1 << 16];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) buf.append(chunk, got);
    std::fclose(f);
    return buf;
}

struct Record {
    long long ts, uid, site, path, status, lat, bytes;
};

// Parse the record whose first byte is at p; returns the byte just past its '\n'.
inline const char* parseRecord(const char* p, const char* end, Record& r) {
    long long* fields[7] = {&r.ts, &r.uid, &r.site, &r.path, &r.status, &r.lat, &r.bytes};
    for (int f = 0; f < 7; f++) {
        bool neg = false;
        if (p < end && *p == '-') {
            neg = true;
            p++;
        }
        long long v = 0;
        while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        *fields[f] = neg ? -v : v;
        if (p < end) p++;  // TAB, or '\n' after the last field
    }
    return p;
}

struct SiteAgg {
    long long count, sumLat, maxLat, sumBytes;
};

struct Etl {
    SiteAgg sites[kSites]{};
    long long statusCount[kStatuses]{};
    long long lines = 0;
};

Etl computeEtl(const std::string& buf) {
    Etl e;
    const char* p = buf.data();
    const char* end = p + buf.size();
    while (p < end) {
        Record r;
        p = parseRecord(p, end, r);
        SiteAgg& a = e.sites[r.site];
        a.count++;
        a.sumLat += r.lat;
        a.maxLat = std::max(a.maxLat, r.lat);
        a.sumBytes += r.bytes;
        for (int k = 0; k < kStatuses; k++) {
            if (kStatusTable[k] == r.status) {
                e.statusCount[k]++;
                break;
            }
        }
        e.lines++;
    }
    return e;
}

uint64_t etlChecksum(const Etl& e) {
    uint64_t h = kFnvBasis;
    for (int s = 0; s < kSites; s++) {
        h = fnv(h, static_cast<uint64_t>(e.sites[s].count));
        h = fnv(h, static_cast<uint64_t>(e.sites[s].sumLat));
        h = fnv(h, static_cast<uint64_t>(e.sites[s].maxLat));
        h = fnv(h, static_cast<uint64_t>(e.sites[s].sumBytes));
    }
    for (int k = 0; k < kStatuses; k++) h = fnv(h, static_cast<uint64_t>(e.statusCount[k]));
    return fnv(h, static_cast<uint64_t>(e.lines));
}

int workEtl(const char* path) {
    double t0 = nowMs();
    std::string buf = readFile(path);
    double t1 = nowMs();
    Etl e = computeEtl(buf);
    double t2 = nowMs();
    uint64_t cs = etlChecksum(e);
    long long sumLat = 0, maxLat = 0, sumBytes = 0;
    for (int s = 0; s < kSites; s++) {
        sumLat += e.sites[s].sumLat;
        maxLat = std::max(maxLat, e.sites[s].maxLat);
        sumBytes += e.sites[s].sumBytes;
    }
    std::printf("etl n=%lld read_ms=%.1f work_ms=%.1f cs=%llu count=%lld sumlat=%lld maxlat=%lld sumbytes=%lld\n",
                e.lines, t1 - t0, t2 - t1, (unsigned long long)cs, e.lines, sumLat, maxLat, sumBytes);
    return 0;
}

// Workload B. Two object lifetimes: a message object per record (materialised,
// used, then deleted) and a session object per uid (created on first sight,
// deleted when the TTL window drops it).
struct Session {
    long long lastTs = 0;
    long long count = 0;
    long long sumIdx = 0;
    long long sumLat = 0;
};

int workSessions(const char* path) {
    double t0 = nowMs();
    std::string buf = readFile(path);
    double t1 = nowMs();
    std::unordered_map<long long, Session*> sessions;
    sessions.reserve(kUids * 2);
    const char* p = buf.data();
    const char* end = p + buf.size();
    long long lines = 0;
    long long newest = 0;
    while (p < end) {
        Record* ev = new Record();
        p = parseRecord(p, end, *ev);
        Session*& slot = sessions[ev->uid];
        if (!slot) {
            slot = new Session();
            slot->lastTs = ev->ts;
        }
        slot->count++;
        slot->sumIdx += lines;
        slot->sumLat += ev->lat;
        slot->lastTs = ev->ts;
        newest = std::max(newest, ev->ts);
        delete ev;
        lines++;
    }
    long long cutoff = newest - 300;
    uint64_t h = kFnvBasis;
    long long retained = 0, liveoff = 0;
    for (auto it = sessions.begin(); it != sessions.end();) {
        if (it->second->lastTs < cutoff) {
            delete it->second;
            it = sessions.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<long long> keep;
    keep.reserve(sessions.size());
    for (const auto& kv : sessions) keep.push_back(kv.first);
    std::sort(keep.begin(), keep.end());
    for (long long uid : keep) {
        const Session* s = sessions[uid];
        retained++;
        liveoff += s->count;
        h = fnv(h, static_cast<uint64_t>(uid));
        h = fnv(h, static_cast<uint64_t>(s->count));
        h = fnv(h, static_cast<uint64_t>(s->sumIdx));
        h = fnv(h, static_cast<uint64_t>(s->sumLat));
    }
    for (auto& kv : sessions) delete kv.second;
    double t2 = nowMs();
    std::printf("sessions n=%lld read_ms=%.1f work_ms=%.1f cs=%llu retained=%lld liveoff=%lld\n", lines,
                t1 - t0, t2 - t1, (unsigned long long)h, retained, liveoff);
    return 0;
}

// ---------------------------------------------------------------------- serve

// One connection = one entry in the epoll set plus two buffers. The state
// machine is deliberately plain: append what arrived, consume whole lines,
// queue the replies, write what the socket accepts.
struct Conn {
    int fd = -1;
    uint32_t want = 0;
    std::string in;
    std::string out;
    size_t consumed = 0;
    bool eof = false;
};

class Server {
  public:
    Server(int port, const Etl& e, double readMs, long long lines)
        : port_(port), readMs_(readMs), lines_(lines), etl_(e) {}

    void run() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) { std::perror("socket"); std::exit(2); }
        int one = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            std::perror("bind"); std::exit(2);
        }
        if (::listen(listenFd_, 1024) != 0) { std::perror("listen"); std::exit(2); }
        setNonBlock(listenFd_);
        epollFd_ = ::epoll_create1(0);
        addFd(listenFd_, EPOLLIN);
        std::printf("serve_ready\n");
        std::fflush(stdout);

        epoll_event events[128];
        while (true) {
            int n = ::epoll_wait(epollFd_, events, 128, 1000);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;
                if (fd == listenFd_) { acceptAll(); continue; }
                Conn* c = findConn(fd);
                if (!c) continue;
                if (!drainIn(c)) { dropConn(fd); continue; }
                serveLines(c);
                if (!flushOut(c)) { dropConn(fd); continue; }
                if (c->eof && c->out.empty()) { dropConn(fd); continue; }
                updateInterest(c);
            }
            if (conns_.empty() && served_ > 0) break;
        }
        double ms = startMs_ < 0 ? 0.0 : nowMs() - startMs_;
        uint64_t h = kFnvBasis;
        for (int s = 0; s < kSites; s++) {
            h = fnv(h, static_cast<uint64_t>(s));
            h = fnv(h, static_cast<uint64_t>(etl_.sites[s].count));
            h = fnv(h, static_cast<uint64_t>(etl_.sites[s].sumLat));
            h = fnv(h, static_cast<uint64_t>(etl_.sites[s].maxLat));
            h = fnv(h, static_cast<uint64_t>(etl_.sites[s].sumBytes));
        }
        std::printf("serve n=%lld read_ms=%.1f req=%lld secs=%.1f cs=%llu\n", lines_, readMs_, served_, ms,
                    (unsigned long long)h);
        std::fflush(stdout);
        closeAll();
    }

  private:
    void closeAll() {
        for (auto& kv : conns_) ::close(kv.first);
        conns_.clear();
        if (epollFd_ >= 0) ::close(epollFd_);
        if (listenFd_ >= 0) ::close(listenFd_);
    }
    void addFd(int fd, uint32_t ev) {
        epoll_event e{};
        e.events = ev;
        e.data.fd = fd;
        ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &e);
    }
    Conn* findConn(int fd) {
        auto it = conns_.find(fd);
        return it == conns_.end() ? nullptr : &it->second;
    }
    static void setNonBlock(int fd) {
        int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    void acceptAll() {
        while (true) {
            int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd < 0) return;
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof one);
            setNonBlock(fd);
            Conn c;
            c.fd = fd;
            c.want = EPOLLIN;
            conns_.emplace(fd, std::move(c));
            addFd(fd, EPOLLIN);
        }
    }
    // Returns false when the connection is broken and must be dropped.
    bool drainIn(Conn* c) {
        char buf[16384];
        while (true) {
            ssize_t got = ::read(c->fd, buf, sizeof buf);
            if (got > 0) {
                c->in.append(buf, static_cast<size_t>(got));
                continue;
            }
            if (got == 0) { c->eof = true; return true; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return false;
        }
    }
    void serveLines(Conn* c) {
        char line[96];
        while (true) {
            size_t nl = c->in.find('\n', c->consumed);
            if (nl == std::string::npos) break;
            if (startMs_ < 0) startMs_ = nowMs();
            int site = 0;
            bool ok = nl - c->consumed >= 2 && c->in[c->consumed] == 'G';
            for (size_t k = c->consumed + 2; ok && k < nl; k++) {
                char ch = c->in[k];
                if (ch < '0' || ch > '9') { ok = false; break; }
                site = site * 10 + (ch - '0');
            }
            const SiteAgg& a = etl_.sites[ok && site < kSites ? site : 0];
            int n = std::snprintf(line, sizeof line, "%lld %lld %lld %lld\n", a.count, a.sumLat, a.maxLat,
                                  a.sumBytes);
            c->out.append(line, static_cast<size_t>(n));
            c->consumed = nl + 1;
            served_++;
        }
        if (c->consumed == c->in.size()) {
            c->in.clear();
            c->consumed = 0;
        } else if (c->consumed > (1u << 17)) {
            c->in.erase(0, c->consumed);
            c->consumed = 0;
        }
    }
    // Returns false when the write failed in a way that kills the connection.
    bool flushOut(Conn* c) {
        size_t off = 0;
        while (off < c->out.size()) {
            ssize_t n = ::write(c->fd, c->out.data() + off, c->out.size() - off);
            if (n > 0) { off += static_cast<size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        c->out.erase(0, off);
        return true;
    }
    void updateInterest(Conn* c) {
        uint32_t want = c->eof ? 0u : static_cast<uint32_t>(EPOLLIN);
        if (!c->out.empty()) want |= EPOLLOUT;
        if (want == 0) return;
        if (want == c->want) return;
        epoll_event e{};
        e.events = want;
        e.data.fd = c->fd;
        ::epoll_ctl(epollFd_, EPOLL_CTL_MOD, c->fd, &e);
        c->want = want;
    }
    void dropConn(int fd) {
        ::close(fd);
        ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
        conns_.erase(fd);
    }

    int port_ = 0;
    int listenFd_ = -1;
    int epollFd_ = -1;
    double readMs_ = 0;
    long long lines_ = 0;
    double startMs_ = -1;
    long long served_ = 0;
    const Etl& etl_;
    std::unordered_map<int, Conn> conns_;
};

int workServe(const char* path, int port) {
    double t0 = nowMs();
    std::string buf = readFile(path);
    double t1 = nowMs();
    Etl e = computeEtl(buf);
    Server srv(port, e, t1 - t0, e.lines);
    srv.run();
    return 0;
}

// --------------------------------------------------------------------- parmix

inline uint64_t mixRecord(const Record& r) {
    uint64_t x = static_cast<uint64_t>(r.ts) ^ (static_cast<uint64_t>(r.uid) << 17) ^
                 (static_cast<uint64_t>(r.lat) << 33) ^ static_cast<uint64_t>(r.bytes);
    for (int k = 0; k < 64; k++) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 29;
        x *= 0xc4ceb9fe1a85ec53ULL;
    }
    return x;
}

int workParmix(const char* path, int threads) {
    double t0 = nowMs();
    std::string buf = readFile(path);
    double t1 = nowMs();
    // Offsets are sequential work: each worker must only parse its own records.
    std::vector<uint32_t> offs;
    offs.reserve(buf.size() / 40 + 16);
    {
        const char* p = buf.data();
        const char* end = p + buf.size();
        while (p < end) {
            offs.push_back(static_cast<uint32_t>(p - buf.data()));
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
        }
    }
    size_t n = offs.size();
    std::vector<uint64_t> out(n);
    if (threads < 1) threads = 1;
    auto worker = [&](size_t lo, size_t hi) {
        for (size_t i = lo; i < hi; i++) {
            Record r;
            parseRecord(buf.data() + offs[i], buf.data() + buf.size(), r);
            out[i] = mixRecord(r);
        }
    };
    if (threads == 1) {
        worker(0, n);
    } else {
        std::vector<std::thread> pool;
        size_t chunk = (n + static_cast<size_t>(threads) - 1) / static_cast<size_t>(threads);
        for (int t = 0; t < threads; t++) {
            size_t lo = chunk * static_cast<size_t>(t);
            size_t hi = std::min(n, lo + chunk);
            if (lo >= hi) break;
            pool.emplace_back(worker, lo, hi);
        }
        for (auto& th : pool) th.join();
    }
    double t2 = nowMs();
    uint64_t h = kFnvBasis;
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += out[i];
        h = fnv(h, out[i]);
    }
    h = fnv(h, static_cast<uint64_t>(n));
    std::printf("parmix n=%zu t=%d read_ms=%.1f work_ms=%.1f cs=%llu sum=%llu\n", n, threads, t1 - t0,
                t2 - t1, (unsigned long long)h, (unsigned long long)sum);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: realsvc_cpp <etl|sessions|serve|parmix> <log.tsv> [args]\n"
                     "  serve  <port> [requests]\n"
                     "  parmix [threads]\n");
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "etl") return workEtl(argv[2]);
    if (mode == "sessions") return workSessions(argv[2]);
    if (mode == "serve") {
        int port = argc > 3 ? std::atoi(argv[3]) : 9100;
        return workServe(argv[2], port);
    }
    if (mode == "parmix") return workParmix(argv[2], argc > 3 ? std::atoi(argv[3]) : 8);
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}

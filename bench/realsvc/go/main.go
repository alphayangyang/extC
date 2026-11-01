// bench/realsvc/go/main.go — the Go arm of the realsvc benchmark.
//
// Four workloads, one dataset, one checksum contract (see ../SPEC.md):
//
//	etl       single-threaded parse + aggregate
//	sessions  session store with a one-hour TTL
//	serve     single-threaded query server (goroutine per connection)
//	parmix    parallel scoring pass (t goroutines, one checksum)
//
// Plain Go and the standard library only: no unsafe, no hand-rolled allocators,
// no tricks around the container shapes the other arms use. The point is what a
// working Go service would do.
package main

import (
	"bufio"
	"errors"
	"fmt"
	"net"
	"os"
	"runtime"
	"slices"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

const (
	fnvBasis = uint64(14695981039346656037)
	fnvPrime = uint64(1099511628211)

	nSites    = 16
	nStatuses = 9
)

var statusTable = [nStatuses]int64{200, 201, 204, 301, 400, 403, 404, 500, 502}

// fnv folds one 64-bit value into the running FNV-1a hash.
func fnv(h, v uint64) uint64 { return (h ^ v) * fnvPrime }

// processStart is the zero point for read_ms/work_ms: both are milliseconds
// since program start, the same clock the other arms use for their own phases.
var processStart = time.Now()

// nowMs returns milliseconds since processStart.
func nowMs() float64 {
	return float64(time.Since(processStart).Nanoseconds()) / 1e6
}

// readFile reads the whole file into one buffer with ordinary buffered I/O.
// Every workload parses from that buffer, so the reported read_ms covers the
// ingest only and the workload timing covers CPU work, not disk.
func readFile(path string) []byte {
	buf, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open %s: %v\n", path, err)
		os.Exit(1)
	}
	return buf
}

// Event is one parsed log record — the message object an ingest path
// materialises before it does anything with it. Fields are not individually
// meaningful to the workloads beyond site/uid/lat/ts.
type Event struct {
	ts     int64 // 0: unix seconds
	uid    int64 // 1: user id
	site   int64 // 2: [0, 15]
	path   int64 // 3: [0, 23]
	status int64 // 4: one of statusTable
	lat    int64 // 5: microseconds
	bytes  int64 // 6: [0, 65535]
}

// parseField parses one optional-sign decimal field starting at b[p] and
// returns it with the index one past the field's terminator (TAB, or '\n'
// after the last field). The pointer may land on len(b), which callers treat
// as end of input.
func parseField(b []byte, p int) (int64, int) {
	neg := false
	if p < len(b) && b[p] == '-' {
		neg = true
		p++
	}
	var x int64
	for p < len(b) {
		d := int(b[p]) - '0'
		if d < 0 || d > 9 {
			break
		}
		x = x*10 + int64(d)
		p++
	}
	if neg {
		x = -x
	}
	if p < len(b) {
		p++ // TAB, or '\n' after the last field
	}
	return x, p
}

// parseRecord parses the record whose first byte is at b[p] and returns it
// with the index just past its trailing '\n'.
func parseRecord(b []byte, p int) (Event, int) {
	var r Event
	var np int
	r.ts, np = parseField(b, p)
	p = np
	r.uid, np = parseField(b, p)
	p = np
	r.site, np = parseField(b, p)
	p = np
	r.path, np = parseField(b, p)
	p = np
	r.status, np = parseField(b, p)
	p = np
	r.lat, np = parseField(b, p)
	p = np
	r.bytes, np = parseField(b, p)
	return r, np
}

// parseInto fills *r from the record at b[p] and returns the index past it.
// Separate from parseRecord so the sessions workload can hand the fields
// straight to its per-record heap object.
func parseInto(b []byte, p int, r *Event) int {
	r.ts, p = parseField(b, p)
	r.uid, p = parseField(b, p)
	r.site, p = parseField(b, p)
	r.path, p = parseField(b, p)
	r.status, p = parseField(b, p)
	r.lat, p = parseField(b, p)
	r.bytes, p = parseField(b, p)
	return p
}

// ------------------------------------------------------------------ workload A

// siteAgg is the per-site aggregate all of A, C (and the client) agree on.
type siteAgg struct {
	count    int64
	sumLat   int64
	maxLat   int64
	sumBytes int64
}

// etlAgg is the full etl state: per-site aggregates, per-status counts and the
// line count.
type etlAgg struct {
	sites       [nSites]siteAgg
	statusCount [nStatuses]int64
	lines       int64
}

// computeEtl parses every record and folds it into the aggregate. Nothing is
// filtered: every line updates exactly one site and one status bucket.
func computeEtl(buf []byte) etlAgg {
	var e etlAgg
	p := 0
	n := len(buf)
	for p < n {
		r, np := parseRecord(buf, p)
		p = np
		a := &e.sites[r.site]
		a.count++
		a.sumLat += r.lat
		if r.lat > a.maxLat {
			a.maxLat = r.lat
		}
		a.sumBytes += r.bytes
		for k := 0; k < nStatuses; k++ {
			if statusTable[k] == r.status {
				e.statusCount[k]++
				break
			}
		}
		e.lines++
	}
	return e
}

// etlChecksum folds etlAgg in the order the contract fixes: per site count,
// sumLat, maxLat, sumBytes (s = 0..15), then the nine status counts, then the
// line count.
func etlChecksum(e *etlAgg) uint64 {
	h := fnvBasis
	for s := 0; s < nSites; s++ {
		h = fnv(h, uint64(e.sites[s].count))
		h = fnv(h, uint64(e.sites[s].sumLat))
		h = fnv(h, uint64(e.sites[s].maxLat))
		h = fnv(h, uint64(e.sites[s].sumBytes))
	}
	for k := 0; k < nStatuses; k++ {
		h = fnv(h, uint64(e.statusCount[k]))
	}
	return fnv(h, uint64(e.lines))
}

func workEtl(path string) int {
	t0 := nowMs()
	buf := readFile(path)
	t1 := nowMs()
	e := computeEtl(buf)
	t2 := nowMs()
	cs := etlChecksum(&e)
	var sumLat, maxLat, sumBytes int64
	for s := 0; s < nSites; s++ {
		sumLat += e.sites[s].sumLat
		if e.sites[s].maxLat > maxLat {
			maxLat = e.sites[s].maxLat
		}
		sumBytes += e.sites[s].sumBytes
	}
	fmt.Printf("etl n=%d read_ms=%.1f work_ms=%.1f cs=%d count=%d sumlat=%d maxlat=%d sumbytes=%d\n",
		e.lines, t1-t0, t2-t1, cs, e.lines, sumLat, maxLat, sumBytes)
	return 0
}

// ------------------------------------------------------------------ workload B

// Session is the per-user session registry entry: created on first sight,
// deleted when the TTL window drops it.
type Session struct {
	lastTs int64
	count  int64
	sumIdx int64
	sumLat int64
}

func workSessions(path string) int {
	t0 := nowMs()
	buf := readFile(path)
	t1 := nowMs()

	// The registry is created on first sight of a uid and deleted on TTL
	// eviction; the hint is the dataset's uid population, as the other arms'
	// reserve() calls are.
	sessions := make(map[int64]*Session, 400000)

	var lines, newest int64
	p := 0
	n := len(buf)
	for p < n {
		// The per-record message object: materialised, used, then dropped.
		ev := &Event{}
		p = parseInto(buf, p, ev)
		s := sessions[ev.uid]
		if s == nil {
			s = &Session{lastTs: ev.ts}
			sessions[ev.uid] = s
		}
		s.count++
		s.sumIdx += lines
		s.sumLat += ev.lat
		s.lastTs = ev.ts
		if ev.ts > newest {
			newest = ev.ts
		}
		lines++
	}

	// Evict: a session is retained when lastTs >= newest-300; the rest are
	// dropped from the registry and become garbage.
	cutoff := newest - 300
	for uid, s := range sessions {
		if s.lastTs < cutoff {
			delete(sessions, uid)
		}
	}

	// The checksum is defined over retained sessions in ascending uid order, so
	// the map's iteration order has to be sorted out before folding.
	keep := make([]int64, 0, len(sessions))
	for uid := range sessions {
		keep = append(keep, uid)
	}
	slices.Sort(keep)

	var retained, liveoff int64
	h := fnvBasis
	for _, uid := range keep {
		s := sessions[uid]
		retained++
		liveoff += s.count
		h = fnv(h, uint64(uid))
		h = fnv(h, uint64(s.count))
		h = fnv(h, uint64(s.sumIdx))
		h = fnv(h, uint64(s.sumLat))
	}
	t2 := nowMs()
	fmt.Printf("sessions n=%d read_ms=%.1f work_ms=%.1f cs=%d retained=%d liveoff=%d\n",
		lines, t1-t0, t2-t1, h, retained, liveoff)
	return 0
}

// ------------------------------------------------------------------ workload C

// serveBuf keeps the log buffer reachable for the life of the server, so the
// peak-RSS column measures the same thing as in the arms that keep their file
// buffer alive while serving.
var serveBuf []byte

// serveState is the state the connection goroutines share.
type serveState struct {
	etl     *etlAgg
	served  atomic.Int64 // replies written
	start   sync.Once    // first-reply timestamp latch
	startMs float64      // set once by the first reply
}

// serveLine answers one request line ("G <site>\n"). Anything malformed is
// answered with site 0, exactly as the other arms do, so a bad request can
// never crash the server.
func (st *serveState) serveLine(line []byte, w *bufio.Writer, scratch []byte) bool {
	ok := len(line) >= 2 && line[0] == 'G'
	site := 0
	if ok {
		for k := 2; k < len(line)-1; k++ {
			ch := line[k]
			if ch < '0' || ch > '9' {
				ok = false
				break
			}
			site = site*10 + int(ch-'0')
		}
	}
	if !ok || site < 0 || site >= nSites {
		site = 0
	}
	a := &st.etl.sites[site]
	b := scratch[:0]
	b = strconv.AppendInt(b, a.count, 10)
	b = append(b, ' ')
	b = strconv.AppendInt(b, a.sumLat, 10)
	b = append(b, ' ')
	b = strconv.AppendInt(b, a.maxLat, 10)
	b = append(b, ' ')
	b = strconv.AppendInt(b, a.sumBytes, 10)
	b = append(b, '\n')
	if _, err := w.Write(b); err != nil {
		return false
	}
	st.start.Do(func() { st.startMs = nowMs() })
	st.served.Add(1)
	return true
}

// serveConn is one goroutine per connection: consume complete lines, answer
// them, and flush once the read buffer has been drained (so replies go out in
// batches instead of one write syscall per request). Returns when the peer is
// done or the connection breaks.
func serveConn(c net.Conn, st *serveState) {
	defer c.Close()
	r := bufio.NewReaderSize(c, 1<<16)
	w := bufio.NewWriterSize(c, 1<<16)
	var scratch [128]byte
	for {
		line, err := r.ReadSlice('\n')
		if err != nil {
			if errors.Is(err, bufio.ErrBufferFull) {
				// Overlong line: drop everything up to the next newline.
				for errors.Is(err, bufio.ErrBufferFull) {
					_, err = r.ReadSlice('\n')
				}
				if err != nil {
					break
				}
				continue
			}
			break
		}
		if !st.serveLine(line, w, scratch[:]) {
			return
		}
		if r.Buffered() == 0 {
			if err := w.Flush(); err != nil {
				return
			}
		}
	}
	_ = w.Flush()
}

// workServe computes the per-site aggregates (not measured), then serves the
// line protocol on loopback until every client connection has gone away.
// GOMAXPROCS=1 is set explicitly: the axis is "concurrency on one core",
// goroutines and the Go scheduler, not parallelism.
func workServe(path string, port int) int {
	t0 := nowMs()
	buf := readFile(path)
	t1 := nowMs()
	e := computeEtl(buf)
	serveBuf = buf

	runtime.GOMAXPROCS(1)

	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen: %v\n", err)
		os.Exit(2)
	}
	st := &serveState{etl: &e}
	var live atomic.Int64
	done := make(chan struct{})
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			if tc, ok := c.(*net.TCPConn); ok {
				_ = tc.SetNoDelay(true)
			}
			live.Add(1)
			go func() {
				serveConn(c, st)
				// The server is done when the last connection is gone and
				// at least one request was answered.
				if live.Add(-1) == 0 && st.served.Load() > 0 {
					close(done)
				}
			}()
		}
	}()

	fmt.Printf("serve_ready\n")
	<-done
	ms := nowMs() - st.startMs

	h := fnvBasis
	for s := 0; s < nSites; s++ {
		h = fnv(h, uint64(s))
		h = fnv(h, uint64(e.sites[s].count))
		h = fnv(h, uint64(e.sites[s].sumLat))
		h = fnv(h, uint64(e.sites[s].maxLat))
		h = fnv(h, uint64(e.sites[s].sumBytes))
	}
	fmt.Printf("serve n=%d read_ms=%.1f req=%d secs=%.1f cs=%d\n",
		e.lines, t1-t0, st.served.Load(), ms, h)
	_ = ln.Close()
	return 0
}

// ------------------------------------------------------------------ workload D

// mixRecord is the fixed 64-round mix the parmix contract specifies.
func mixRecord(r *Event) uint64 {
	x := uint64(r.ts) ^ (uint64(r.uid) << 17) ^ (uint64(r.lat) << 33) ^ uint64(r.bytes)
	for k := 0; k < 64; k++ {
		x ^= x >> 33
		x *= 0xff51afd7ed558ccd
		x ^= x >> 29
		x *= 0xc4ceb9fe1a85ec53
	}
	return x
}

func workParmix(path string, threads int) int {
	t0 := nowMs()
	buf := readFile(path)
	t1 := nowMs()

	// Offsets are sequential work, outside the parallel section: each worker
	// must only parse its own records, and every record's start is known.
	offs := make([]uint32, 0, len(buf)/40+16)
	p := 0
	n := len(buf)
	for p < n {
		offs = append(offs, uint32(p))
		for p < n && buf[p] != '\n' {
			p++
		}
		if p < n {
			p++
		}
	}
	nrec := len(offs)
	out := make([]uint64, nrec)

	if threads < 1 {
		threads = 1
	}
	worker := func(lo, hi int) {
		for i := lo; i < hi; i++ {
			r, _ := parseRecord(buf, int(offs[i]))
			out[i] = mixRecord(&r)
		}
	}
	if threads == 1 {
		worker(0, nrec)
	} else {
		var wg sync.WaitGroup
		chunk := (nrec + threads - 1) / threads
		for t := 0; t < threads; t++ {
			lo := chunk * t
			hi := lo + chunk
			if hi > nrec {
				hi = nrec
			}
			if lo >= hi {
				break
			}
			wg.Add(1)
			go func(lo, hi int) {
				defer wg.Done()
				worker(lo, hi)
			}(lo, hi)
		}
		wg.Wait()
	}
	t2 := nowMs()

	var h uint64 = fnvBasis
	var sum uint64
	for i := 0; i < nrec; i++ {
		sum += out[i]
		h = fnv(h, out[i])
	}
	h = fnv(h, uint64(nrec))
	fmt.Printf("parmix n=%d t=%d read_ms=%.1f work_ms=%.1f cs=%d sum=%d\n",
		nrec, threads, t1-t0, t2-t1, h, sum)
	return 0
}

// ------------------------------------------------------------------------ main

func usage() {
	fmt.Fprintf(os.Stderr,
		"usage: realsvc_go <etl|sessions|serve|parmix> <log.tsv> [args]\n"+
			"  serve  <port> [requests]\n"+
			"  parmix [threads]\n")
}

func main() {
	if len(os.Args) < 3 {
		usage()
		os.Exit(2)
	}
	path := os.Args[2]
	switch os.Args[1] {
	case "etl":
		os.Exit(workEtl(path))
	case "sessions":
		os.Exit(workSessions(path))
	case "serve":
		port := 9100
		if len(os.Args) > 3 {
			port, _ = strconv.Atoi(os.Args[3])
		}
		os.Exit(workServe(path, port))
	case "parmix":
		threads := 8
		if len(os.Args) > 3 {
			threads, _ = strconv.Atoi(os.Args[3])
		}
		os.Exit(workParmix(path, threads))
	default:
		fmt.Fprintf(os.Stderr, "unknown mode %s\n", os.Args[1])
		os.Exit(2)
	}
}

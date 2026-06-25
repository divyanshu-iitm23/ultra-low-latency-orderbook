// itch_metrics_replay.cpp - live latency monitoring driven by a REAL NASDAQ ITCH feed.
//
// Same engine + metrics wiring as engine_metrics_demo; only the producer changes:
// the hot thread parses a real ITCH 5.0 file and drives the OrderBook via BookReplay.
// Because add/cancel/modify are instrumented, every book-affecting message is timed
// automatically; the aggregator thread drains the ring and paints the live `top` view.

#include "book_replay.hpp"          // OrderBook + BookReplay + itch::parseBuffer
#include "metrics_recorder.hpp"
#include "aggregator.hpp"
#include "rdtsc_timer.hpp"          // orderbook::TscClock (ticks -> ns)

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

using namespace orderbook;
namespace m = metrics;

static uint64_t now_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop.store(true, std::memory_order_release); }

// Sleep the bulk / spin the tail until CLOCK_MONOTONIC reaches target_wall (ns).
// Sleeps are chunked to stay responsive to Ctrl-C across long inter-message gaps.
static void pace_to(uint64_t target_wall) {
    for (;;) {
        const uint64_t t = now_ns();
        if (t >= target_wall) return;
        if (g_stop.load(std::memory_order_acquire)) return;   // don't get stuck pacing
        const uint64_t rem = target_wall - t;
        if (rem > 2000000ULL) {                               // >2 ms: sleep all but ~1 ms
            uint64_t sl = rem - 1000000ULL;
            if (sl > 100000000ULL) sl = 100000000ULL;         // cap 100 ms (Ctrl-C latency)
            timespec ts{ (time_t)(sl / 1000000000ULL), (long)(sl % 1000000000ULL) };
            nanosleep(&ts, nullptr);
        } else {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s <itch_file> <SYMBOL> [options]\n"
            "  --pace=<F>     replay speed: 0=max (default), 1=real-time, 10=10x\n"
            "  --snap=<N>     snapshot every N messages (default 8192)\n"
            "  --no-prefault  lazy load (live view instant; early samples carry one-time\n"
            "                 page-fault jitter) instead of MAP_POPULATE prefault\n",
            argv[0]);
        return 1;
    }
    const char* path = argv[1];
    const char* sym  = argv[2];
    double   pace       = 0.0;       // 0 => max throughput (no pacing)
    uint64_t snap_every = 8192;
    bool     prefault   = true;
    for (int i = 3; i < argc; ++i) {
        if      (!strncmp(argv[i], "--pace=", 7)) pace = atof(argv[i] + 7);
        else if (!strncmp(argv[i], "--snap=", 7)) snap_every = strtoull(argv[i] + 7, nullptr, 10);
        else if (!strcmp (argv[i], "--no-prefault")) prefault = false;
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 1; }
    }
    if (snap_every == 0) snap_every = 8192;
    if (pace < 0)        pace = 0.0;
    std::signal(SIGINT, on_sigint);

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st{}; fstat(fd, &st);

    char pacebuf[40];
    if (pace <= 0) snprintf(pacebuf, sizeof pacebuf, "max (flat-out)");
    else           snprintf(pacebuf, sizeof pacebuf, "%g x%s", pace, pace == 1 ? " (real-time)" : "");
    printf("ITCH metrics replay: %s   (%s, %.1f MB)\n", sym, path, st.st_size / 1e6);
    printf("pace: %s   prefault: %s   snapshot: every %llu msgs\n",
           pacebuf, prefault ? "on" : "off (lazy)", (unsigned long long)snap_every);

    // the file
    const int flags = MAP_PRIVATE | (prefault ? MAP_POPULATE : 0);
    void* p = mmap(nullptr, st.st_size, PROT_READ, flags, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    const uint8_t* data = (const uint8_t*)p;
    madvise(p, st.st_size, MADV_SEQUENTIAL);
    if (prefault) {
        madvise(p, st.st_size, MADV_WILLNEED);
        printf("loading %.1f MB into RAM (prefault)... ", st.st_size / 1e6);
        fflush(stdout);
        const uint64_t l0 = now_ns();
        volatile uint64_t t = 0;
        for (size_t i = 0; i < (size_t)st.st_size; i += 4096) t += data[i];
        (void)t;
        printf("done in %.2f s\n", (now_ns() - l0) / 1e9);
    } else {
        printf("lazy load - pages fault as the parser reaches them\n");
    }
    printf("starting hot thread (parse+book) + aggregator thread...\n\n");

    // the pipeline
    orderbook::TscClock clk;
    const double ns_per_tick = 1e9 / (double)clk.hz();

    m::EventRing ring;
    m::MetricsRecorder rec(ring, 0);                 // record every op
    OrderBook book(1, 500000, 1u << 21);             // ladder to $5000, 2M-order pool
    book.setMetrics(&rec);                           // <-- engine now times its ops
    BookReplay rp(book, sym);
    m::Aggregator agg(ring, rec.dropsCounter(), ns_per_tick, /*render_hz=*/5.0);

    std::thread consumer([&]{ agg.run(); });

    // producer: THIS thread parses + drives the book, optionally paced by ITCH time
    uint64_t msgs = 0, itch0 = 0, wall0 = 0;
    bool have_base = false;
    size_t n = itch::parseBuffer(data, (size_t)st.st_size,
        [&](const itch::Message& mm){
            if (g_stop.load(std::memory_order_acquire)) return;     // Ctrl-C: ignore the rest
            if (pace > 0) {                                          // wall-clock pacing
                if (!have_base) { itch0 = mm.timestamp; wall0 = now_ns(); have_base = true; }
                else {
                    const uint64_t itch_el = (mm.timestamp >= itch0) ? (mm.timestamp - itch0) : 0;
                    pace_to(wall0 + (uint64_t)((double)itch_el / pace));
                }
            }
            rp.onMessage(mm);                                        // the timed work
            if ((++msgs % snap_every) == 0)
                rec.recordSnapshot(book.getBestBid(), book.getBestAsk());
        });
    rec.recordSnapshot(book.getBestBid(), book.getBestAsk());        // final state

    agg.stop();
    consumer.join();

    // printing summary..
    printf("\n--- replay summary (%s) ---\n", sym);
    if (g_stop.load()) printf("** stopped early (Ctrl-C) after %llu processed messages **\n",
                              (unsigned long long)msgs);
    printf("messages parsed : %zu   (book-affecting: adds=%zu deletes=%zu reduces=%zu replaces=%zu)\n",
           n, rp.added(), rp.deleted(), rp.reduced(), rp.replaced());
    printf("metric events   : consumed=%llu  drops=%llu  (book/ladder prices are in cents)\n",
           (unsigned long long)agg.consumed(), (unsigned long long)rec.drops());
    if (book.getActiveBids() && book.getActiveAsks())
        printf("final book      : best_bid=$%.2f best_ask=$%.2f spread=$%.2f live=%zu\n",
               book.getBestBid()/100.0, book.getBestAsk()/100.0,
               book.getSpread()/100.0, book.getTotalOrders());
    else
        printf("final book      : one side empty (wrong symbol, or full-day file drained)\n");

    munmap(p, st.st_size);
    close(fd);
    return 0;
}

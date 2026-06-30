// json_snapshot_demo.cpp - emit the aggregator's per-cycle snapshot as NDJSON
//   ./json_snapshot_demo [seconds]      (stdout = NDJSON, one object per render cycle)
//
#include "orderbook.hpp"
#include "metrics_recorder.hpp"
#include "aggregator.hpp"
#include "rdtsc_timer.hpp"

#include <atomic>
#include <thread>
#include <vector>
#include <random>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>

using namespace metrics;
using orderbook::OrderBook;
using orderbook::Side;
using orderbook::Price;
using orderbook::Quantity;
using orderbook::OrderId;

static uint64_t now_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static std::atomic<bool> g_run{true};
static void on_sigint(int) { g_run.store(false, std::memory_order_release); }

int main(int argc, char** argv) {
    const double seconds = (argc > 1) ? atof(argv[1]) : 6.0;
    std::signal(SIGINT, on_sigint);

    orderbook::TscClock clk;
    const double ns_per_tick = 1e9 / (double)clk.hz();

    EventRing ring;
    MetricsRecorder rec(ring, 0);
    OrderBook book(1, 200000, 1u << 16);
    book.setMetrics(&rec);

    Aggregator agg(ring, rec.dropsCounter(), ns_per_tick, /*render_hz=*/5.0);
    agg.setConsole(false);                                   // NDJSON instead of "top" view
    agg.setSnapshotSink([](const MetricsSnapshot& s){
        char buf[4096];
        size_t n = writeJson(s, buf, sizeof buf);
        fwrite(buf, 1, n, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    });

    std::thread hot([&]{
        const uint64_t deadline_ns = seconds > 0 ? (now_ns() + (uint64_t)(seconds * 1e9)) : 0;
        std::mt19937_64 rng(0xC0FFEE);
        std::vector<OrderId> live; live.reserve(1u << 16);
        const Price MID = 100000;
        uint64_t i = 0;
        while (g_run.load(std::memory_order_acquire)) {
            const unsigned r = rng() % 100;
            const bool force_cancel = live.size() > (1u << 15);
            if (!force_cancel && (live.empty() || r < 55)) {
                Side s = (rng() & 1) ? Side::BUY : Side::SELL;
                Price px = (s == Side::BUY) ? MID - 1 - (Price)(rng() % 20)
                                            : MID + 1 + (Price)(rng() % 20);
                OrderId id = book.addOrder(s, px, 1 + (rng() % 100));
                if (id) live.push_back(id);
            } else if (force_cancel || r < 80) {
                size_t k = rng() % live.size();
                book.cancelOrder(live[k]);
                live[k] = live.back(); live.pop_back();
            } else {
                book.modifyOrder(live[rng() % live.size()], 1 + (rng() % 100));
            }
            if ((i % 1000) == 0) rec.recordSnapshot(book.getBestBid(), book.getBestAsk());
            ++i;
            if (deadline_ns && (i & 1023) == 0 && now_ns() >= deadline_ns) break;
        }
        g_run.store(false, std::memory_order_release);
    });

    std::thread consumer([&]{ agg.run(); });
    hot.join();
    agg.stop();
    consumer.join();

    // keep stdout pure NDJSON; status goes to stderr
    fprintf(stderr, "json_snapshot_demo: done (events=%llu drops=%llu)\n",
            (unsigned long long)agg.consumed(), (unsigned long long)rec.drops());
    return 0;
}

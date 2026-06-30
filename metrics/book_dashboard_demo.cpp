// book_dashboard_demo.cpp - drive a live two-sided book with trades and publish the full
// snapshot (latencies + depth ladder + trade tape) to the dashboard.
//
// Unlike engine_metrics_demo (storage-only churn, no depth/trades), this keeps resting
// orders on both sides AND crosses the book with market orders, so the snapshot carries:
//   - per-op add/cancel/modify latencies (as before)
//   - a top-N depth ladder, published lock-free via BookPublisher (seqlock)
//   - a trade tape, fed by recordTrade() on each execution
// Console "top" view stays on; UDP publishes to the bridge -> browser.
//
#include "orderbook.hpp"
#include "metrics_recorder.hpp"
#include "aggregator.hpp"
#include "udp_publisher.hpp"
#include "book_view.hpp"
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

using orderbook::OrderBook;
using orderbook::Side;
using orderbook::Price;
using orderbook::Quantity;
using orderbook::OrderId;
using orderbook::Trade;

static uint64_t now_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static std::atomic<bool> g_run{true};
static void on_sigint(int) { g_run.store(false, std::memory_order_release); }

int main(int argc, char** argv) {
    const double   seconds = (argc > 1) ? atof(argv[1]) : 30.0;
    const char*    host    = (argc > 2) ? argv[2] : "127.0.0.1";
    const uint16_t port    = (argc > 3) ? (uint16_t)atoi(argv[3]) : 9099;
    std::signal(SIGINT, on_sigint);

    metrics::UdpPublisher pub(host, port);
    if (!pub.ok()) { fprintf(stderr, "bad host/port %s:%u\n", host, port); return 1; }
    fprintf(stderr, "book_dashboard_demo: depth+tape -> %s:%u for %.0fs\n", host, port, seconds);

    orderbook::TscClock clk;
    const double ns_per_tick = 1e9 / (double)clk.hz();

    metrics::EventRing ring;
    metrics::MetricsRecorder rec(ring, 0);
    OrderBook book(1, 200000, 1u << 16);
    book.setMetrics(&rec);
    metrics::BookPublisher bookpub;

    metrics::Aggregator agg(ring, rec.dropsCounter(), ns_per_tick, /*render_hz=*/5.0);
    agg.setConsole(true);                      // console + browser at once
    agg.setBookPublisher(&bookpub);            // depth side-channel
    agg.setSnapshotSink([&pub](const metrics::MetricsSnapshot& s){ pub.send(s); });

    std::thread hot([&]{
        const uint64_t deadline_ns = seconds > 0 ? (now_ns() + (uint64_t)(seconds * 1e9)) : 0;
        std::mt19937_64 rng(0xBEEF);
        std::vector<OrderId> live; live.reserve(1u << 16);
        const Price MID = 100000;
        OrderId taker = 1000000000ULL;         // distinct from the book's auto ids
        uint64_t i = 0;

        auto publishDepth = [&]{
            orderbook::DepthLevel ob_b[metrics::MAX_DEPTH], ob_a[metrics::MAX_DEPTH];
            int nb = 0, na = 0;
            book.getDepth(metrics::MAX_DEPTH, ob_b, &nb, ob_a, &na);
            metrics::BookView bv;
            bv.nbids = (uint32_t)nb; bv.nasks = (uint32_t)na;
            for (int k = 0; k < nb; ++k) bv.bids[k] = { ob_b[k].price, (int64_t)ob_b[k].qty };
            for (int k = 0; k < na; ++k) bv.asks[k] = { ob_a[k].price, (int64_t)ob_a[k].qty };
            bookpub.publish(bv);
        };

        while (g_run.load(std::memory_order_acquire)) {
            const unsigned r = rng() % 100;
            const bool too_big = live.size() > (1u << 15);

            if (!too_big && r < 88) {                              // ADD resting (banded)
                Side s = (rng() & 1) ? Side::BUY : Side::SELL;
                Price px = (s == Side::BUY) ? MID - 1 - (Price)(rng() % 10)
                                            : MID + 1 + (Price)(rng() % 10);
                OrderId id = book.addOrder(s, px, 1 + (rng() % 50));
                if (id) live.push_back(id);
            } else if (!too_big && r < 94) {                       // CROSS -> trades
                Side aggressor = (rng() & 1) ? Side::BUY : Side::SELL;
                std::vector<Trade> trades = book.submitMarket(aggressor, 1 + (rng() % 30), taker++);
                for (const Trade& t : trades)
                    rec.recordTrade(t.price, (uint32_t)t.quantity, (uint32_t)t.makerId);
            } else if (!live.empty()) {                            // CANCEL
                size_t k = rng() % live.size();
                book.cancelOrder(live[k]);
                live[k] = live.back(); live.pop_back();
            }

            if ((i % 256) == 0) {                                  // ~off-hot-path depth/snapshot
                publishDepth();
                rec.recordSnapshot(book.getBestBid(), book.getBestAsk());
            }
            ++i;
            if (deadline_ns && (i & 1023) == 0 && now_ns() >= deadline_ns) break;
        }
        g_run.store(false, std::memory_order_release);
    });

    std::thread consumer([&]{ agg.run(); });
    hot.join();
    agg.stop();
    consumer.join();

    fprintf(stderr, "book_dashboard_demo: done  datagrams=%llu  events=%llu drops=%llu\n",
            (unsigned long long)pub.sent(), (unsigned long long)agg.consumed(),
            (unsigned long long)rec.drops());
    return 0;
}

// engine_metrics_demo.cpp - metrics wired into the REAL OrderBook.
//
// Unlike aggregator_demo.cpp (which times synthetic do_work), this drives the actual
// engine: a hot thread issues real add / cancel / modify against an OrderBook that has
// a MetricsRecorder attached via setMetrics(), so every operation is timed by the
// ScopedLatency now compiled into orderbook.cpp. A second thread runs the Aggregator
// and paints the live `top` readout, with the book line fed by real best bid/ask.
//
// On exit: produced == consumed + drops  (every timed op is accounted for).

#include "orderbook.hpp"            // the real engine
#include "metrics_recorder.hpp"
#include "aggregator.hpp"
#include "rdtsc_timer.hpp"         // orderbook::TscClock (ticks -> ns)

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
    const double seconds = (argc > 1) ? atof(argv[1]) : 8.0;   // 0 => until Ctrl-C
    std::signal(SIGINT, on_sigint);

    orderbook::TscClock clk;
    const double ns_per_tick = 1e9 / (double)clk.hz();

    EventRing ring;
    MetricsRecorder rec(ring, 0);              // mask 0 -> record every op (clean accounting)

    OrderBook book(/*min_price=*/1, /*max_price=*/200000, /*pool_cap=*/1u << 16);
    book.setMetrics(&rec);                     // <-- the wiring: engine now times its ops

    Aggregator agg(ring, rec.dropsCounter(), ns_per_tick, /*render_hz=*/5.0);

    std::atomic<uint64_t> produced{0};

    // hot thread: on real book
    std::thread hot([&]{
        const uint64_t deadline_ns = seconds > 0
            ? (now_ns() + (uint64_t)(seconds * 1e9)) : 0;
        std::mt19937_64 rng(0xC0FFEE);
        std::vector<OrderId> live;             // ids currently resting (for cancel/modify)
        live.reserve(1u << 16);

        const Price MID = 100000;              // centre of activity, in ticks
        uint64_t i = 0, p = 0;
        while (g_run.load(std::memory_order_acquire)) {
            const unsigned r = rng() % 100;
            // keep the book bounded: force a cancel when it grows large
            const bool force_cancel = live.size() > (1u << 15);

            if (!force_cancel && (live.empty() || r < 55)) {            // ADD (~55%)
                // band the prices so bids rest below asks -> book stays uncrossed
                Side s = (rng() & 1) ? Side::BUY : Side::SELL;
                Price px = (s == Side::BUY) ? MID - 1 - (Price)(rng() % 20)   // [MID-20, MID-1]
                                            : MID + 1 + (Price)(rng() % 20);  // [MID+1, MID+20]
                Quantity q = 1 + (rng() % 100);
                OrderId id = book.addOrder(s, px, q);
                if (id) live.push_back(id);
                ++p;
            } else if (force_cancel || r < 80) {                       // CANCEL (~25%)
                size_t k = rng() % live.size();
                book.cancelOrder(live[k]);
                live[k] = live.back(); live.pop_back();                // swap-remove
                ++p;
            } else {                                                   // MODIFY (~20%)
                book.modifyOrder(live[rng() % live.size()], 1 + (rng() % 100));
                ++p;
            }

            if ((i % 1000) == 0) {                                     // periodic book snapshot
                rec.recordSnapshot(book.getBestBid(), book.getBestAsk());
                ++p;
            }
            ++i;
            if (deadline_ns && (i & 1023) == 0 && now_ns() >= deadline_ns) break;
        }
        produced.store(p, std::memory_order_release);
        g_run.store(false, std::memory_order_release);
    });

    // consumer: aggregator + live readout
    std::thread consumer([&]{ agg.run(); });

    hot.join();
    agg.stop();
    consumer.join();

    // self-checks
    const uint64_t prod = produced.load();
    const uint64_t cons = agg.consumed();
    const uint64_t drops = rec.drops();
    printf("\nengine ops timed.  produced=%llu  consumed=%llu  drops=%llu  ->  %s\n",
           (unsigned long long)prod, (unsigned long long)cons, (unsigned long long)drops,
           (cons + drops == prod) ? "ACCOUNTING OK (nothing lost)"
                                  : "ACCOUNTING MISMATCH");
    printf("final book: best_bid=%lld best_ask=%lld spread=%lld live_orders=%zu\n",
           (long long)book.getBestBid(), (long long)book.getBestAsk(),
           (long long)book.getSpread(), book.getTotalOrders());
    return (cons + drops == prod) ? 0 : 1;
}

// aggregator_demo.cpp - demo of the metrics consumer pipeline.
//
// Producer thread = a stand-in for the engine hot path: it times synthetic work
// with ScopedLatency (Add/Cancel/Modify) and periodically emits Trade + Snapshot
// events. Consumer thread = the Aggregator, draining the ring and painting a live
// `top`-style readout (calibrated to real nanoseconds via the engine's TscClock).
// On exit it prints an accounting self-check: produced == consumed + drops.
//
// Build:
//   g++ -O2 -std=c++17 -pthread -Imetrics/include -Iinclude \
//       metrics/aggregator_demo.cpp -o build/aggregator_demo
// Run:
//   ./build/aggregator_demo [seconds]      (0 or omitted with a TTY => until Ctrl-C)

#include "aggregator.hpp"
#include "metrics_recorder.hpp"
#include "rdtsc_timer.hpp"          // engine's calibrated TSC (orderbook::TscClock)

#include <atomic>
#include <thread>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>

using namespace metrics;

static uint64_t now_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// opaque work the optimizer can't fold away -> gives ScopedLatency something real to time
static volatile uint64_t sink = 0;
static uint64_t do_work(uint64_t x, int iters) {
    uint64_t a = x;
    for (int i = 0; i < iters; ++i) a = a * 6364136223846793005ULL + 1;
    sink += a;
    return a;
}

static std::atomic<bool> g_run{true};
static void on_sigint(int) { g_run.store(false, std::memory_order_release); }

int main(int argc, char** argv) {
    const double seconds = (argc > 1) ? atof(argv[1]) : 8.0;   // 0 => run until Ctrl-C
    std::signal(SIGINT, on_sigint);

    orderbook::TscClock clk;                 // calibrate TSC -> ns once
    const double ns_per_tick = 1e9 / (double)clk.hz();

    EventRing ring;
    MetricsRecorder rec(ring, 0);            // sample_mask 0 -> record every op
    Aggregator agg(ring, rec.dropsCounter(), ns_per_tick, /*render_hz=*/5.0);

    std::atomic<uint64_t> produced{0};

    // producer: the simulated engine hot path
    std::thread producer([&]{
        const uint64_t deadline_ns = seconds > 0
            ? (now_ns() + (uint64_t)(seconds * 1e9)) : 0;
        uint64_t i = 0, p = 0;
        while (g_run.load(std::memory_order_acquire)) {
            { ScopedLatency _l(rec, OpType::Add);    do_work(i, 18);      ++p; }
            { ScopedLatency _l(rec, OpType::Cancel); do_work(i ^ 7, 14);  ++p; }
            if ((i & 3) == 0) { ScopedLatency _l(rec, OpType::Modify); do_work(i*3, 22); ++p; }

            if ((i % 2000) == 0) {
                // occasional trade + book snapshot, like the matching engine would emit
                rec.recordTrade(100500 + (int64_t)(i % 50), 1 + (uint32_t)(i % 7),
                                (uint32_t)(i % 9));                         ++p;
                rec.recordSnapshot(28753 - (int64_t)(i % 3), 28758 + (int64_t)(i % 4)); ++p;
            }
            ++i;
            if (deadline_ns && (i & 1023) == 0 && now_ns() >= deadline_ns) break;
        }
        produced.store(p, std::memory_order_release);
        g_run.store(false, std::memory_order_release);
    });

    // consumer: aggregator + live readout
    std::thread consumer([&]{ agg.run(); });

    producer.join();
    agg.stop();
    consumer.join();

    // self-check
    const uint64_t prod = produced.load();
    const uint64_t cons = agg.consumed();
    const uint64_t drops = rec.drops();
    printf("\nproduced=%llu  consumed=%llu  drops=%llu  ->  %s\n",
           (unsigned long long)prod, (unsigned long long)cons, (unsigned long long)drops,
           (cons + drops == prod) ? "ACCOUNTING OK (nothing lost)"
                                  : "ACCOUNTING MISMATCH");
    return (cons + drops == prod) ? 0 : 1;
}

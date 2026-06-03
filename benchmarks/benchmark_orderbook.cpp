// benchmark_orderbook.cpp
//
// Cycle-accurate benchmark for the order book using a calibrated TSC clock.
//
// What it does right:
//   - calibrates ticks->ns at runtime (no hardcoded GHz)
//   - measures and SUBTRACTS the timer's own overhead
//   - PRE-GENERATES all inputs so the RNG is never inside the timed region
//   - warms up data structures, caches, branch predictor, and CPU frequency
//   - pins to a single core to avoid migration
//   - reports BOTH a batch throughput mean AND a per-op latency distribution
//   - repeats the distribution run and reports median-of-medians for stability
//
// Build (see CMakeLists): -O3 -march=native -std=c++17
// Run pinned:  taskset -c 3 ./benchmark_orderbook
//
#include "orderbook.hpp"
#include "rdtsc_timer.hpp"

#include <sched.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>
#include <thread>

using namespace orderbook;

// ---- pin this thread to one core (Linux). Harmless if it fails. ----
static void pinToCore(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

struct Input {
    Side     side;
    Price    price;
    Quantity qty;
};

// Pre-generate a fixed set of inputs (NOT timed).
static std::vector<Input> makeInputs(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> price(9000, 11000);
    std::uniform_int_distribution<int> qty(100, 1000);
    std::uniform_int_distribution<int> side(0, 1);
    std::vector<Input> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i)
        v.push_back({ side(rng) ? Side::SELL : Side::BUY,
                      (Price)price(rng), (Quantity)qty(rng) });
    return v;
}

// ---- 1) BATCH THROUGHPUT: time the whole loop, divide by N. ----
// Overhead amortizes to ~zero; gives a trustworthy mean ns/op.
static double batchThroughputNs(const TscClock& clk, const std::vector<Input>& in) {
    OrderBook book(1, 20000, 1 << 20);
    // warm up: create every price level so we measure steady-state adds
    for (Price p = 9000; p <= 11000; ++p) book.addOrder(Side::BUY, p, 1);

    uint64_t s = tscStart();
    for (const auto& x : in) {
        OrderId id = book.addOrder(x.side, x.price, x.qty);
        doNotOptimize(id);
    }
    uint64_t e = tscEnd();
    return clk.ticksToNs(e - s) / (double)in.size();
}

// ---- 2) PER-OP DISTRIBUTION: time each addOrder individually. ----
// Overhead-subtracted; returns the sorted per-op ns samples.
static std::vector<double> perOpLatenciesNs(const TscClock& clk, uint64_t overhead,
                                            const std::vector<Input>& in) {
    OrderBook book(1, 20000, 1 << 20);
    for (Price p = 9000; p <= 11000; ++p) book.addOrder(Side::BUY, p, 1);

    // warm up the exact code path (discarded)
    for (int w = 0; w < 20000; ++w) {
        const auto& x = in[w % in.size()];
        OrderId id = book.addOrder(x.side, x.price, x.qty);
        doNotOptimize(id);
    }

    std::vector<uint64_t> ticks;
    ticks.reserve(in.size());
    for (const auto& x : in) {
        uint64_t s = tscStart();
        OrderId id = book.addOrder(x.side, x.price, x.qty);
        doNotOptimize(id);
        uint64_t e = tscEnd();
        uint64_t d = e - s;
        ticks.push_back(d > overhead ? d - overhead : 0);
    }
    std::sort(ticks.begin(), ticks.end());

    std::vector<double> ns;
    ns.reserve(ticks.size());
    for (uint64_t t : ticks) ns.push_back(clk.ticksToNs(t));
    return ns;
}

static double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

int main(int argc, char** argv) {
    int core = (std::thread::hardware_concurrency() > 1)
                   ? (int)std::thread::hardware_concurrency() - 1 : 0;
    if (argc > 1) core = atoi(argv[1]);
    pinToCore(core);

    printf("=== Order Book RDTSC Benchmark ===\n");
    printf("pinned to core      : %d\n", core);

    TscClock clk;
    uint64_t overhead = measureTimerOverhead();
    printf("TSC frequency       : %.4f GHz\n", clk.hz() / 1e9);
    printf("timer overhead      : %llu ticks (%.2f ns) [subtracted per-op]\n\n",
           (unsigned long long)overhead, clk.ticksToNs(overhead));

    const size_t N = 200000;
    auto inputs = makeInputs(N, 12345);

    // ---- batch throughput ----
    double batch_ns = batchThroughputNs(clk, inputs);
    printf("[Batch throughput]  addOrder mean = %.1f ns/op  (%.2f M ops/s)\n\n",
           batch_ns, 1000.0 / batch_ns);

    // ---- per-op distribution, repeated for stability ----
    const int REPS = 7;
    std::vector<double> p50s, p90s, p99s, p999s, maxes;
    for (int r = 0; r < REPS; ++r) {
        auto ns = perOpLatenciesNs(clk, overhead, inputs);
        // ns is sorted ascending
        auto pct = [&](double p) {
            size_t idx = (size_t)(ns.size() * p);
            if (idx >= ns.size()) idx = ns.size() - 1;
            return ns[idx];
        };
        p50s.push_back(pct(0.50));
        p90s.push_back(pct(0.90));
        p99s.push_back(pct(0.99));
        p999s.push_back(pct(0.999));
        maxes.push_back(ns.back());
    }

    printf("[Per-op distribution]  median-of-%d-runs, overhead-subtracted\n", REPS);
    printf("  p50   : %7.1f ns\n", median(p50s));
    printf("  p90   : %7.1f ns\n", median(p90s));
    printf("  p99   : %7.1f ns\n", median(p99s));
    printf("  p99.9 : %7.1f ns\n", median(p999s));
    printf("  max   : %7.1f ns  (worst single sample; high variance)\n", median(maxes));

    return 0;
}
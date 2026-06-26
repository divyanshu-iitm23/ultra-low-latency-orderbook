// benchmark_overhead.cpp - the instrumentation Overhead A/B.
//
// Question: how many ns does the metrics instrumentation add to a single addOrder?
//
// Built in BOTH configs; the A/B is the delta of the means across the two builds:
//   build-baseline (METRICS_ENABLED=0): prints "baseline" - clean engine, no metrics.
//   build-metrics  (METRICS_ENABLED=1): prints "detached" (compiled-in, no recorder)
//                                       and "attached" (recording every op, with a
//                                       draining consumer pinned to a second core).

#include "orderbook.hpp"
#include "rdtsc_timer.hpp"
#include "metrics_recorder.hpp"     // MetricsRecorder/ScopedLatency (stubs when off)

#include <sched.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>
#include <thread>
#include <atomic>

using namespace orderbook;
using metrics::MetricsRecorder;
using metrics::EventRing;
using metrics::MetricsEvent;

static void pinToCore(int core) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(core, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

struct Input { Side side; Price price; Quantity qty; };

static std::vector<Input> makeInputs(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> price(9000, 11000), qty(100, 1000), side(0, 1);
    std::vector<Input> v; v.reserve(n);
    for (size_t i = 0; i < n; ++i)
        v.push_back({ side(rng) ? Side::SELL : Side::BUY, (Price)price(rng), (Quantity)qty(rng) });
    return v;
}

static void attach(OrderBook& book, MetricsRecorder* rec) {
#if METRICS_ENABLED
    if (rec) book.setMetrics(rec);
#else
    (void)book; (void)rec;
#endif
}

static double batchNs(const TscClock& clk, const std::vector<Input>& in, MetricsRecorder* rec) {
    OrderBook book(1, 20000, 1 << 20);
    attach(book, rec);
    for (Price p = 9000; p <= 11000; ++p) book.addOrder(Side::BUY, p, 1);   // warm levels
    uint64_t s = tscStart();
    for (const auto& x : in) { OrderId id = book.addOrder(x.side, x.price, x.qty); doNotOptimize(id); }
    uint64_t e = tscEnd();
    return clk.ticksToNs(e - s) / (double)in.size();
}

static std::vector<double> perOpNs(const TscClock& clk, uint64_t overhead,
                                   const std::vector<Input>& in, MetricsRecorder* rec) {
    OrderBook book(1, 20000, 1 << 20);
    attach(book, rec);
    for (Price p = 9000; p <= 11000; ++p) book.addOrder(Side::BUY, p, 1);
    for (int w = 0; w < 20000; ++w) {                                       // warm the path
        const auto& x = in[w % in.size()];
        OrderId id = book.addOrder(x.side, x.price, x.qty); doNotOptimize(id);
    }
    std::vector<uint64_t> ticks; ticks.reserve(in.size());
    for (const auto& x : in) {
        uint64_t s = tscStart();
        OrderId id = book.addOrder(x.side, x.price, x.qty); doNotOptimize(id);
        uint64_t e = tscEnd();
        uint64_t d = e - s;
        ticks.push_back(d > overhead ? d - overhead : 0);
    }
    std::sort(ticks.begin(), ticks.end());
    std::vector<double> ns; ns.reserve(ticks.size());
    for (uint64_t t : ticks) ns.push_back(clk.ticksToNs(t));
    return ns;
}

static double median(std::vector<double> v) {
    if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2];
}

struct Result { double batch, p50, p99, p999, mx; };

static Result measure(const TscClock& clk, uint64_t overhead,
                      const std::vector<Input>& in, MetricsRecorder* rec) {
    Result r{};
    r.batch = batchNs(clk, in, rec);
    std::vector<double> p50s, p99s, p999s, maxes;
    for (int rep = 0; rep < 7; ++rep) {                                     // median-of-7
        auto ns = perOpNs(clk, overhead, in, rec);
        auto pct = [&](double p) { size_t i = (size_t)(ns.size() * p);
                                   if (i >= ns.size()) i = ns.size() - 1; return ns[i]; };
        p50s.push_back(pct(0.50)); p99s.push_back(pct(0.99));
        p999s.push_back(pct(0.999)); maxes.push_back(ns.back());
    }
    r.p50 = median(p50s); r.p99 = median(p99s); r.p999 = median(p999s); r.mx = median(maxes);
    return r;
}

static void report(const char* label, const Result& r) {
    printf("[%s]\n", label);
    printf("  batch mean : %6.2f ns/op   (%.2f M ops/s)\n", r.batch, 1000.0 / r.batch);
    printf("  p50        : %6.2f ns\n", r.p50);
    printf("  p99        : %6.2f ns\n", r.p99);
    printf("  p99.9      : %6.2f ns\n", r.p999);
    printf("  max        : %6.2f ns\n\n", r.mx);
}

int main(int argc, char** argv) {
    const int hw = (int)std::thread::hardware_concurrency();
    int core = (hw > 1) ? hw - 1 : 0;
    if (argc > 1) core = atoi(argv[1]);
    pinToCore(core);

    TscClock clk;
    const uint64_t overhead = measureTimerOverhead();
    const size_t N = 200000;
    auto inputs = makeInputs(N, 12345);

    printf("=== addOrder instrumentation overhead A/B ===\n");
    printf("pinned core   : %d\n", core);
    printf("TSC frequency : %.4f GHz\n", clk.hz() / 1e9);
    printf("timer overhead: %.2f ns [subtracted per-op]\n", clk.ticksToNs(overhead));

#if METRICS_ENABLED
    printf("BUILD         : METRICS_ENABLED=1 (instrumented)\n\n");

    Result det = measure(clk, overhead, inputs, nullptr);
    report("detached  (instrumentation compiled in, no recorder attached)", det);

    const int ccore = (core >= 1) ? core - 1 : (hw > 1 ? 1 : 0);
    EventRing ring;
    MetricsRecorder rec(ring, 0);                          // record every op
    std::atomic<bool> done{false};
    std::thread consumer([&]{
        pinToCore(ccore);
        MetricsEvent e;
        while (!done.load(std::memory_order_acquire)) {
            while (ring.try_pop(e)) { /* drain */ }
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
        while (ring.try_pop(e)) { /* final drain */ }
    });

    Result att = measure(clk, overhead, inputs, &rec);
    done.store(true, std::memory_order_release);
    consumer.join();

    printf("consumer core : %d   drops during attached run: %llu\n\n",
           ccore, (unsigned long long)rec.drops());
    report("attached  (rdtsc x2 + build 32B event + ring push, every op)", att);
#else
    printf("BUILD         : METRICS_ENABLED=0 (clean baseline)\n\n");
    Result base = measure(clk, overhead, inputs, nullptr);
    report("baseline  (no instrumentation compiled in)", base);
#endif
    return 0;
}

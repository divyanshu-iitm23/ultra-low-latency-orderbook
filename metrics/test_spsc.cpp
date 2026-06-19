// Torture test for SPSCRing

#include "spsc_ring.hpp"
#include "metrics_event.hpp"
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdint>
#include <cassert>
using namespace metrics;

static inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

static int failures = 0;
#define REQUIRE(c, m) do{ if(!(c)){ printf("FAIL: %s\n", (m)); ++failures; } }while(0)

// Test 1: no loss, exact ordering
template <std::size_t CAP>
static void test_no_loss(uint64_t N) {
    SPSCRing<uint64_t, CAP> ring;
    std::atomic<bool> bad{false};

    std::thread cons([&]{
        uint64_t expected = 0, v;
        while (expected < N) {
            if (ring.try_pop(v)) {
                if (v != expected) { bad.store(true); return; }   // out-of-order / dup / loss
                ++expected;
            } else {
                cpu_relax();
            }
        }
    });
    std::thread prod([&]{
        for (uint64_t i = 0; i < N; ++i)
            while (!ring.try_push(i)) cpu_relax();                // retry -> never lose
    });
    prod.join(); cons.join();
    REQUIRE(!bad.load(), "test1: consumer saw exact 0..N-1 in order (no reorder/dup/loss)");
}

// Test 2: drop on full, strictly increasing + accounting
template <std::size_t CAP>
static void test_drop(uint64_t N) {
    SPSCRing<uint64_t, CAP> ring;
    std::atomic<bool> done{false}, bad{false};
    std::atomic<uint64_t> pushed{0}, dropped{0}, popped{0};

    std::thread cons([&]{
        uint64_t v, prev = 0; bool have = false; uint64_t pc = 0;
        for (;;) {
            if (ring.try_pop(v)) {
                if (have && v <= prev) { bad.store(true); }       // not strictly increasing
                prev = v; have = true; ++pc;
            } else if (done.load(std::memory_order_acquire)) {
                while (ring.try_pop(v)) {                          // final drain
                    if (have && v <= prev) bad.store(true);
                    prev = v; have = true; ++pc;
                }
                break;
            } else {
                cpu_relax();
            }
        }
        popped.store(pc);
    });
    std::thread prod([&]{
        uint64_t up = 0, dp = 0;
        for (uint64_t i = 0; i < N; ++i) {
            if (ring.try_push(i)) ++up; else ++dp;                 // no retry -> drops happen
        }
        pushed.store(up); dropped.store(dp);
        done.store(true, std::memory_order_release);
    });
    prod.join(); cons.join();

    REQUIRE(!bad.load(), "test2: popped values strictly increasing (no reorder/dup under drops)");
    REQUIRE(pushed.load() == popped.load(), "test2: every pushed item was popped");
    REQUIRE(pushed.load() + dropped.load() == N, "test2: pushed + dropped == N (accounting)");
    printf("   [test2] N=%llu pushed=%llu dropped=%llu popped=%llu (%.1f%% dropped)\n",
           (unsigned long long)N, (unsigned long long)pushed.load(),
           (unsigned long long)dropped.load(), (unsigned long long)popped.load(),
           100.0 * dropped.load() / N);
}

// MetricsEvent round-trip through the ring
static void test_event_roundtrip() {
    SPSCRing<MetricsEvent, 256> ring;
    MetricsEvent in{}; in.tsc = 0xDEADBEEF; in.latency = 42;
    in.etype = EventType::Trade; in.otype = OpType::Match;
    in.payload.trade = { 100500, 250, 7 };
    REQUIRE(ring.try_push(in), "event: push succeeds");
    MetricsEvent out{};
    REQUIRE(ring.try_pop(out), "event: pop succeeds");
    REQUIRE(out.tsc==in.tsc && out.latency==in.latency && out.etype==EventType::Trade
            && out.payload.trade.price==100500 && out.payload.trade.qty==250
            && out.payload.trade.maker==7, "event: fields round-trip intact");
    MetricsEvent dummy{};
    REQUIRE(!ring.try_pop(dummy), "event: ring empty after pop");
}

int main() {
    const uint64_t N = 10'000'000;        // 10M items through a tiny ring -> heavy churn
    printf("SPSC torture test (N=%llu)\n", (unsigned long long)N);
    test_no_loss<1024>(N);
    test_drop<1024>(N);
    test_drop<16>(N);                     // even tinier ring -> mostly drops, stresses full path
    test_event_roundtrip();
    printf(failures==0 ? "\nSPSC RING OK (FIFO, no reorder/dup/loss, accounting, event round-trip)\n"
                       : "\n%d FAILURES\n", failures);
    return failures ? 1 : 0;
}
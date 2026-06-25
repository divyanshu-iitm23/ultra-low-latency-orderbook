// test_engine_wiring.cpp - deterministic, single-threaded test of the metrics
// instrumentation compiled INTO the OrderBook. No aggregator thread, no timing
// assertions on absolute ns: we drive a known sequence of public mutators and
// assert that exactly one correctly-typed Latency event comes out per call - and
// that detaching the recorder silences it. (Producer and consumer are the same
// thread here, run strictly in sequence, which is a valid degenerate SPSC use.)
#include "orderbook.hpp"
#include "metrics_recorder.hpp"
#include <cstdio>
#include <cstdint>

using namespace metrics;
using orderbook::OrderBook;
using orderbook::Side;
using ull = unsigned long long;

static int failures = 0;
#define REQUIRE(c, m) do { if (!(c)) { printf("FAIL: %s\n", (m)); ++failures; } } while (0)

struct Tally { ull add=0, cancel=0, modify=0, other=0, nonzero=0, total=0; };

// drain the ring (the producing calls have already finished) and tally by op type
static Tally drain(EventRing& ring) {
    Tally t; MetricsEvent e;
    while (ring.try_pop(e)) {
        ++t.total;
        if (e.etype == EventType::Latency) {
            if (e.latency > 0) ++t.nonzero;
            switch (e.otype) {
                case OpType::Add:    ++t.add;    break;
                case OpType::Cancel: ++t.cancel; break;
                case OpType::Modify: ++t.modify; break;
                default:             ++t.other;  break;
            }
        } else {
            ++t.other;
        }
    }
    return t;
}

int main() {
    {
        EventRing ring;
        MetricsRecorder rec(ring, 0);              // mask 0 -> record every op
        OrderBook book(1, 200000, 1u << 16);
        book.setMetrics(&rec);

        auto a = book.addOrder(Side::BUY, 100, 10);   // Add
        auto b = book.addOrder(Side::SELL, 200, 5);   // Add
        book.modifyOrder(a, 7);                        // Modify
        bool hit  = book.cancelOrder(b);               // Cancel (found)
        bool miss = book.cancelOrder(999999);          // Cancel (not found - still timed)

        REQUIRE(a != 0 && b != 0,  "adds returned valid ids");
        REQUIRE(hit && !miss,      "cancel hit succeeds, miss fails (but is still timed)");

        Tally t = drain(ring);
        REQUIRE(t.total  == 5, "exactly 5 events for 5 mutator calls");
        REQUIRE(t.add    == 2, "2 Add events");
        REQUIRE(t.cancel == 2, "2 Cancel events (including the miss)");
        REQUIRE(t.modify == 1, "1 Modify event");
        REQUIRE(t.other  == 0, "no stray event types");
        REQUIRE(t.nonzero == t.total, "all latencies non-zero (real rdtsc timing happened)");
        REQUIRE(rec.drops() == 0,     "no drops (ring is large enough)");
        printf("[attached]  add=%llu cancel=%llu modify=%llu total=%llu  drops=%llu\n",
               t.add, t.cancel, t.modify, t.total, (ull)rec.drops());
    }

    {
        EventRing ring;
        MetricsRecorder rec(ring, 0);
        OrderBook book(1, 200000, 1u << 16);
        book.setMetrics(nullptr);                  // explicitly detached
        auto a = book.addOrder(Side::BUY, 100, 10);
        book.modifyOrder(a, 3);
        book.cancelOrder(a);

        Tally t = drain(ring);
        REQUIRE(t.total == 0, "detached recorder produces no events");
        printf("[detached]  total=%llu (expected 0)\n", t.total);
    }

    printf(failures ? "\n%d FAILURES\n"
                    : "\nENGINE WIRING OK (one typed event per add/cancel/modify; detach silences)\n",
           failures);
    return failures ? 1 : 0;
}

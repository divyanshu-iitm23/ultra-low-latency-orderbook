#pragma once
// metrics_event.hpp - fixed-size POD pushed through the SPSC ring by the hot thread.
// Exactly 32 bytes so two events share one 64-byte cache line. No virtuals, no heap.
#include <cstdint>

namespace metrics {

// event type..
enum class EventType : uint8_t { Latency = 0, Snapshot = 1, Trade = 2 };
// operation type..
enum class OpType    : uint8_t { Add = 0, Cancel = 1, Modify = 2, Match = 3, Market = 4 };

struct MetricsEvent {
    uint64_t  tsc;          // rdtsc when recorded                         (8)
    uint32_t  latency;      // op latency in tsc ticks (Latency events)    (4)
    EventType etype;        //                                             (1)
    OpType    otype;        //                                             (1)
    uint16_t  flags;        // reserved / drop markers                     (2)
    union {                 //                                            (16)
        struct { int64_t best_bid; int64_t best_ask; } snap;     // Snapshot
        struct { int64_t price; uint32_t qty; uint32_t maker; } trade; // Trade
        uint8_t raw[16];
    } payload;
};

static_assert(sizeof(MetricsEvent) == 32, "MetricsEvent must stay 32 bytes (2 per cache line)");
static_assert(alignof(MetricsEvent) == 8, "MetricsEvent should be 8-byte aligned");

} // namespace metrics
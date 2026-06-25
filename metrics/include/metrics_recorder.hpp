#pragma once
// hot-path instrumentation, behind a compile-time switch called 'METRICS_ENABLED',
// METRICS_ENABLED=0 makes MetricsRecorder and ScopedLatency compile to NOTHING,
// METRICS_ENABLED=0 : no rdtsc, no ring push, no branchs
#include "metrics_config.hpp"
#include "tsc.hpp"
#include <atomic>

#ifndef METRICS_ENABLED
#  define METRICS_ENABLED 1     // compile-time switch
#endif

namespace metrics {

#if METRICS_ENABLED

class MetricsRecorder {
public:
    explicit MetricsRecorder(EventRing& ring, uint32_t sample_mask = 0)
        : ring_(&ring), sample_mask_(sample_mask) {}

    // Called by ScopedLatency,
    inline void recordLatency(OpType op, uint32_t ticks, uint64_t tsc_now) {
        if ((sample_count_++ & sample_mask_) != 0u) return;     // 1-in-(mask+1) sampling
        MetricsEvent e;
        e.tsc = tsc_now; e.latency = ticks;
        e.etype = EventType::Latency; e.otype = op; e.flags = 0;
        if (!ring_->try_push(e)) drops_.fetch_add(1, std::memory_order_relaxed);
    }
    inline void recordSnapshot(int64_t best_bid, int64_t best_ask) {
        MetricsEvent e; e.tsc = rdtsc(); e.latency = 0;
        e.etype = EventType::Snapshot; e.otype = OpType::Add; e.flags = 0;
        e.payload.snap = { best_bid, best_ask };
        if (!ring_->try_push(e)) drops_.fetch_add(1, std::memory_order_relaxed);
    }
    inline void recordTrade(int64_t price, uint32_t qty, uint32_t maker) {
        MetricsEvent e; e.tsc = rdtsc(); e.latency = 0;
        e.etype = EventType::Trade; e.otype = OpType::Match; e.flags = 0;
        e.payload.trade = { price, qty, maker };
        if (!ring_->try_push(e)) drops_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t drops() const { return drops_.load(std::memory_order_relaxed); }
    // Live handle to the drop counter so the aggregator (another thread) can poll it,
    const std::atomic<uint64_t>* dropsCounter() const { return &drops_; }

private:
    EventRing* ring_;
    uint64_t   sample_count_ = 0;       // producer-only,
    uint32_t   sample_mask_  = 0;
    std::atomic<uint64_t> drops_{0};    // atomic touched only on the (rare) drop path
};

class ScopedLatency {
public:
    ScopedLatency(MetricsRecorder& r, OpType op) : r_(&r), op_(op), t0_(rdtsc()) {}
    // constructor calling rdtsc()..
    ScopedLatency(MetricsRecorder* r, OpType op) : r_(r), op_(op), t0_(r ? rdtsc() : 0) {}
    // destructor calling rstsc() and reads Latency metric..
    ~ScopedLatency() { if (r_) { uint64_t t1 = rdtsc(); r_->recordLatency(op_, uint32_t(t1 - t0_), t1); } }
    ScopedLatency(const ScopedLatency&) = delete;
    ScopedLatency& operator=(const ScopedLatency&) = delete;
private:
    MetricsRecorder* r_; OpType op_; uint64_t t0_;
};

#else  // METRICS_ENABLED == 0 : everything below compiles to nothing

class MetricsRecorder {
public:
    MetricsRecorder() = default;
    explicit MetricsRecorder(EventRing&, uint32_t = 0) {}
    inline void recordLatency(OpType, uint32_t, uint64_t) {}
    inline void recordSnapshot(int64_t, int64_t) {}
    inline void recordTrade(int64_t, uint32_t, uint32_t) {}
    uint64_t drops() const { return 0; }
    const std::atomic<uint64_t>* dropsCounter() const { return nullptr; }
};
struct ScopedLatency { inline ScopedLatency(MetricsRecorder&, OpType) {} };

#endif

} // namespace metrics

// METRICS_SCOPE(recorder_ptr, op)
#if METRICS_ENABLED
#  define METRICS_SCOPE(rec_ptr, op) ::metrics::ScopedLatency _metrics_scope_((rec_ptr), (op))
#else
#  define METRICS_SCOPE(rec_ptr, op) ((void)0)
#endif

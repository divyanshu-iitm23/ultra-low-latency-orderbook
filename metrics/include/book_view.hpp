#pragma once
// book_view.hpp - a low-frequency depth side-channel from the producer to the aggregator.
//
// The aggregator only sees the SPSC event ring, and a top-N ladder is far too big for a
// 32-byte MetricsEvent. Depth is also low-rate (~5-10 Hz, off the hot path), so instead of
// streaming it through the ring we publish it through a tiny single-writer/single-reader
// SEQLOCK: the producer (which owns the OrderBook) writes a BookView periodically; the
// aggregator reads a consistent copy lock-free (retrying only if it caught a write in
// flight). This mirrors how the aggregator already polls the producer's drops atomic.
#include "metrics_snapshot.hpp"     // DepthLevel, MAX_DEPTH
#include <atomic>
#include <cstdint>

namespace metrics {

struct BookView {
    uint32_t   nbids = 0, nasks = 0;
    DepthLevel bids[MAX_DEPTH];
    DepthLevel asks[MAX_DEPTH];
};

// Single-writer / single-reader seqlock over a POD BookView.
class BookPublisher {
public:
    // producer side: publish a fresh view (odd seq while writing, even when done)
    void publish(const BookView& v) {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        data_ = v;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }

    // consumer side: copy a consistent snapshot; false if no stable read (writer too hot)
    bool read(BookView& out) const {
        for (int tries = 0; tries < 16; ++tries) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;                 // write in progress
            std::atomic_thread_fence(std::memory_order_acquire);
            out = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_acquire) == s1) return true;
        }
        return false;
    }

private:
    std::atomic<uint32_t> seq_{0};
    BookView data_{};
};

} // namespace metrics

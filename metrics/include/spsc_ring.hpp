#pragma once
// spsc_ring.hpp - single-producer / single-consumer bounded lock-free ring buffer

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace metrics {

// toggle the cache-line padding for the false-sharing A/B
#ifdef SPSC_NO_PAD
#  define SPSC_ALIGN
#else
#  define SPSC_ALIGN alignas(64)
#endif

template <typename T, std::size_t Cap>
class SPSCRing {
    static_assert((Cap & (Cap - 1)) == 0 && Cap >= 2, "Cap must be a power of two >= 2");
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    static constexpr std::size_t MASK = Cap - 1;

public:
    SPSCRing() = default;
    SPSCRing(const SPSCRing&) = delete;
    SPSCRing& operator=(const SPSCRing&) = delete;

    // PRODUCER ONLY - Returns false if the ring is full (caller decides to drop or retry).
    bool try_push(const T& v) {
        const std::size_t head = head_.load(std::memory_order_relaxed);  // only we write head_
        if (head - tail_cache_ >= Cap) {                 // cache asks if it's full
            tail_cache_ = tail_.load(std::memory_order_acquire);         // refresh from consumer
            if (head - tail_cache_ >= Cap) return false;                 // genuinely full
        }
        buf_[head & MASK] = v;                           // write slot...
        head_.store(head + 1, std::memory_order_release);// ...then publish (release)
        return true;
    }

    // CONSUMER ONLY - Returns false if the ring is empty.
    bool try_pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);  // only we write tail_
        if (tail == head_cache_) {                       // cache says possibly empty?
            head_cache_ = head_.load(std::memory_order_acquire);         // refresh from producer
            if (tail == head_cache_) return false;                       // genuinely empty
        }
        out = buf_[tail & MASK];                         // read slot (after acquire on head_)...
        tail_.store(tail + 1, std::memory_order_release);// ...then publish consumption
        return true;
    }

    // Approximate size
    std::size_t size_approx() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    static constexpr std::size_t capacity() { return Cap; }

private:
    // Producer-owned line: head_ (atomic) + its cached view of tail_.
    SPSC_ALIGN std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_{0};
    // Consumer-owned line: tail_ (atomic) + its cached view of head_.
    SPSC_ALIGN std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_{0};
    // The storage.
    SPSC_ALIGN T buf_[Cap];
};

} // namespace metrics
#pragma once
//
// rdtsc_timer.hpp - low-overhead timing primitives for benchmarking.
//
// KEY FACT: on modern CPUs the TSC is an "invariant" high-resolution WALL CLOCK
// that ticks at a fixed rate (~the base frequency), NOT a per-cycle counter.
// So we calibrate its real tick-rate at runtime and convert ticks -> nanoseconds.
//
// x86-only (uses RDTSC/RDTSCP). On other ISAs it falls back to a no-op clock.
//
#include <cstdint>
#include <ctime>
#include <vector>
#include <algorithm>

#if defined(__x86_64__) || defined(__i386__)
  #include <x86intrin.h>
  #define ORDERBOOK_HAS_RDTSC 1
#else
  #define ORDERBOOK_HAS_RDTSC 0
#endif

namespace orderbook {

#if ORDERBOOK_HAS_RDTSC
// Read TSC at the START of a measured region.
// lfence on both sides stops the read from drifting across the boundary
// under out-of-order execution.
static inline uint64_t tscStart() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// Read TSC at the END of a measured region.
// rdtscp waits for all prior instructions to retire before reading;
// the trailing lfence blocks later instructions from creeping in.
static inline uint64_t tscEnd() {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}
#else
static inline uint64_t tscStart() { return 0; }
static inline uint64_t tscEnd()   { return 0; }
#endif

// Stop the optimizer from deleting work whose result is otherwise unused.
// (Same idea as Google Benchmark's DoNotOptimize.)
template <typename T>
static inline void doNotOptimize(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

// Nanoseconds between two CLOCK_MONOTONIC samples.
static inline uint64_t nsBetween(const timespec& a, const timespec& b) {
    return static_cast<uint64_t>(b.tv_sec - a.tv_sec) * 1000000000ULL
         + static_cast<uint64_t>(b.tv_nsec - a.tv_nsec);
}

// Calibrated TSC clock: measures ticks-per-second ONCE against CLOCK_MONOTONIC,
// then converts raw tick deltas to nanoseconds.
class TscClock {
public:
    TscClock() : tsc_hz_(calibrate()) {}

    double   ticksToNs(uint64_t ticks) const {
        return static_cast<double>(ticks) * 1e9 / static_cast<double>(tsc_hz_);
    }
    uint64_t hz() const { return tsc_hz_; }

private:
    static uint64_t calibrate() {
#if ORDERBOOK_HAS_RDTSC
        timespec t0{}, t1{};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t c0 = tscStart();
        // Busy-wait ~200 ms of real time (busy, not sleep, to stay on-core).
        do {
            clock_gettime(CLOCK_MONOTONIC, &t1);
        } while (nsBetween(t0, t1) < 200000000ULL);
        uint64_t c1 = tscEnd();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs = nsBetween(t0, t1) / 1e9;
        return static_cast<uint64_t>(static_cast<double>(c1 - c0) / secs);
#else
        return 1000000000ULL; // 1 tick == 1 ns fallback
#endif
    }
    uint64_t tsc_hz_;
};

// Irreducible cost of a back-to-back start/end pair, in ticks.
// We take the MIN over many tries = the least-perturbed (cleanest) sample,
// then subtract it from per-op measurements so we time the OP, not the clock.
static inline uint64_t measureTimerOverhead(int iters = 200000) {
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < iters; ++i) {
        uint64_t s = tscStart();
        uint64_t e = tscEnd();
        uint64_t d = e - s;
        if (d < best) best = d;
    }
    return (best == UINT64_MAX) ? 0 : best;
}

// Percentile from an ALREADY-SORTED vector.
static inline uint64_t percentile(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(sorted.size() * p);
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

} // namespace orderbook
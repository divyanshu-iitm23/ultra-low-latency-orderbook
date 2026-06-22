#pragma once
// log-linear histogram for latency percentiles.
// this is the data-structure that the aggregator thread owns,
// exact min/max/sum are tracked separately so means and the worst-case tail are
// reported,
//
// the design is single-threaded: one aggregator thread owns its histograms.
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace metrics {

class LatencyHistogram {
    static constexpr uint32_t SUB_BITS = 5;            // 32 sub-buckets,
    static constexpr uint32_t SUB      = 1u << SUB_BITS;
    // values are uint32_t ticks; MSB position spans [SUB_BITS .. 31] in the log region.
    static constexpr uint32_t NB = (32 - SUB_BITS + 1) * SUB;   // 896 buckets

    // value -> bucket index (monotonic in value)
    static uint32_t bucketOf(uint32_t v) {
        if (v < SUB) return v;                          // bottom octave is linear/exact
        const uint32_t hb    = 31u - (uint32_t)__builtin_clz(v);   // MSB position
        const uint32_t shift = hb - SUB_BITS;
        const uint32_t sub   = (v >> shift) & (SUB - 1);
        return (hb - SUB_BITS + 1) * SUB + sub;
    }
    // bucket index -> lower-bound value of that bucket (representative for percentiles)
    static uint32_t valueOf(uint32_t b) {
        if (b < SUB) return b;
        const uint32_t octave = b / SUB;                // = hb - SUB_BITS + 1
        const uint32_t sub    = b % SUB;
        const uint32_t shift  = octave - 1;
        return (SUB + sub) << shift;
    }

public:
    LatencyHistogram() { reset(); }

    inline void record(uint32_t v) {
        ++bucket_[bucketOf(v)];
        ++count_;
        sum_ += v;
        if (v > max_) max_ = v;
        if (v < min_) min_ = v;
    }

    // Fold `o` into this (used to roll an interval window into the cumulative total).
    void merge(const LatencyHistogram& o) {
        for (uint32_t b = 0; b < NB; ++b) bucket_[b] += o.bucket_[b];
        count_ += o.count_;
        sum_   += o.sum_;
        if (o.max_ > max_) max_ = o.max_;
        if (o.min_ < min_) min_ = o.min_;
    }

    void reset() {
        std::memset(bucket_, 0, sizeof(bucket_));
        count_ = 0; sum_ = 0; max_ = 0; min_ = UINT32_MAX;
    }

    // p in [0,100]. Returns the representative value (bucket lower bound) at that rank.
    uint32_t percentile(double p) const {
        if (count_ == 0) return 0;
        uint64_t rank = (uint64_t)std::ceil(p * 0.01 * (double)count_);
        if (rank == 0) rank = 1;
        if (rank > count_) rank = count_;
        uint64_t acc = 0;
        for (uint32_t b = 0; b < NB; ++b) {
            acc += bucket_[b];
            if (acc >= rank) return valueOf(b);
        }
        return max_;
    }

    uint64_t count() const { return count_; }
    uint32_t max()   const { return max_; }
    uint32_t min()   const { return count_ ? min_ : 0; }
    double   mean()  const { return count_ ? (double)sum_ / (double)count_ : 0.0; }

private:
    uint64_t bucket_[NB];
    uint64_t count_;
    uint64_t sum_;
    uint32_t max_;
    uint32_t min_;
};

} // namespace metrics

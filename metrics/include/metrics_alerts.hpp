#pragma once
// metrics_alerts.hpp - operational alert rules the aggregator evaluates each render cycle.
//
// evaluateAlerts() is a PURE function of a MetricsSnapshot plus thresholds: it reads only
// the snapshot's own fields and writes the result back into s.alerts[]. That makes it
// unit-testable in isolation and keeps the rules in one place, while the alerts ride the
// same snapshot the console / JSON / dashboard already carry. Runs on Core B, off the hot path.
#include "metrics_snapshot.hpp"
#include <cstdint>
#include <cstdio>

namespace metrics {

struct AlertConfig {
    double  p99_ns     = 5000.0;   // warn if any op's window p99 exceeds this (ns); 0 = off
    int64_t spread_max = 0;        // warn if book spread exceeds this (ticks);      0 = off
    bool    on_drops   = true;     // warn when new drops appear since the last cycle
    bool    on_crossed = true;     // CRIT if best_bid >= best_ask (should never happen)
};

// Fill s.alerts from s's own fields. `last_drops` carries the drop count across cycles so
// the drops rule fires on the *delta* (new drops this interval), not the sticky cumulative.
inline void evaluateAlerts(MetricsSnapshot& s, const AlertConfig& cfg, uint64_t& last_drops) {
    s.num_alerts = 0;
    const uint32_t CAP = (uint32_t)(sizeof(s.alerts) / sizeof(s.alerts[0]));
    auto add = [&](const char* level, const char* code) -> Alert* {
        if (s.num_alerts >= CAP) return nullptr;
        Alert& a = s.alerts[s.num_alerts++];
        a.level = level; a.code = code; a.text[0] = 0;
        return &a;
    };

    // crossed-book: the "should never fire" correctness alarm (both sides must be present)
    if (cfg.on_crossed && s.have_book && s.best_bid > 0 && s.best_ask > 0 &&
        s.best_bid >= s.best_ask) {
        if (Alert* a = add("crit", "crossed"))
            snprintf(a->text, sizeof a->text, "crossed book: bid %lld >= ask %lld",
                     (long long)s.best_bid, (long long)s.best_ask);
    }

    // spread blowout
    if (cfg.spread_max > 0 && s.have_book && s.best_bid > 0 && s.best_ask > 0) {
        const int64_t spread = s.best_ask - s.best_bid;
        if (spread > cfg.spread_max) {
            if (Alert* a = add("warn", "spread"))
                snprintf(a->text, sizeof a->text, "spread %lld > %lld",
                         (long long)spread, (long long)cfg.spread_max);
        }
    }

    // drops since the last cycle (ring overflow -> the consumer is falling behind)
    if (cfg.on_drops && s.drops > last_drops) {
        if (Alert* a = add("warn", "drops"))
            snprintf(a->text, sizeof a->text, "%llu new drops (ring overflow)",
                     (unsigned long long)(s.drops - last_drops));
    }
    last_drops = s.drops;

    // per-op p99 breach (only meaningful when latencies are calibrated to ns)
    if (cfg.p99_ns > 0 && s.unit[0] == 'n') {                 // "ns"
        for (uint32_t i = 0; i < s.num_ops; ++i) {
            if (s.ops[i].p99 > cfg.p99_ns) {
                if (Alert* a = add("warn", "p99"))
                    snprintf(a->text, sizeof a->text, "%s p99 %.0f ns > %.0f ns",
                             s.ops[i].name, s.ops[i].p99, cfg.p99_ns);
            }
        }
    }
}

} // namespace metrics

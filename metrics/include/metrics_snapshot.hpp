#pragma once
// metrics_snapshot.hpp - a point-in-time view the aggregator produces each render
// cycle, plus a compact JSON serializer
// This lives on the CONSUMER side (Core B), renders at ~5-10 Hz, off the hot path,
// The same struct feeds the console "top" view, the NDJSON logger, and the UDP publisher
//
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>

namespace metrics {

struct OpStat {
    const char* name = "?";   // add/ cancel/ modify/ match/ market
    uint64_t    count = 0;    // all-time count for this op
    double      ops_per_s = 0;// recent-window rate
    double      p50 = 0, p99 = 0, p999 = 0, max = 0;   // recent p50/p99/p99.9, all-time max
};


struct Alert {
    const char* level = "warn";   // "warn" | "crit"
    const char* code  = "?";      // "p99" | "spread" | "drops" | "crossed"
    char        text[80] = {0};   // human-readable message
};

struct MetricsSnapshot {
    double      uptime_s = 0;
    bool        final    = false;
    uint64_t    events   = 0;    // consumed
    uint64_t    trades   = 0;
    uint64_t    snaps    = 0;
    uint64_t    drops    = 0;
    const char* unit     = "ns"; // latency unit: "ns" when TSC-calibrated, else "tick"
    bool        have_book = false;
    int64_t     best_bid = 0, best_ask = 0, last_px = 0;
    uint64_t    volume   = 0;
    uint32_t    num_ops  = 0;
    OpStat      ops[8];
    uint32_t    num_alerts = 0;
    Alert       alerts[8];
};

// Bounded append helper: never writes past `cap`, keeps `n` clamped, buf stays NUL-terminated.
inline void jappend(char* buf, size_t cap, size_t& n, const char* fmt, ...) {
    if (n >= cap) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    n += (size_t)w;
    if (n >= cap) n = cap - 1;   // truncated; vsnprintf already NUL-terminated at cap-1
}

// Serialize to a compact single-line JSON object (NDJSON / single-UDP-datagram friendly).
// Returns the number of bytes written (excluding the NUL).
inline size_t writeJson(const MetricsSnapshot& s, char* buf, size_t cap) {
    size_t n = 0;
    jappend(buf, cap, n,
        "{\"t\":%.2f,\"final\":%s,\"events\":%llu,\"trades\":%llu,\"snaps\":%llu,"
        "\"drops\":%llu,\"unit\":\"%s\"",
        s.uptime_s, s.final ? "true" : "false",
        (unsigned long long)s.events, (unsigned long long)s.trades,
        (unsigned long long)s.snaps, (unsigned long long)s.drops, s.unit);

    if (s.have_book)
        jappend(buf, cap, n,
            ",\"book\":{\"bid\":%lld,\"ask\":%lld,\"spread\":%lld,\"last\":%lld,\"vol\":%llu}",
            (long long)s.best_bid, (long long)s.best_ask,
            (long long)(s.best_ask - s.best_bid), (long long)s.last_px,
            (unsigned long long)s.volume);
    else
        jappend(buf, cap, n, ",\"book\":null");

    jappend(buf, cap, n, ",\"ops\":[");
    for (uint32_t i = 0; i < s.num_ops; ++i) {
        const OpStat& o = s.ops[i];
        jappend(buf, cap, n,
            "%s{\"op\":\"%s\",\"ops_s\":%.0f,\"count\":%llu,"
            "\"p50\":%.1f,\"p99\":%.1f,\"p999\":%.1f,\"max\":%.1f}",
            i ? "," : "", o.name, o.ops_per_s, (unsigned long long)o.count,
            o.p50, o.p99, o.p999, o.max);
    }
    jappend(buf, cap, n, "],\"alerts\":[");
    for (uint32_t i = 0; i < s.num_alerts; ++i) {
        const Alert& a = s.alerts[i];
        jappend(buf, cap, n, "%s{\"level\":\"%s\",\"code\":\"%s\",\"text\":\"%s\"}",
                i ? "," : "", a.level, a.code, a.text);
    }
    jappend(buf, cap, n, "]}");
    return n;
}

} // namespace metrics

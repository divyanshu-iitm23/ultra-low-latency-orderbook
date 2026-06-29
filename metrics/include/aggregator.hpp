#pragma once
// aggregator.hpp - the consumer side of the metrics pipeline.
//
// One thread drains the SPSC ring and folds every event into per-op latency
// histograms, book state, and trade/drop counters. Because it is the SOLE owner
// of those structures, no locks are needed: read and write happen on one thread.
//
// This thread also renders a `top`-style live readout on a wall-clock cadence:
//   per op  ->  p50 / p99 / p99.9 (recent window)  ·  max (sticky, all-time)  ·  ops/s
//   header  ->  uptime · events/s · drops · trades · book bid/ask/spread
//
// Decoupled from the engine: it needs only the ring, an optional drop counter to
// poll, and a ticks->ns factor for display. That makes it a drop-in for the real
// hot path (construct a MetricsRecorder there, hand its ring + dropsCounter here).
#include "metrics_config.hpp"
#include "metrics_event.hpp"
#include "latency_histogram.hpp"
#include "metrics_snapshot.hpp"             // json snapshot serialization
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <unistd.h>

namespace metrics {

class Aggregator {
public:
    static constexpr uint32_t NUM_OPS = 5;   // Add, Cancel, Modify, Match, Market

    // ring        : the SPSC ring this aggregator drains (producer is the engine).
    // drops       : optional live handle to the producer's drop counter (may be null).
    // ns_per_tick : TSC ticks -> nanoseconds; 0 => display raw ticks.
    // render_hz   : live-readout refresh rate.
    Aggregator(EventRing& ring,
               const std::atomic<uint64_t>* drops = nullptr,
               double ns_per_tick = 0.0,
               double render_hz   = 5.0)
        : ring_(&ring), drops_(drops), ns_per_tick_(ns_per_tick),
          render_ns_((uint64_t)(1e9 / (render_hz > 0 ? render_hz : 1.0))),
          use_ansi_(::isatty(STDOUT_FILENO) != 0) {}

    void stop() { running_.store(false, std::memory_order_release); }

    // called once per render cycle with the same data the console shows,
    // off the hot path (Core B, ~render_hz). The NDJSON logger / UDP publisher attach here..
    void setSnapshotSink(std::function<void(const MetricsSnapshot&)> s) { sink_ = std::move(s); }
    void setConsole(bool on) { console_ = on; }

    // Blocking drain+render loop. Runs until stop(); on exit it drains whatever
    // remains and paints a final frame so no tail events are lost.
    void run() {
        const uint64_t t_start = nowNs();
        uint64_t next_render = t_start + render_ns_;
        last_render_ns_ = t_start;

        MetricsEvent e;
        while (running_.load(std::memory_order_acquire)) {
            bool got = false;
            for (int i = 0; i < kDrainBurst; ++i) {
                if (!ring_->try_pop(e)) break;
                ingest(e);
                got = true;
            }
            const uint64_t t = nowNs();
            if (t >= next_render) {
                render(t, t_start, /*final=*/false);
                next_render = t + render_ns_;
            }
            if (!got) cpuRelax();   // ring empty -> spin politely
        }

        // final drain so accounting is exact at shutdown
        while (ring_->try_pop(e)) ingest(e);
        render(nowNs(), t_start, /*final=*/true);
    }

    // Totaling (consumed + drops == produced).
    uint64_t consumed() const { return consumed_; }

private:
    // ingest() -- folding one event.
    void ingest(const MetricsEvent& e) {
        ++consumed_;
        switch (e.etype) {
            case EventType::Latency: {
                const uint32_t op = (uint32_t)e.otype;
                if (op < NUM_OPS) win_[op].record(e.latency);
                break;
            }
            case EventType::Snapshot:
                best_bid_ = e.payload.snap.best_bid;
                best_ask_ = e.payload.snap.best_ask;
                ++snaps_;
                break;
            case EventType::Trade:
                last_px_ = e.payload.trade.price;
                volume_ += e.payload.trade.qty;
                ++trades_;
                break;
        }
    }

    // render() -- build the per-cycle snapshot, paint the console view, fan out to the sink.
    void render(uint64_t t_now, uint64_t t_start, bool final) {
        const double win_s = (t_now - last_render_ns_) / 1e9;   // window for ops/s
        const double up_s  = (t_now - t_start)        / 1e9;
        last_render_ns_ = t_now;

        const uint64_t drops = drops_ ? drops_->load(std::memory_order_relaxed) : 0;

        // header / book fields are identical for console and snapshot
        MetricsSnapshot snap;
        snap.uptime_s = up_s; snap.final = final;
        snap.events = consumed_; snap.trades = trades_; snap.snaps = snaps_; snap.drops = drops;
        snap.unit = (ns_per_tick_ > 0.0) ? "ns" : "tick";
        if (snaps_) {
            snap.have_book = true;
            snap.best_bid = best_bid_; snap.best_ask = best_ask_;
            snap.last_px = last_px_;   snap.volume = volume_;
        }

        char buf[4096];
        int n = 0;
        if (console_) {
            if (use_ansi_) n += snprintf(buf + n, sizeof(buf) - n, "\033[H");  // cursor home
            n += snprintf(buf + n, sizeof(buf) - n,
                "  ORDER-BOOK LIVE METRICS%s   up %6.1fs   refresh %.0fHz\n",
                final ? "  (final)" : "", up_s, 1e9 / (double)render_ns_);
            n += snprintf(buf + n, sizeof(buf) - n,
                "  events %-12llu  trades %-8llu  snaps %-8llu  drops %-10llu\n",
                (unsigned long long)consumed_, (unsigned long long)trades_,
                (unsigned long long)snaps_, (unsigned long long)drops);
            if (snaps_) {
                const long long spread = (long long)(best_ask_ - best_bid_);
                n += snprintf(buf + n, sizeof(buf) - n,
                    "  book   bid %-12lld ask %-12lld spread %-8lld  last_px %-12lld vol %llu\n",
                    (long long)best_bid_, (long long)best_ask_, spread,
                    (long long)last_px_, (unsigned long long)volume_);
            }
            n += snprintf(buf + n, sizeof(buf) - n,
                "\n  %-8s %12s %12s %10s %10s %10s %10s\n",
                "OP", "ops/s", "count", "p50", "p99", "p99.9", "max");
        }

        for (uint32_t op = 0; op < NUM_OPS; ++op) {
            cum_[op].merge(win_[op]);                 // roll window into all-time
            if (cum_[op].count() == 0) continue;      // never seen -> skip the row

            const uint64_t wc    = win_[op].count();
            const double   opsps = win_s > 0 ? (double)wc / win_s : 0.0;
            const uint32_t t50  = win_[op].percentile(50.0);
            const uint32_t t99  = win_[op].percentile(99.0);
            const uint32_t t999 = win_[op].percentile(99.9);
            const uint32_t tmax = cum_[op].max();     // sticky worst case

            snap.ops[snap.num_ops++] = { opName(op), cum_[op].count(), opsps,
                                         toNs(t50), toNs(t99), toNs(t999), toNs(tmax) };

            if (console_) {
                char p50[24], p99[24], p999[24], mx[24];
                fmt(t50, p50, sizeof p50); fmt(t99, p99, sizeof p99);
                fmt(t999, p999, sizeof p999); fmt(tmax, mx, sizeof mx);
                n += snprintf(buf + n, sizeof(buf) - n,
                    "  %-8s %12.0f %12llu %10s %10s %10s %10s\n",
                    opName(op), opsps, (unsigned long long)cum_[op].count(),
                    p50, p99, p999, mx);
            }
            win_[op].reset();                         // start a fresh window
        }

        if (console_) {
            if (use_ansi_) n += snprintf(buf + n, sizeof(buf) - n, "\033[J");  // clear below
            fwrite(buf, 1, (size_t)n, stdout);
            fflush(stdout);
        }

        if (sink_) sink_(snap);                       // NDJSON logger / UDP publisher
    }

    // Ticks -> nanoseconds for the snapshot; raw ticks when uncalibrated.
    double toNs(uint32_t ticks) const {
        return ns_per_tick_ > 0.0 ? (double)ticks * ns_per_tick_ : (double)ticks;
    }

    // Format a tick value into the display buffer (ns/us/ms if calibrated, else raw ticks).
    void fmt(uint32_t ticks, char* out, size_t cap) const {
        if (ns_per_tick_ <= 0.0) { snprintf(out, cap, "%u tk", ticks); return; }
        const double ns = (double)ticks * ns_per_tick_;
        if      (ns < 1e3) snprintf(out, cap, "%.0f ns", ns);
        else if (ns < 1e6) snprintf(out, cap, "%.2f us", ns / 1e3);
        else               snprintf(out, cap, "%.2f ms", ns / 1e6);
    }

    static const char* opName(uint32_t op) {
        switch ((OpType)op) {
            case OpType::Add:    return "add";
            case OpType::Cancel: return "cancel";
            case OpType::Modify: return "modify";
            case OpType::Match:  return "match";
            case OpType::Market: return "market";
        }
        return "?";
    }

    static uint64_t nowNs() {
        timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    static void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }

    static constexpr int kDrainBurst = 4096;   // pop in bursts to amortise the loop

    EventRing* ring_;
    const std::atomic<uint64_t>* drops_;
    double   ns_per_tick_;
    uint64_t render_ns_;
    bool     use_ansi_;
    bool     console_ = true;
    std::function<void(const MetricsSnapshot&)> sink_;

    std::atomic<bool> running_{true};

    LatencyHistogram win_[NUM_OPS];   // recent window (reset each render)
    LatencyHistogram cum_[NUM_OPS];   // all-time

    uint64_t consumed_ = 0;
    uint64_t trades_   = 0;
    uint64_t snaps_    = 0;
    uint64_t volume_   = 0;
    int64_t  best_bid_ = 0;
    int64_t  best_ask_ = 0;
    int64_t  last_px_  = 0;
    uint64_t last_render_ns_ = 0;
};

} // namespace metrics

// benchmark_false_sharing.cpp - the False-sharing A/B for the SPSC ring.
//
// This benchmark streams N items through the ring on two pinned cores with a
// retry-on-full producer (no drops -> pure throughput). The SAME source is built twice:
//   bench_false_sharing_padded   (default)
//   bench_false_sharing_nopad    (-DSPSC_NO_PAD)
// and the delta in items/sec is the cost of the two cores fighting over one cache line.
//

#include "spsc_ring.hpp"
#include "metrics_event.hpp"

#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <sched.h>

using namespace metrics;

#ifndef FS_RING_CAP
#define FS_RING_CAP 1024            // small enough that both sides stay engaged
#endif

static void pinToCore(int core) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core, &s);
    sched_setaffinity(0, sizeof(s), &s);
}
static uint64_t now_ns() {
    timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

static SPSCRing<MetricsEvent, FS_RING_CAP> ring;   // shared; reused across reps (empty between)
static volatile uint64_t g_sink = 0;               // keep the consumer's reads alive

int main(int argc, char** argv) {
    const int pcore = (argc > 1) ? atoi(argv[1]) : 2;
    const int ccore = (argc > 2) ? atoi(argv[2]) : 3;
    const uint64_t N = (argc > 3) ? strtoull(argv[3], nullptr, 10) : 50000000ULL;
    const int REPS = 5;

#ifdef SPSC_NO_PAD
    const char* mode = "NO-PAD  (head_/tail_/buf_ packed -> false sharing)";
#else
    const char* mode = "PADDED  (head_ / tail_ / buf_ on separate 64B lines)";
#endif

    printf("=== SPSC ring false-sharing A/B ===\n");
    printf("build mode : %s\n", mode);
    printf("ring cap   : %d slots x %zu B\n", (int)FS_RING_CAP, sizeof(MetricsEvent));
    printf("cores      : producer %d, consumer %d\n", pcore, ccore);
    printf("workload   : %llu items/rep, median of %d, retry-on-full (no drops)\n\n",
           (unsigned long long)N, REPS);

    std::vector<double> mops;
    for (int r = 0; r < REPS; ++r) {
        std::atomic<bool> consumer_ready{false};
        uint64_t acc = 0;

        std::thread cons([&]{
            pinToCore(ccore);
            MetricsEvent e;
            consumer_ready.store(true, std::memory_order_release);
            uint64_t c = 0, local = 0;
            while (c < N) {
                if (ring.try_pop(e)) { local += e.tsc; ++c; }
                else cpuRelax();
            }
            acc = local;
        });

        while (!consumer_ready.load(std::memory_order_acquire)) {}
        pinToCore(pcore);

        MetricsEvent e{};
        e.latency = 7; e.etype = EventType::Latency; e.otype = OpType::Add; e.flags = 0;

        const uint64_t t0 = now_ns();
        for (uint64_t i = 0; i < N; ++i) {
            e.tsc = i;
            while (!ring.try_push(e)) cpuRelax();
        }
        cons.join();
        const uint64_t t1 = now_ns();

        g_sink += acc;
        const double secs = (t1 - t0) / 1e9;
        const double m = (double)N / secs / 1e6;
        mops.push_back(m);
        printf("  rep %d: %7.1f M items/s   (%.2f ns/item)\n", r + 1, m, secs * 1e9 / (double)N);
    }

    std::sort(mops.begin(), mops.end());
    printf("\nmedian throughput: %.1f M items/s   [%s]\n",
           mops[mops.size() / 2], mode);
    return 0;
}

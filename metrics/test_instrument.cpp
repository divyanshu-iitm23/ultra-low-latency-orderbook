#include "metrics_recorder.hpp"
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdint>
using namespace metrics;
typedef unsigned long long ull;

static volatile uint64_t sink = 0;
static uint64_t do_work(uint64_t x) { uint64_t a = x; for (int i=0;i<20;++i) a = a*6364136223846793005ULL + 1; sink += a; return a; }

int main() {
    EventRing ring;
    MetricsRecorder rec(ring, 0);                 // mask 0 -> record EVERY op
    std::atomic<bool> done{false};
    std::atomic<ull> c_total{0}, c_lat{0}, c_snap{0}, c_trade{0};
    std::atomic<ull> a_lmin{~0ull}, a_lmax{0}, a_lsum{0}, a_ln{0};

    std::thread consumer([&]{
        MetricsEvent e; ull total=0,lat=0,snap=0,trade=0,lmin=~0ull,lmax=0,lsum=0,ln=0;
        auto take=[&](const MetricsEvent& e){
            ++total;
            if (e.etype==EventType::Latency){ ++lat; uint32_t L=e.latency;
                if(L<lmin)lmin=L; if(L>lmax)lmax=L; lsum+=L; ++ln; }
            else if (e.etype==EventType::Snapshot) ++snap;
            else if (e.etype==EventType::Trade)    ++trade;
        };
        for(;;){
            if (ring.try_pop(e)) { take(e); continue; }
            if (done.load(std::memory_order_acquire)) { while (ring.try_pop(e)) take(e); break; }
            __builtin_ia32_pause();
        }
        c_total=total; c_lat=lat; c_snap=snap; c_trade=trade;
        a_lmin=lmin; a_lmax=lmax; a_lsum=lsum; a_ln=ln;
    });

    const ull N = 2'000'000;
    ull att_lat=0, att_snap=0, att_trade=0;
    for (ull i=0;i<N;++i){
        { ScopedLatency _l(rec, OpType::Add);    do_work(i);   ++att_lat; }
        { ScopedLatency _l(rec, OpType::Cancel); do_work(i^7); ++att_lat; }
        if ((i % 1000) == 0) {
            rec.recordTrade(100500 + (int64_t)(i%50), 5, (uint32_t)(i%9)); ++att_trade;
            rec.recordSnapshot(28753, 28758);                              ++att_snap;
        }
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    ull attempted = att_lat + att_snap + att_trade;
    ull consumed  = c_total.load();
    ull drops     = rec.drops();
    int fail=0; auto CHK=[&](bool c,const char*m){ if(!c){printf("FAIL: %s\n",m);++fail;} };

    printf("attempted=%llu  consumed=%llu  drops=%llu\n", attempted, consumed, drops);
    printf("  by type: latency=%llu  snapshot=%llu  trade=%llu\n",
           c_lat.load(), c_snap.load(), c_trade.load());
    printf("  latency ticks: min=%llu max=%llu mean=%.1f over %llu samples\n",
           a_lmin.load(), a_lmax.load(), a_ln.load()? (double)a_lsum.load()/a_ln.load():0.0, a_ln.load());

    CHK(consumed + drops == attempted, "accounting: consumed + drops == attempted (nothing lost)");
    CHK(att_trade > 0 && att_snap > 0,  "trade + snapshot events were produced");
    CHK(a_ln.load() > 0,                "latency events were produced");
    CHK(a_lmin.load() > 0,              "latencies are non-zero (real timing happened)");
    CHK(a_lmax.load() < 1'000'000'000ull, "latencies are sane (< ~0.3s, no garbage)");
    printf(fail? "\n%d FAILURES\n" : "\nINSTRUMENTATION OK (events flow, accounting holds)\n", fail);
    return fail?1:0;
}

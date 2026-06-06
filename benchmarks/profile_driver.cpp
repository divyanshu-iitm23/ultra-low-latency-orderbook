// perf profiling
// building : g++ -O3 -g -fno-omit-frame-pointer -std=c++17 -march=native \
//          -Iinclude src/orderbook.cpp profile_driver.cpp -o profile_driver
// running :   taskset -c 3 ./profile_driver 200000000 3
#include "orderbook.hpp"
#include <random>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <sched.h>
using namespace orderbook;

static void pin(int core) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core, &s);
    sched_setaffinity(0, sizeof(s), &s);
}

int main(int argc, char** argv) {
    long N    = (argc > 1) ? atol(argv[1]) : 200000000L;
    int  core = (argc > 2) ? atoi(argv[2]) : 3;
    pin(core);

    OrderBook book(1, 50000, 1 << 21);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> pr(9000, 11000), qd(100, 1000), sd(0, 1), ad(0, 9);

    // pre-create every price level -> steady state (adds land on existing levels)
    for (Price p = 9000; p <= 11000; ++p) book.addOrder(Side::BUY, p, 1);

    const size_t CAP = 100000;          // keep the live order count bounded
    std::vector<OrderId> live;
    live.reserve(CAP * 2);

    long adds = 0, cancels = 0;
    unsigned long long sink = 0;
    for (long i = 0; i < N; ++i) {
        bool doCancel = !live.empty() && (live.size() > CAP || ad(rng) < 5);
        if (doCancel) {
            size_t k = rng() % live.size();
            if (book.cancelOrder(live[k])) {
                ++cancels; live[k] = live.back(); live.pop_back();
            }
        } else {
            OrderId id = book.addOrder(sd(rng) ? Side::SELL : Side::BUY, pr(rng), qd(rng));
            if (id) live.push_back(id);
            ++adds;
        }
        sink += book.getActiveBids();
    }
    fprintf(stderr, "adds=%ld cancels=%ld live=%zu sink=%llu\n",
            adds, cancels, live.size(), sink);
    return 0;
}
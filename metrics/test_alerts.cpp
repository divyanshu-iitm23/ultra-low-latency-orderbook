// test_alerts.cpp - deterministic check of the operational alert rules.
//

#include "metrics_alerts.hpp"
#include <cstdio>
#include <cstring>

using namespace metrics;

static int failures = 0;
#define REQUIRE(c, m) do { if (!(c)) { printf("FAIL: %s\n", (m)); ++failures; } } while (0)

// is there an alert with this code (and, if given, this level)?
static const Alert* find(const MetricsSnapshot& s, const char* code, const char* level = nullptr) {
    for (uint32_t i = 0; i < s.num_alerts; ++i)
        if (!strcmp(s.alerts[i].code, code) && (!level || !strcmp(s.alerts[i].level, level)))
            return &s.alerts[i];
    return nullptr;
}

int main() {
    AlertConfig cfg;                 // defaults: p99 5us, drops on, crossed on, spread off
    cfg.spread_max = 10;             // enable spread rule for the test
    uint64_t last_drops = 0;

    // clean snapshot: no alerts
    {
        MetricsSnapshot s;
        s.unit = "ns"; s.have_book = true; s.best_bid = 99999; s.best_ask = 100001; // spread 2
        s.num_ops = 1; s.ops[0] = { "add", 100, 0, 20, 200, 400, 1000 };            // p99 200ns
        uint64_t ld = 0;
        evaluateAlerts(s, cfg, ld);
        REQUIRE(s.num_alerts == 0, "clean snapshot raises no alerts");
    }

    // crossed book -> crit
    {
        MetricsSnapshot s;
        s.unit = "ns"; s.have_book = true; s.best_bid = 100002; s.best_ask = 100000; // crossed
        uint64_t ld = 0;
        evaluateAlerts(s, cfg, ld);
        const Alert* a = find(s, "crossed", "crit");
        REQUIRE(a != nullptr, "crossed book raises a CRIT 'crossed' alert");
    }

    // spread blowout -> warn
    {
        MetricsSnapshot s;
        s.unit = "ns"; s.have_book = true; s.best_bid = 100000; s.best_ask = 100050; // spread 50 > 10
        uint64_t ld = 0;
        evaluateAlerts(s, cfg, ld);
        REQUIRE(find(s, "spread", "warn") != nullptr, "wide spread raises a 'spread' warn");
        REQUIRE(find(s, "crossed") == nullptr, "non-crossed book raises no crossed alert");
    }

    // p99 breach -> warn, per op
    {
        MetricsSnapshot s;
        s.unit = "ns";
        s.num_ops = 2;
        s.ops[0] = { "add",    100, 0, 20,  200,   400,  1000 };   // ok
        s.ops[1] = { "cancel", 100, 0, 50, 8000,  9000, 12000 };   // p99 8us > 5us
        uint64_t ld = 0;
        evaluateAlerts(s, cfg, ld);
        const Alert* a = find(s, "p99", "warn");
        REQUIRE(a != nullptr, "op p99 over threshold raises a 'p99' warn");
        REQUIRE(a && strstr(a->text, "cancel"), "the p99 alert names the offending op (cancel)");
        REQUIRE(s.num_alerts == 1, "only the breaching op alerts (add stays silent)");
    }

    // drops fire on the DELTA, not the sticky cumulative
    {
        MetricsSnapshot s; s.unit = "ns";
        last_drops = 0;
        s.drops = 5;                         // 5 new since 0
        evaluateAlerts(s, cfg, last_drops);
        REQUIRE(find(s, "drops", "warn") != nullptr, "new drops raise a 'drops' warn");
        REQUIRE(last_drops == 5, "last_drops advances to the current count");

        s.drops = 5;                         // unchanged -> no new drops
        evaluateAlerts(s, cfg, last_drops);
        REQUIRE(find(s, "drops") == nullptr, "no NEW drops -> no alert (delta, not sticky)");
    }

    // p99 rule is skipped when latencies are uncalibrated (unit != ns)
    {
        MetricsSnapshot s;
        s.unit = "tick";
        s.num_ops = 1; s.ops[0] = { "add", 100, 0, 20, 99999, 99999, 99999 };
        uint64_t ld = 0;
        evaluateAlerts(s, cfg, ld);
        REQUIRE(find(s, "p99") == nullptr, "p99 rule is off when latencies are raw ticks");
    }

    printf(failures ? "\n%d FAILURES\n" : "\nALERTS OK (crossed/spread/p99/drops fire and clear as specified)\n",
           failures);
    return failures ? 1 : 0;
}

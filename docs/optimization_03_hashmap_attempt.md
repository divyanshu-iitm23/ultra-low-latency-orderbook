# Attempt 3: Replace std::unordered_map (orders_) — REVERTED

Hypothesis: a flat open-addressing table would remove per-insert malloc,
improve cache locality, and shrink the ~24us tail spike.

## Result (RDTSC, pinned core, median-of-7) — WORSE on every metric
| Metric | std::unordered_map | OpenHashMap | outcome   |
|--------|-------------------:|------------:|-----------|
| mean   |  26.6 ns           |  32.5 ns    | 22% worse |
| p50    |  25.1 ns           |  97.7 ns    | 3.9x worse|
| p99    |  37.3 ns           | 265.2 ns    | 7.1x worse|
| p99.9  |  98.7 ns           | 561.3 ns    | 5.7x worse|
| max    | 23964 ns           | 26744 ns    | unchanged |

## Why it failed
- max was UNCHANGED -> orders_ was never the tail source. The ~24us spike is
  elsewhere (likely first-touch page faults on pool memory / scheduling), not
  the hash map. The premise was wrong.
- A naive open-addressing table (expensive splitmix64 hash, lengthening linear
  probes, rehash-on-growth) is slower than a tuned std::unordered_map.

# Profiling (perf)

## Method
- profile_driver: bounded steady-state (~100k live orders), 200M ops, pinned to core 3
- built -O3 -g -fno-omit-frame-pointer; perf stat (sw + hw events) + task-clock flamegraph

## Findings (200M ops, 9.5s)
- IPC ~1.78 (75.0B instr / 42.2B cycles) — healthy, not memory-bound
- cache-misses ~18.3M => ~0.09 per op — good locality (pool + array ladder)
- context-switches: 67 ; cpu-migrations: 0 — pinned thread barely touched by OS
- page-faults: 37,353, all minor, ~153MB => one-time first-touch mapping, not recurring
- major-faults: 0

## Conclusion
- The ~24us worst case is NOT algorithmic. No recurring software source:
  faults are one-time first-touch; switches/migrations ~0 on the pinned core.
- Residual tail = OS/hardware jitter (interrupt/SMI/first-touch during warmup).
  Removing it requires core isolation (isolcpus, nohz_full, IRQ affinity) —
  a deployment concern, not a data-structure change.
- No mean-latency win identified that the data justifies; structure near its floor.

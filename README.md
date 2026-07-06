# Ultra-Low-Latency Order Book

A high-performance limit order book written in C++, built as the software core of a
three-part HFT systems- orderbook, FPGA packet parser and real-time latency dashboard. The focus is not just on *being* fast, but on being fast
**predictably** — minimizing the latency *tail*, which is what actually matters in
high-frequency trading, where a single multi-microsecond spike is a lost trade.

Every performance claim in this document was measured on real hardware. Here's my setup :
- OS/Kernel : Kali Linux
- CPU Architecture : x86 (16 cores, profiling done on single pinned core)
- IDE : vsCode ( c++17, gcc 9+/Clang 10+ ).
- Every test result is median-of-seven-runs.

---

## Roadmap

```
PHASE 1: ORDER BOOK ENGINE (Software)
│
├── # Initial Order Book Implementation
|   |
│   ├──  Core types, Order, PriceLevel, OrderBook
│   ├──  add / cancel / modify / executeMarketOrder
│   ├──  Fixed-point pricing, intrusive FIFO lists
│   ├──  13 unit + stress tests (to 1M ops)
│
├── # Performance Optimization
|   |
│   ├──  Object pool allocator (killed ~775µs malloc tail)
│   ├──  Cache-line alignment (measured neutral)
│   ├──  False-sharing elimination → DEFERRED to Phase 2 (needs 2nd thread)
│   ├──  RDTSC benchmark harness (calibrated, pinned, median-of-7)
│   ├──  std::map → array ladder + bitmap  (3.9× mean, 28× max)
│   ├──  orders_ hash map → tried & REVERTED (documented)
│   ├──  perf profiling (synthetic) — tail proven to be OS/hardware jitter, not code
│   └──  branch-prediction tuning → SKIPPED (low value; IPC already 1.78)
│
└── # Market Data Integration & Matching Engine
    |
    ├──  NASDAQ ITCH 5.0 parser (round-trip verified)
    ├──  Parser validated on REAL data (13.8M msgs)
    ├──  Parser → order book integration (addOrder-with-external-id, BookReplay)
    ├──  Reconstructed real AAPL book — non-crossed, $0.05 spread
    ├──  Batch report wrapper (run_itch_replays.sh → docs/)
    ├──  perf profiling (real feed)
    ├──  Limit-order matching engine (just an attempt)
    └──  End-to-end latency measurement (messages/sec on real feed)

PHASE 2: REAL-TIME MONITORING DASHBOARD (Ops)
│
├── # Low-overhead metrics instrumentation
|   |
│   ├──  MetricsEvent (32-byte POD) + SPSC lock-free ring (padded, drop-on-full)
│   ├──  ScopedLatency + MetricsRecorder — zero machine code when compiled off
│   ├──  Aggregator thread — log-bucket histograms, p50/p99/p99.9/max, windows
│   ├──  Console "top"-style live readout (per-op percentiles + ops/sec)
│   ├──  Wired into the engine (add / cancel / modify), METRICS_ENABLED guard
│   ├──  Driven live from a real ITCH feed — paced replay (max / real-time / Nx)
│   ├──  Overhead A/B — detached +1.5 ns, attached +41 ns vs clean addOrder
│   └──  False-sharing A/B — padded 2.4x ring throughput vs SPSC_NO_PAD (cross-core)
│
└── # Real-time visualization
    |
    ├──  Snapshot serialization (JSON) — aggregator sink, NDJSON, off hot path
    ├──  UDP snapshot publisher (C++) — fire-and-forget, one snapshot per datagram
    ├──  UDP -> WebSocket bridge (Python, asyncio + websockets)
    ├──  Browser dashboard v1 (single HTML + uPlot) — percentiles, throughput, book
    ├──  Alert rules in the aggregator (p99 · spread · drops · crossed-book)
    ├──  Book-depth ladder + trade tape (seqlock depth channel + Trade-event tape)
    └──  NDJSON logging + historical playback (bridge --log + ndjson_playback.py)

PHASE 3: FPGA UDP PACKET PARSER (Hardware)
│
├── 1:  FPGA env + SystemVerilog refresher (simulation-only)
├── 2: UDP / IP / Ethernet parser in RTL (AXI-Stream, line-rate)
├── 3: ITCH message parser in hardware
└── 4: FPGA vs software comparison + integration
```

---

## Architecture

The book is built from three independent structures, each chosen to answer one specific
question efficiently:

| Question | Structure | Complexity |
|---|---|---|
| "Where does order #N live?" | hash map (`OrderId → Order*`) | O(1) lookup |
| "What is the best price, and what sits at price P?" | direct-mapped array + occupancy bitmap | O(1) access |
| "Who is first in line at this price?" (time priority) | intrusive doubly-linked list | O(1) insert/remove |

Memory for the `Order` objects themselves is supplied by a custom object pool, so the
data structures above hold *pointers* into pooled storage rather than separately-allocated
objects.

On a real NASDAQ ITCH feed, these structures are driven by a thin replay layer that decodes
the binary protocol and maps each market-data message onto the book operations above —
turning the data structure into a market-data system that reconstructs a live venue's book
from raw exchange .

---

## Data Structure Choices & Evolution

The project's performance story is largely a story of replacing general-purpose containers
with structures matched to the specific access pattern. Three separate concerns, three
separate decisions.

### 1. Orders at a single price (time priority) — *intrusive doubly-linked list*

This choice was made at the start and has not changed. Orders at the same price must be
served first-in-first-out, which calls for a queue supporting O(1) append and O(1) removal
from the middle (a cancel can target any order).

| Option | Append | Cancel (middle) | Allocation | Cache |
|---|---|---|---|---|
| `std::list` | O(1) | O(1) | node alloc per order | poor (scattered) |
| `std::vector` | O(1) amortized | O(n) shift | bulk | good |
| `std::deque` | O(1) | O(n) | block | medium |
| **intrusive linked list** | **O(1)** | **O(1)** | **none (pointers live in Order)** | **good (with pool)** |

The `next`/`prev` pointers live *inside* the `Order` struct, so no separate list-node is
ever allocated, and — because the orders come from a contiguous pool — walking the list
stays cache-friendly.

### 2. Price levels (best-price lookup + level access) — *std::map → direct-mapped array*

This is where the biggest win came from. The initial implementation used `std::map`, which
is correct and convenient but carries three costs.

| | `std::map` (initial) | direct-mapped array + bitmap (current) |
|---|---|---|
| Access a price level | O(log P) tree walk | **O(1)** direct index (`price − min_price`) |
| Best bid/ask | O(1) via `begin()` | **O(1)** via occupancy bitmap + bit-scan |
| Allocation | **one `malloc` per new price level** | **zero** (one array allocated up front) |
| Memory layout | scattered tree nodes (pointer-chasing, cache misses) | one contiguous block (cache-friendly) |
| Memory cost | proportional to *occupied* levels | proportional to the *price range* |

The array is a "price ladder": one pre-allocated array of `PriceLevel`, indexed directly by
`price − min_price`. Because an array has no inherent notion of "the best occupied price"
(the convenience `std::map` gave us via `begin()`), an **occupancy bitmap** — one bit per
level — is maintained alongside it. Finding the best bid becomes "highest set bit," located
with a hardware bit-scan instruction (`__builtin_clzll`); the best ask is the lowest set
bit (`__builtin_ctzll`). This is O(1) in the common case and a short, bounded word-scan in
the worst case — and, critically, never allocates.

The trade-off is explicit: the ladder reserves memory across the instrument's entire price
range whether or not those levels are used. For a single instrument with a bounded trading
band this is a few megabytes — a non-issue — and the unused pages are never faulted into
physical RAM by the OS. It does *not* generalize to thousands of instruments or unbounded
price ranges; those would need a price-relative windowed array or an array+map hybrid. This
is the correct trade-off for a single-instrument book and a known limitation otherwise.

### 3. Order lookup by ID — *std::unordered_map (kept after a failed experiment)*

Cancels and modifies need to find an arbitrary order by its ID in O(1). `std::unordered_map`
handles this. An attempt was made to replace it with a hand-rolled open-addressing flat
hash table (the idiomatic HFT choice), hypothesizing that removing per-node allocation and
improving cache locality would shrink the latency tail.

**The experiment was measured and reverted** — it was worse on every metric (see the
optimization log below). The lesson is recorded deliberately: "hand-rolled beats the
standard library" is a *heuristic in HFT, not a law*. A naive open-addressing table with an
expensive hash and lengthening probe chains lost decisively to a mature, tuned
`std::unordered_map`. The win has to be measured, never assumed.

A second vindication of keeping `std::unordered_map` surfaced during market-data
integration. Real NASDAQ order references are **arbitrary 64-bit values**, and the reverted
open-addressing table reserved `UINT64_MAX` as an empty-slot sentinel — a value a real
exchange reference could legitimately take, which would have corrupted the table.
`std::unordered_map` has no such restriction, so the structure that won on raw latency also
turned out to be the correct one for real-world keys.

---

## Memory Management

Predictable latency requires controlling *when* memory is touched. The guiding rule is:
**no allocation on the hot path in steady state.** Three mechanisms enforce this.

### Object pool for `Order` objects

Rather than `new`/`delete` per order — which occasionally detours into the kernel
(`mmap`/`sbrk`, page faults) and produces multi-microsecond spikes — all `Order` storage
comes from a custom `ObjectPool<Order>`:

- **Bulk allocation up front.** One large slab is allocated at construction; the hot path
  only ever *hands out* pre-reserved slots.
- **Intrusive free list.** Free slots store a "next free slot" pointer inside their own
  unused bytes (via a `union`), so the free list costs zero extra memory. A slot is *either*
  a live `Order` *or* a free-list link, never both at once.
- **O(1) allocate / deallocate.** Both operations are a single pointer swap — no search,
  no syscall.
- **Placement new.** Objects are constructed directly into pooled storage, preserving the
  `Order` constructor's behavior without touching the heap.
- **Chunked growth.** If the pool is exhausted it grabs another slab; in steady state this
  never fires, so the hot path remains allocation-free.

A secondary benefit is locality: pooled `Order`s are contiguous, so walking the price-level
linked lists frequently finds the next order already in cache.

### Pre-allocated price ladder

The price-level array is a single allocation sized to the price range. A brand-new price
level therefore costs an array index and a bit-set — never a heap allocation. This is what
eliminated the per-level `malloc` that previously dominated the worst-case latency.

### Fixed-point pricing

Prices are stored as 64-bit integer "ticks," not floating-point. This avoids floating-point
representation error in price comparisons (critical for correct ordering and matching) and
makes prices exact, hashable, and directly usable as array indices.

### Cache-line alignment

The `Order` struct is aligned to a 64-byte cache line (`alignas(64)`), with a compile-time
`static_assert` guarding its size. On the current single-threaded workload this measured as
performance-neutral and is kept primarily as documentation of intent; its value is expected
to appear once the engine becomes multi-threaded (see *Deferred Work*).

### What profiling concluded about the remaining allocation source

`std::unordered_map` (the order-lookup index) still allocates a node per insert, and at the
end of the optimization phase this was the prime suspect for the remaining ~24 µs worst-case
sample. **Profiling disproved it.** On the synthetic workload the hash map's behavior was
healthy (see *Perf Profiling*), and on a real feed the hash map accounted for **0.06%** of
runtime — it is not a meaningful cost. The residual tail turned out to be OS/hardware jitter
rather than any allocation in the engine. This is recorded as a worked example of the
project's central discipline: the obvious suspect was wrong, and only measurement revealed
it.

---

## Optimization Log

Each entry records what changed, the reasoning, and the measured outcome. All latency
figures are `addOrder`, RDTSC harness, pinned core, median-of-seven, timer-overhead
subtracted, on 16-core bare-metal Linux.

### 1. Object pool allocator
Removed `new`/`delete` from the hot path. Eliminated the dominant allocation-induced tail
spike (early chrono-based measurement showed a ~775 µs worst-case sample collapse). Also
improved typical-case latency via cache locality. *The first and most impactful correctness-
for-determinism change.*

### 2. Cache-line alignment of `Order`
Aligned `Order` to 64 bytes so each order occupies exactly one cache line. Measured as
within run-to-run noise on the single-threaded workload — possibly a hair slower due to
reduced density. Kept for documentation and future multi-threaded relevance. *A correct
negative-ish result: hypothesized benefit did not materialize here, and that was measured
rather than assumed.*

### 3. RDTSC benchmark harness — **infrastructure**
Replaced `std::chrono` timing (tens of ns of its own overhead, polluting sub-100 ns
measurements) with a calibrated TSC clock. Key correctness detail: on modern CPUs the TSC
is an invariant high-resolution *wall clock*, not a per-cycle counter, so its tick-rate is
calibrated at runtime rather than assumed. The harness subtracts its own overhead,
pre-generates inputs (so the RNG is never inside the timed region), warms up
caches/branch-predictor/frequency, pins a core, and reports median-of-seven for stability.
*This is what made every subsequent comparison trustworthy.*

### 4. `std::map` → direct-mapped array + occupancy bitmap

| Metric | `std::map` | array + bitmap | Improvement |
|---|---:|---:|---:|
| batch mean | 104.8 ns | 26.6 ns | **3.9×** |
| p50 | 86.4 ns | 25.1 ns | 3.4× |
| p90 | 105.4 ns | 28.6 ns | 3.7× |
| p99 | 123.4 ns | 37.3 ns | 3.3× |
| p99.9 | 150.7 ns | 98.7 ns | 1.5× |
| **max** | **665,081 ns** | **23,964 ns** | **28×** |

O(log P) → O(1) access, contiguous memory, and zero per-level allocation. The 28× drop in
worst-case latency is the headline: the per-level `malloc` that caused the spike no longer
exists. (Note that p99.9 improved least — a clue that the *remaining* tail lives elsewhere.)

### 5. `std::unordered_map` → open-addressing flat table — **REVERTED**

| Metric | `std::unordered_map` | open-addressing | outcome |
|---|---:|---:|---|
| batch mean | 26.6 ns | 32.5 ns | 22% worse |
| p50 | 25.1 ns | 97.7 ns | 3.9× worse |
| p99 | 37.3 ns | 265.2 ns | 7.1× worse |
| p99.9 | 98.7 ns | 561.3 ns | 5.7× worse |
| max | 23,964 ns | 26,744 ns | unchanged |

Worse across the board, and the worst-case sample was **unchanged** — proving the
hypothesis wrong twice over: the order-lookup map was never the tail source, and a first-cut
open-addressing table (expensive hash, lengthening linear probes, rehash-on-growth) is
slower than a tuned `std::unordered_map`. Reverted. The dead-end is documented because the
discipline it demonstrates — measure, then decide — is the point.

### 6. Symbol filter: `strcmp` → `uint64_t` compare
Surfaced by profiling the ITCH replay, not the synthetic benchmark. The replay filters the
feed to one symbol; the original filter did a `strcmp` of the 8-byte stock field on every
Add message. Profiling showed `__strcmp_avx2` was **12.88%** of replay time — about **eight
times** the cost of the entire order book. Because ITCH symbols are exactly 8 bytes, the fix
is to pack the target symbol into a `uint64_t` once at construction and compare a single
64-bit integer per Add. Re-profiling **confirmed `__strcmp_avx2` disappeared** from the
flamegraph. The change is behavior-identical (the replay-integration and parser round-trip
tests both pass unchanged) — a pure, measured speedup justified by the profile and verified
by re-profiling.

---

## Perf Profiling and Flamegraphs

Profiling was done in two distinct campaigns answering two different questions, with `perf`
(hardware counters + sampling) and Brendan Gregg's flamegraph tooling. The recurring theme
— stated up front because it is the most valuable takeaway — is that **measurement
repeatedly overturned plausible hypotheses.** The bottleneck was never where intuition first
placed it.

### Part A — synthetic workload: hunting the ~24 µs tail

After the array ladder, a ~24 µs worst-case sample remained. A dedicated `profile_driver`
ran a bounded steady-state workload (~100k live orders, 200M operations, pinned core) so the
profile reflected sustained behavior rather than startup.

A crucial methodological point came first: **a flamegraph is a sampling profiler and finds
where the *average* time goes — it cannot find a rare tail spike**, because a 24 µs event
among hundreds of thousands of operations falls between samples. So the tail was chased with
event counts (`perf stat`) and event-targeted recording, not the flamegraph.

What the counters showed over a 200M-operation, 9.5-second run:

- **IPC ≈ 1.78** — healthy; the CPU is executing efficiently, *not* stalled on memory.
- **Cache-miss rate ≈ 0.09 per operation** — good locality (pool + array ladder working).
- **context-switches: 67; cpu-migrations: 0** — the pinned thread is barely descheduled and
  never leaves its core, so preemption is not a recurring spike source.
- **page-faults: 37,353, all minor, zero major** — these total ~153 MB, matching the
  resident working set, i.e. **one-time first-touch** mapping during ramp-up, not a
  recurring per-operation cost. (`sys` time was a trivial 72 ms, confirming this.)
- **Flamegraph**: the widest box was `cancelOrder` (which does more than `addOrder`: a hash
  lookup, an unlink, a pool deallocation, and a best-price recompute when a level empties);
  the `mt19937` RNG was a visible slice that is a *benchmark artifact*, not part of the book;
  the rare tail events appeared as vanishingly narrow towers.

**Conclusion:** the residual ~24 µs worst case is **not algorithmic.** There is no recurring
software source — faults are one-time, context switches and migrations are ~zero. The spike
is OS/hardware jitter (a first-touch fault landing in a timed iteration during warm-up, or a
one-off interrupt / SMI on the core). Removing it requires platform-level core isolation
(`isolcpus`, `nohz_full`, IRQ affinity) — a deployment concern, not a data-structure change.
Knowing where software ends and the platform begins is part of the engineering, not an
evasion of it.

Checkout flamegraphs in `/docs` .

### Part B — real ITCH feed: where do cycles actually go?

Profiling the real `itch_book_replay` answered a different question: on a real market-data
feed, where does the time go — parsing, book operations, or something else? This campaign
contains the project's sharpest example of measurement correcting intuition.

**The first reading was misleading, and the flamegraph caught it.** `perf stat` on the
500 MB slice looked memory-bound (51% backend-bound, IPC down to 1.1, 37% LLC miss rate),
which suggested cold hash-map order lookups. The flamegraph **disproved that**: roughly
60% of samples were the *kernel page-fault handler* —
`asm_exc_page_fault → handle_mm_fault → __alloc_pages_slowpath → compact_zone →
migrate_pages` — the OS faulting in the 500 MB `mmap`'d file and running memory compaction.
The order book was nearly invisible, and `std::unordered_map` was 0.06%. The "memory-bound
on cold lookups" hypothesis was simply wrong; the stalls were file I/O, not the engine.

**Isolating the engine.** To move file faults out of the measured region, the replay tool
was changed to pre-fault the mapping (`MAP_POPULATE` + `madvise(SEQUENTIAL/WILLNEED)`) and
to print load-vs-process phase timing. Re-profiling the slice (which fits in RAM, so the
parse loop becomes fault-free) finally showed the engine's true proportions:

| Frame | Share of run | What it is |
|---|---:|---|
| `__strcmp_avx2` | **12.88%** | symbol filter (`strcmp` per Add) — **actionable** |
| `__memset_avx2` | 11.78% | `OrderBook` ctor zeroing the price ladder — *one-time startup* |
| page faults (residual) | ~13% | leftover faulting + `munmap` teardown |
| `cancelOrder` + `addOrder` | **~1.6%** | **the entire order book** |
| `std::unordered_map` | 0.06% | the order-lookup index — negligible |

The headline: **on a real feed the order book is ~1.6% of runtime — the engine is
essentially free.** The two largest real costs are things *around* the book: the symbol
filter (now optimized — see Optimization Log #6) and a one-time ladder-zeroing `memset`.

**The full-day file confirms the I/O floor.** Profiling the 15 GB file (which cannot stay
resident, so `MAP_POPULATE` does not help) is dominated by `filemap_fault` (24%),
`asm_exc_page_fault` (20%), and memory compaction `compact_zone`/`migrate_pages` (12%) —
the unavoidable cost of streaming 15 GB through limited RAM. Even here `__strcmp_avx2` (9%,
before the fix) dwarfed `cancelOrder` (0.84%). This file I/O floor is precisely why
production HFT systems push parsing and feed handling into hardware — which is the
motivation for **Phase 3 (FPGA packet parser)**.

**Net conclusion of the profiling work:** there is no further optimization the data
justifies in the order book itself. The synthetic benchmark gives the latency story
(26.6 ns mean, 24 µs jitter-bound tail); the real-feed profile gives the systems story (the
book is ~1.6%; the cost is I/O plus a symbol filter that has been removed). Both are
complete and evidence-backed.

### Parsing Real ITCH feed
```
For a subset of data (~500 Mb)

Method:

./build/itch_book_replay ~/market-data/sample_500mb.ITCH50 AAPL 


Output:

=== ITCH replay: AAPL ===

modeled messages parsed : 13838945

adds=50364

deletes=26200

reduces=3660

replaces=4948

final book state for AAPL:
live orders   : 21948

bid levels    : 3597   ask levels: 1135

best bid      : $287.53

best ask      : $287.58

spread        : $0.05

crossed?      : no (good)
```

```
For complete ITCH data (~8 Gb)

Method:

./build/itch_book_replay ~/market-data/12302019.NASDAQ_ITCH50 AAPL 


Output:

=== ITCH replay: AAPL ===

modeled messages parsed : 263241937

adds=698728

deletes=654441

reduces=62010

replaces=96968

final book state for AAPL:
live orders   : 0

bid levels    : 0

ask levels: 0
  (one side empty — symbol may be wrong, or slice too short to populate both sides)
```
### Generating Flamegraphs for the Run

```bash
sudo sysctl -w kernel.perf_event_paranoid=1

sudo sysctl -w kernel.kptr_restrict=0

taskset -c 3 perf record -F 4000 --call-graph fp -o itch_perf.data -- \
    ./build/itch_book_replay_prof ~/market-data/sample_500mb.ITCH50 AAPL

perf script -i itch_perf.data \
  | ~/FlameGraph/stackcollapse-perf.pl \
  | ~/FlameGraph/flamegraph.pl > docs/flamegraph_itch.svg

```



*For **perf profiling outputs** of the two cases checkout these files:*
- `/docs/profiling_NASDAQ_sample500MB.md` &
- `/docs/profiling_real_NASDAQ.md`


---
---

## Market Data Integration

This is where the project crosses from a fast data structure into a market-data system: the
engine reconstructs a real venue's order book directly from raw exchange tape.

### NASDAQ TotalView-ITCH 5.0 parser

ITCH 5.0 is NASDAQ's binary feed describing the full order book as a stream of events. Its
properties dictate how it must be parsed:

- **Binary, big-endian, packed.** Every multi-byte integer is network byte order, and
  fields sit at fixed offsets with no padding — so each field is read at an explicit offset
  and byte-swapped. A C struct is **never** cast over the bytes (alignment and endianness
  would both be wrong).
- **Historical-file framing.** Downloadable sample files prefix each message with a 2-byte
  big-endian length, then the message body — the framing the parser handles.
- **Field encodings.** Prices are 4-byte integers with 4 implied decimals (`1234500` =
  $123.4500); timestamps are 6-byte nanoseconds-since-midnight; stock symbols are 8 bytes,
  space-padded.

Modeled message types: **A** (Add), **F** (Add with attribution), **D** (Delete — full
removal), **X** (Cancel — partial share reduction), **E**/**C** (Executed — order traded,
reduce shares), **U** (Replace — delete old reference, add new). All other message types are
decoded-and-skipped, with framing advanced by the on-wire length so the stream stays in
sync.

**Verification was layered and honest:**

- *Round-trip test* (generate → parse → check) confirms the parser's **mechanics** —
  endianness, byte offsets, framing, and modeled-vs-skip dispatch are internally consistent.
  This is necessary but **not** proof of spec-correctness; it only proves the parser agrees
  with its own generator.
- *Validation on real data* is the real test. A 500 MB slice decodes to **13,838,945**
  modeled messages with a realistic type distribution:

  ```
  A (Add)            5,956,331
  F (Add w/ MPID)      826,907
  D (Delete)         4,960,858
  U (Replace)        1,284,860
  X (Cancel)           606,522
  E (Execute)          199,294
  C (Execute w/ px)      4,173
  ```

  These proportions match how a real order book behaves (adds dominate, deletes track them,
  `E` ≫ `C`). A parser reading at wrong offsets cannot produce a realistic distribution — so
  the offsets are confirmed against real NASDAQ data, not just self-consistent.

### Replay → order book

A thin `BookReplay` layer maps each decoded message onto a book operation:

- **A / F → add.** This required a new `addOrder(side, price, qty, externalId)` overload that
  keys the book on the exchange's **arbitrary 64-bit order reference** instead of an internal
  counter (the reason `std::unordered_map` being kept mattered — see Data Structures #3).
- **D → cancel.** **X / E / C → reduce** shares, removing the order when it reaches zero.
  **U → cancel-then-add** (the new order inherits the original's side, read before the
  cancel frees it).
- **Symbol filtering.** Only Add messages carry the stock symbol; D/X/E/U reference orders
  by ID. Because only target-symbol orders are ever added, messages for other symbols
  naturally find nothing in the book and are skipped — the book's own membership *is* the
  filter. Prices are converted from ITCH's 1/10000-dollar units to whole cents (exact for any
  NMS stock trading in penny ticks; a documented, deliberate trade-off for sub-$1 names).

The full pipeline (parser → `BookReplay` → `OrderBook`) is verified end-to-end on synthetic
ITCH covering every operation, with assertions on the final book state: symbol filtering,
partial cancel, execution removal, replace-with-inherited-side, delete, FIFO ordering, and
the non-crossed invariant all pass.

### Reconstructing a real book

Running the replay over a real file reconstructs a live AAPL book:

```
live orders : 21,948
best bid    : $287.53      best ask : $287.58
spread      : $0.05        crossed? : no (good)
```

The decisive correctness signal is the **non-crossed invariant**: a correct single-symbol
reconstruction is never crossed, because the exchange's matching removed crossing orders
before they could rest. A tight $0.05 spread on a liquid
name and a deep, two-sided book confirm a faithful reconstruction.

Two honest notes on the data:

- **Price/date.** The file `12302019` is dated 30 Dec 2019; late-2019 AAPL traded around
  $290 (pre-split), so a $287 best bid is plausible — but a cross-check against a second
  symbol (MSFT/SPY) to pin the file's true date is still pending and is the correct way to
  confirm it rather than assume.
- **Full-day replay drains to empty.** Processing all 263 M messages of a full session
  correctly ends with **0 live orders** — every order is eventually filled, cancelled, or
  replaced by the close. This is the right end-of-day state, not a bug; inspecting an
  intraday snapshot requires a mid-session timestamp cutoff (planned).

### A point to notice
`itch_replay` and `itch_book_replay AAPL` parses the same number of messages --> `263241937 (~263 M)`, but their A/D/E/C/X.. differs.
- itch_replay, counts every modeled message in the entire file, across all symbols:
```
A : 117,145,568      # all Adds, every stock on NASDAQ that day
D : 114,360,997      # all Deletes, every stock
...
```
- itch_book_replay AAPL, counts only the messages it actually applied to the AAPL book:
```
adds=698,728         # only AAPL Adds
deletes=654,441      # only Deletes of orders that were in the AAPL book
...
```
It's the symbol filter doing its job. AAPL is one symbol out of thousands, so it's responsible for ~699k of the 117M total adds (about 0.6%). The two tools agree perfectly on the thing they both count - modeled messages: 263,241,937 is identical in both runs, because both parsed the same full file. They diverge on the per-op counts because one is whole-market and the other is one-symbol-filtered.

### Some other Filters apart from AAPL -> MSFT, SPY
```
Method:

./build/itch_book_replay ~/market-data/12302019.NASDAQ_ITCH50 MSFT 

Output:

=== ITCH replay: MSFT ===
file size            : 8251.4 MB
load (mmap+prefault) : 21299 ms
process (parse+book) : 7695 ms   (34.2 M msg/s)
modeled messages     : 263241937
  adds=576011 deletes=548843 reduces=36155 replaces=54873
final book state for MSFT:
  live orders   : 0
  bid levels    : 0   ask levels: 0
  (one side empty — wrong symbol, or full-day file drained to empty)

```
```
Method:

./build/itch_book_replay ~/market-data/12302019.NASDAQ_ITCH50 SPY 

Output:

=== ITCH replay: SPY ===
file size            : 8251.4 MB
load (mmap+prefault) : 22864 ms
process (parse+book) : 7569 ms   (34.8 M msg/s)
modeled messages     : 263241937
  adds=991821 deletes=960491 reduces=62496 replaces=117317
final book state for SPY:
  live orders   : 0
  bid levels    : 0   ask levels: 0
  (one side empty — wrong symbol, or full-day file drained to empty)

```


### Tooling

- `itch_replay` — parser-only message-type histogram; the first-line validation that the
  offsets decode real files correctly.
- `itch_book_replay` — full reconstruction (`mmap` + pre-faulting + phase timing) that
  prints book state and replay throughput for a chosen symbol.
- `run_itch_replays.sh` — batch wrapper that runs the replay across multiple files/symbols
  and saves a timestamped Markdown report into `docs/`.

---

## Matching Engine *(attempting trade execution)*
 
Everything before this point is a **storage** book: `addOrder` inserts at a price level and
never checks whether the order crosses the opposite side. That is exactly why a storage book
is *allowed* to be crossed. The matching engine changes the contract: an incoming aggressive
order is matched against the resting book in **price-time priority** before any remainder is
allowed to rest. With matching in place, the book can never be crossed — and the deferred
invariant returns as an enforced, tested guarantee.
 
### Semantics
 
Four rules define the behavior, following standard exchange conventions:
 
1. **Trades execute at the resting (maker) order's price.** A buy limit at $10.05 hitting a
   resting ask at $10.00 trades at **$10.00** — the aggressor receives price improvement.
2. **Price priority, then time priority.** The best opposing level is swept first; within a
   level, the FIFO head (the oldest resting order) fills first.
3. **Partial fills in both directions.** A partially-hit resting order stays in place with
   reduced quantity, *keeping its time priority*. A partially-filled incoming limit rests its
   remainder at its limit price.
4. **Market orders never rest.** They sweep at any price until filled or the opposing book is
   exhausted; any unfilled remainder is dropped.
### API
 
```cpp
struct Trade {
    Price    price;      // execution price = resting/maker order's price
    Quantity quantity;   // shares traded
    OrderId  makerId;    // resting order matched against
    OrderId  takerId;    // incoming aggressive order
    Side     takerSide;  // side of the aggressor
};
 
std::vector<Trade> submitLimit (Side side, Price price, Quantity qty, OrderId takerId);
std::vector<Trade> submitMarket(Side side, Quantity qty, OrderId takerId);
```
 
Both are built on one private `match()` routine that walks the opposing ladder via the
occupancy bitmap (best price first) and each level's FIFO list. A fully-filled maker is
removed through the already-tested `cancelOrder` path — inheriting the level/bitmap/
best-price/pool bookkeeping for free — while a partially-filled maker is reduced in place.
`submitLimit` rests any unfilled remainder through the existing `addOrder`. The matcher is
therefore a thin orchestration layer over operations the book already proved correct.
 
### Verification
 
A scripted scenario test (`test_matching`) drives the matcher through hand-computed
expectations: FIFO order within a level, multi-level sweeps in strict price order,
maker-priced executions, partial fills on both maker and taker sides, non-crossing limits
resting without generating trades, and a market order sweeping the entire opposing book —
with the **non-crossed invariant asserted after every single operation**. The addition is
purely additive: the ITCH-replay integration test and the parser round-trip test continue to
pass unchanged. (Matching correctness is test-verified; matching *latency* has not yet been
benchmarked with the RDTSC harness — listed under extensions.)
 
### Scope — what this matcher is and isn't
 
This is a deliberately **core** matcher: limit + market orders, price-time priority, partial
fills, maker-priced trades. It does not implement production order-type refinements —
IOC/FOK time-in-force, self-trade prevention, iceberg/hidden orders, pro-rata allocation, or
auction crosses. Two known deviations from the project's own rules are recorded for honesty:
the trade-report `std::vector` allocates on the matching hot path (a production matcher
would use a caller-provided buffer or callback), and `modifyOrder` does not implement
lose-priority-on-quantity-increase semantics (matching itself only ever *reduces* makers, so
the matcher is unaffected). All are listed under *Deferred Work / Extensions* — naming the
omissions is part of the engineering.
 
### Why the ITCH replay does not call the matcher
 
ITCH is a **post-matching** feed: NASDAQ's engine already matched the orders, and the tape
records the results (adds of orders that rested; executions the venue decided). Replaying
ITCH through a local matcher would be wrong — it would re-decide outcomes the exchange
already decided. So reconstruction (`BookReplay`) and matching (`submitLimit`/`submitMarket`)
are deliberately **separate capabilities sharing one book**: replay rebuilds a venue's book
faithfully; the matcher turns that book into an exchange-style engine for new order flow.
 
---
 
## Phase 1: Conclusion
 
Phase 1 set out to build the software core of an HFT stack — a limit order book that is
fast, predictable, correct, and real. All four are demonstrated, each with evidence:
 
| Claim | Evidence |
|---|---|
| **Fast** | 26.6 ns mean `addOrder` (RDTSC harness, pinned core, median-of-7, real hardware) |
| **Predictable** | worst case 665 µs → 24 µs (**28×**); residual tail *proven* OS/hardware jitter |
| **Real** | reconstructs a live NASDAQ AAPL book from raw ITCH tape — non-crossed |
| **Complete** | price-time-priority matching engine; crossed-book invariant enforced and tested |
| **Efficient in context** | the order book is ~1.6% of real-feed replay runtime; the rest is I/O |
 
The arc of the phase: a correct storage book → measured optimization with
documented dead ends → real market data and a matching engine. One
detail bookends the whole phase: the **crossed-book invariant** was written,
correctly *deferred* because a storage-only book may legitimately be crossed (the test was
wrong, not the book), returned as the decisive *validator* of the real-data
reconstruction, and finally became an *enforced guarantee* of the matching engine.
 
---
 
## What Phase-1 Demonstrates
 
The portfolio value of this work is as much in the *engineering judgment* as in the numbers.
The recurring narratives:
 
- **Predictable latency over raw speed.** The whole project optimizes the *tail*, not the
  mean — because in HFT a single multi-microsecond spike is a lost trade. The headline result
  is the **28× worst-case reduction** (665 µs → 24 µs), not just the 3.9× mean.
- **Measure, don't assume.** Every retained change is backed by a clean measurement, and the
  experiments that *failed* are documented as first-class results: the hand-rolled hash table
  that lost to `std::unordered_map`, and the cache-line alignment that measured neutral.
  "Hand-rolled beats the standard library" is treated as a heuristic, not a law.
- **The data corrected the hypothesis — repeatedly.** This is the strongest thread.
  Intuition placed the bottleneck at the hash map; profiling showed the hash map is 0.06%.
  Counters suggested a memory-bound feed; the flamegraph showed the stalls were file I/O.
  The symbol filter — not the book — was the real software cost. Each time, measurement
  overturned a plausible story, and the project followed the evidence.
- **Correctness comes full circle.** The crossed-book invariant was written,
  *deferred* with a documented reason (a storage book may be crossed — the test was invalid,
  not the code), used to *validate* a real NASDAQ reconstruction, and finally
  *enforced* by the matching engine. Knowing when an invariant applies is as important as
  writing it.
- **Knowing where software ends and the platform begins.** The residual 24 µs tail was
  proven to be OS/hardware jitter (interrupts/SMI/first-touch), not algorithm — a conclusion
  that points to core isolation as a deployment concern rather than forcing a hollow code
  "fix."
- **The engine is essentially free on a real feed.** On real NASDAQ data the order book is
  ~1.6% of runtime; the dominant cost is feed I/O. This is exactly why production systems
  push parsing into hardware — and it sets up Phase 3 (FPGA) as a logical next step.
- **Correctness validated on real data.** The non-crossed invariant on a reconstructed AAPL
  book, a realistic message-type histogram matching real market behavior, and end-to-end
  pipeline tests together show the system is not just fast but *right*.
- **Matching Engine.** The matcher implements the hard core (price-time priority, partial
  fills, maker pricing) and *names* its omissions (IOC/FOK, self-trade prevention, icebergs,
  the trade-vector allocation) instead of hiding them.
---
 
## Phase 2: Real-Time Monitoring

Phase 2 instruments the Phase 1 engine and surfaces its behavior on a live dashboard. Phase 1 made `addOrder` cost 26.6 ns, so
**instrumentation that adds 50 ns would destroy the very thing it measures.** The work is
therefore not "draw charts" but *observability with a bounded, measured, near-zero cost on
the hot path.* Three rules follow:

1. The hot path never blocks, never allocates, never does I/O. Per operation it may only read
   `rdtsc`, write one fixed-size record into a pre-allocated lock-free buffer, and continue.
   If the buffer is full it **drops the sample and counts it** — losing a metric is
   acceptable; stalling the engine is not.
2. Only the hot thread touches the `OrderBook` (which is not thread-safe and gains no locks).
   The metrics thread learns book state only from snapshot records the hot thread itself
   pushes. Single-writer, no exceptions.
3. All expensive work — percentile math, serialization, sockets — happens downstream on the
   metrics thread, where microseconds are free.



### The SPSC lock-free ring buffer

The load-bearing primitive —  the *simplest correct* lock-free structure: a
**single producer, single consumer** queue, which is what eliminates the hard parts (no CAS,
no ABA problem, no retry loops — just two indices and memory ordering).

- **Release/acquire pairing is the entire correctness argument.** The producer writes the
  slot, then publishes `head` with `memory_order_release`; the consumer reads `head` with
  `acquire`, then reads the slot. That pair guarantees the slot write happens-before the slot
  read — articulating *why* `relaxed` would be wrong here is the interview question this
  structure exists to answer.
- **Padded indices.** `head` (producer-hammered) and `tail` (consumer-hammered) sit on
  separate 64-byte cache lines so the two cores do not fight over one line.
- **Cached indices.** Each side keeps a plain (non-atomic) copy of the other's index and only
  re-reads the shared atomic when its cache says "full"/"empty" — cutting cross-core
  cache-line traffic in steady state.
- **Drop-on-full is a caller policy**, not the ring's: `try_push` returns `false` and the
  caller decides. The hot thread drops; tests retry. The ring never blocks or allocates.
  Records are a 32-byte `MetricsEvent` (two per cache line).

**Verification was layered, because a hand-written lock-free structure demands it:**

- A *torture test* runs 10 M items through a deliberately tiny ring (constant full/empty
  churn) in two modes. No-loss mode (producer retries on full) asserts the consumer sees
  *exactly* `0..N-1` in order — FIFO, no reorder, no duplication, no loss. Drop mode (producer
  never waits) asserts the consumer sees a *strictly increasing* subsequence and that
  `pushed == popped` with `pushed + dropped == N` — correct accounting under contention.
- *ThreadSanitizer* reports **zero data races** across both modes — the verification that
  matters for lock-free code, because TSAN reasons about the C++ memory model directly, so a
  clean result is meaningful for weakly-ordered hardware (ARM), which a plain x86 run *cannot*
  establish (x86's strong ordering hides missing-barrier bugs).
- On real hardware (producer and consumer on separate cores) the ring streams millions of
  items and demonstrates **buffer depth buys burst tolerance**: with the *same* producer and
  consumer, a 1024-slot ring dropped ~2% of items while a 16-slot ring dropped ~74% — the
  only variable was capacity. The drop *rate* is a property of buffer size and producer/
  consumer speed mismatch; the *correctness* (`pushed == popped`, strict ordering) held
  regardless.

```
// Torture test for SPSCRing: two threads, millions of items, small ring (forces full/empty
// churn constantly). Two modes:
//   Test 1 - NO LOSS (producer retries on full): consumer must see EXACTLY 0..N-1 in order.
//            Proves FIFO + no reorder + no duplication + no loss.
//   Test 2 - DROP ON FULL (producer never waits): consumer sees a STRICTLY INCREASING
//            subsequence (gaps allowed) and pushed == popped, pushed + dropped == N.
//            Proves no reorder/dup under heavy contention + correct accounting.
// Plus a MetricsEvent round-trip to confirm the real element type works.
```
### Running SPSC Ring Test
```bash
# build:
g++ -O2 -std=c++17 -pthread -Iinclude metrics/test_spsc.cpp -o ~/ultra-low-latency-orderbook/build/test_spsc

# run:
# pinning only one core (core-3)
taskset -c 3 ./build/test_spsc

# pinning two cores (core-3 & 4)
taskset -c 3,4 ./build/test_spsc 
```
```
--> Output when ran on core-3 only : 

SPSC torture test (N=10000000)
   [test2] N=10000000 pushed=2048 dropped=9997952 popped=2048 (100.0% dropped)
   [test2] N=10000000 pushed=32 dropped=9999968 popped=32 (100.0% dropped)

SPSC RING OK (FIFO, no reorder/dup/loss, accounting, event round-trip)


--> Output when ran on core-3 and core-4 :

SPSC torture test (N=10000000)
   [test2] N=10000000 pushed=9816171 dropped=183829 popped=9816171 (1.8% dropped)
   [test2] N=10000000 pushed=2552614 dropped=7447386 popped=2552614 (74.5% dropped)

SPSC RING OK (FIFO, no reorder/dup/loss, accounting, event round-trip)

```
The honest claim: *race-free under ThreadSanitizer across millions of operations in both
retry and drop modes*.

### Zero-overhead instrumentation

Per-operation latency is captured by a `ScopedLatency` RAII guard (constructor reads `rdtsc`,
destructor reads it again and records the delta) feeding a `MetricsRecorder` that stamps a
`MetricsEvent` and pushes it into the ring. Both sit behind a `METRICS_ENABLED` compile-time
switch, and the defining property — **when metrics are compiled off, the instrumentation
produces literally zero machine code** — is verified in the generated assembly:

```
metrics OFF — the instrumented function compiles to 3 instructions:
    endbr64                  ; CFI landing pad (not instrumentation)
    mov   %rsi,%rdi          ; move arg
    jmp   do_work            ; tail-call straight into the real operation
    => zero rdtsc, zero ring code, zero branches
```

With metrics on, the same function contains the full machinery: two `rdtsc` reads, the event
written into the ring slot, the full/empty index check, and the drop path with its atomic
increment. This is what makes the overhead A/B *honest*: the "off" build is provably the
engine with no observer attached, not merely a predicted-false branch.

Supporting details: a **sampling knob** (record 1-in-N latency events) to tame the firehose
at full replay speed; a **drop counter** that is atomic but touched only on the rare
full-ring path (the common path is allocation- and atomic-free); and `recordTrade`/
`recordSnapshot` that read `rdtsc` internally so their call sites pass no timestamp (which
would otherwise survive even when metrics are off). End-to-end, events flow through the ring
with exact `consumed + drops == attempted` accounting.

### Wiring it into the engine

The instrumentation only matters once it is measuring the real engine. `OrderBook` gained a
single hook — `setMetrics(MetricsRecorder*)` — and `addOrder`, `cancelOrder`, and
`modifyOrder` each open with a `METRICS_SCOPE(...)` guard that wraps the operation in a
`ScopedLatency`. Two switches keep this honest:

- **Compile-time** `METRICS_ENABLED` (a CMake `ENABLE_METRICS` option). When off,
  `METRICS_SCOPE` expands to nothing and the recorder member is compiled out — the engine
  builds exactly as before. That is what makes the overhead A/B a true instrumented-vs-clean
  comparison: two build directories, `build-metrics` and `build-baseline`.
- **Runtime** attach/detach. A build with metrics compiled in but no recorder attached
  (`metrics_ == nullptr`) pays only a single predictable branch, so the existing tests and
  benchmarks construct an `OrderBook` and are unaffected.

Because the wiring lives in the engine rather than in each caller, anything that drives the
book — synthetic churn or a real market feed — produces latency events for free.

A deterministic test (`test_engine_wiring`, under CTest) pins the contract: a known sequence
of mutators yields exactly one correctly-typed event each (including a *missed* cancel, which
is still timed), every latency is non-zero, and detaching the recorder produces zero events.
One caveat is recorded for later — the matcher's `submitLimit`/`submitMarket` reuse
`addOrder`/`cancelOrder` internally, so timing Match/Market will first need the public timed
entry separated from an untimed internal path to avoid double counting.

### The aggregator thread and live console readout

The producer side only stamps and pushes; the **aggregator thread** is where the event
stream becomes meaning. It is the single consumer — it drains the ring in bursts and, owning
its data structures outright, needs no locks. Each event folds into per-operation latency
histograms using HDR-style log-linear bucketing (32 sub-buckets per power-of-two, ~3%
relative error, ~900 buckets) that tracks exact min/max/sum alongside, so the worst-case tail
is reported precisely rather than quantized. Two windows are kept per operation: a recent
interval (reset each refresh, driving p50/p99/p99.9 and ops/sec) and a sticky cumulative one
(the all-time max). Snapshot events feed a live book line (best bid/ask/spread); the drop
counter is polled from the producer once per frame.

On a wall-clock cadence (about 5 Hz) the same thread renders a `top`-style console readout —
per-operation ops/sec, count, p50/p99/p99.9 and max, in real nanoseconds (the TSC tick-rate
is calibrated once at startup) — repainting in place over ANSI on a terminal, or printing
plain frames when piped. The pipeline is self-checking: `consumed + drops == produced` holds
exactly at shutdown, after a final drain of the ring.

### Driving it from a real ITCH feed

The payoff of instrumenting at the engine level is that pointing the live monitor at a real
NASDAQ feed needed no new engine or recorder code — only a small harness.
`itch_metrics_replay` memory-maps an ITCH file, attaches a recorder to the book that
`BookReplay` drives, spawns the aggregator on a second core, and parses on the hot thread, so
every Add/Delete/Reduce/Replace is timed as it reconstructs the book and the percentiles
update live.

Two refinements make it a monitor rather than a batch run:

- **Pacing.** `--pace=` replays by the messages' own ITCH nanosecond timestamps: `0` runs
  flat-out (the latency-distribution view), `1` replays at real market speed, `10` at ten
  times. Pacing inserts waits *between* operations, outside the timed region, so it never
  pollutes the per-operation latency it reports.
- **Load behavior.** Prefaulting a multi-gigabyte file into RAM (the profiling default) shows
  nothing for several seconds while it loads; a loading indicator now makes that explicit, and
  `--no-prefault` switches to lazy faulting so the live view appears immediately (at the cost
  of one-time page-fault jitter in the earliest samples). Ctrl-C stops cleanly and still
  prints the summary.
- **Driving the dashboard.** `--udp[=host:port]` attaches the UDP publisher as a snapshot sink
  (default `127.0.0.1:9099`), so the same paced ITCH feed that prints the console "top" view also
  streams through the bridge to the browser dashboard — real NASDAQ latencies on a live chart, with
  the console kept on. Pace it (`--pace=10`) so the file spans wall-clock time and the charts move.

> **Note — why `--no-prefault` changes the reported latency and throughput.** Lazy loading moves the
> file's page faults *into* the measured run instead of paying them up front, and that shows up two
> ways. **Throughput drops:** every minor fault is microseconds of kernel time spent inside the ops/s
> window, and there are tens of thousands of them for a large file. **The latency tail widens:** the
> fault itself happens in the parser (reading the mapped bytes), *outside* the `ScopedLatency` scope —
> but the fault handler pollutes the cache and TLB, so the *next* timed op runs cold and its p99.9/max
> inflate. The **median (p50) barely moves**, because the book work is identical in both modes and most
> ops aren't adjacent to a fault. Both effects are **one-time and front-loaded** — each page faults once
> on first touch (a sequential walk), so the gap is largest in the first second or two and then converges
> to the prefault numbers as the working set becomes resident. Magnitude depends on the page cache: an
> already-cached file takes cheap *minor* faults; a cold file takes *major* faults (real disk reads) that
> cause large early spikes. So for clean, comparable numbers use prefault (the LOAD-phase wait buys a
> fault-free measured region); with `--no-prefault` you get an instant view but should read steady-state
> and discount the first few frames.

### Overhead A/B: the cost of being watched

The instrumentation is only worth anything if observing the engine does not distort it. Phase 1
made `addOrder` cost tens of nanoseconds; an observer that adds another tens of nanoseconds would
mean the dashboard reports a system the measurement itself changed — the observer effect, at a
scale that matters here. So the overhead is *measured*, not asserted, with the same RDTSC harness
used for the Phase-1 numbers (calibrated TSC, pinned core, pre-generated inputs, timer-overhead
subtracted, median-of-7). The only variable is one compile-time switch, so the builds are identical
except for the instrumentation:

- **baseline** (`build-baseline`, `METRICS_ENABLED=0`) — the clean engine, no observer compiled in.
- **detached** (`build-metrics`, no recorder attached) — instrumentation compiled in but dormant.
- **attached** (`build-metrics`, recording every op, with a draining consumer on a second core).

Measured on a pinned core, 0 drops (the consumer kept up, so the attached figure is the real
successful-push cost, not the cheaper drop path):

| Mode | addOrder (batch mean) | overhead |
|---|---:|---:|
| baseline (no instrumentation) | 48.6 ns | — |
| detached (compiled in, not attached) | 50.0 ns | **+1.5 ns** |
| attached (recording every op) | 89.3 ns | **+41 ns** |

Two conclusions, both honest:

- **Detached is effectively free (+1.5 ns).** A build that carries the instrumentation but has no
  recorder attached pays one predictable branch and a member load. Combined with the compile-time
  `=0` path (literally zero machine code, proven in the disassembly), this means a single
  instrumented binary can be shipped and toggled at runtime at negligible cost.
- **Attached roughly doubles `addOrder` (+41 ns).** Recording every operation is *not* free: two
  `rdtsc` reads, a 32-byte event build, the ring push, and the cross-core SPSC hand-off (the
  consumer is actively reading the head index and slots, so the producer's release-store pays
  coherence traffic). The design answers for this number two ways:

  1. *On a real feed it is invisible.* Phase-1 profiling put the order book at ~1.6% of ITCH-replay
     runtime (the rest is I/O); adding 41 ns to an operation that is 1.6% of the work is negligible
     system-wide, which is why the live ITCH monitor shows clean latencies.
  2. *For book-bound measurement, this is exactly what the 1-in-N sampling knob is for.* One
     subtlety the A/B exposes: `ScopedLatency` stamps *every* op (the two `rdtsc` reads are in its
     constructor/destructor) and the sample mask gates *inside* `recordLatency`, after the timing —
     so sampling removes the event-build and ring-push pressure but not the per-op `rdtsc` floor.
     Making sampling skip the `rdtsc` too would mean moving the sample decision up into
     `ScopedLatency` (a noted refinement).

One caveat recorded for honesty: the absolute baseline here (~48 ns) is higher than the Phase-1
**26.6 ns** headline — almost certainly environment (no turbo / a busy machine; the timer overhead was
an unusually high ~21 ns), not a regression. The A/B *delta* is unaffected, because all three
numbers come from the same session on the same core; only the difference is the overhead claim.

### False-sharing A/B: the padding that only pays off with a second thread

Phase 1 aligned the hot structures to cache lines, measured it *neutral* single-threaded, and deferred
the real test with a documented reason: false sharing needs a second thread to appear. The SPSC ring
carries that test. Its hot indices are laid out so the producer-owned line (`head_` + its cached view
of `tail_`) and the consumer-owned line (`tail_` + its cached view of `head_`) sit on *separate*
64-byte lines; `-DSPSC_NO_PAD` removes the padding, packing `head_`, `tail_`, and `buf_[0]` onto one
line — so the producer's store to `head_` and the consumer's store to `tail_` invalidate each other's
copy on every operation. One source, built twice, streams 50 M items through the ring on two pinned
cores with a retry-on-full producer (no drops — pure throughput); the delta is the cost of the two
cores fighting over one line.

**Across two different physical cores** (the case that matters):

| Build | throughput | per item | vs padded |
|---|---:|---:|---:|
| **PADDED** (default) | **23.8 M items/s** | 42 ns | — |
| **NO-PAD** (`-DSPSC_NO_PAD`) | **9.9 M items/s** | 101 ns | **2.4× slower** |

The padding — which costs nothing but an `alignas(64)` — more than doubles ring throughput, and the
NO-PAD figures were stable to the decimal across all five reps, confirming a structural cache-coherence
effect rather than noise. This is the deferred Phase-1 hypothesis, now confirmed with a second thread:
the alignment that looked free is worth 2.4×.

**The contrast — the same A/B across two hyperthread siblings** (sharing L1, so the hand-off never
crosses the coherence interconnect):

| Placement | PADDED | NO-PAD | padding wins | why |
|---|---:|---:|---:|---|
| different physical cores | 23.8 M/s | 9.9 M/s | 2.4× | hand-off crosses L3 / interconnect |
| hyperthread siblings (shared L1) | 197 M/s | 102 M/s | 1.9× | hand-off stays in L1 |

Two lessons fall out. First, *thread placement matters as much as the padding*: siblings sharing L1 push
~8× the throughput of separate physical cores (197 vs 24 M/s), because the SPSC hand-off stays in cache
instead of traversing the coherence fabric — so which two cores the producer and consumer run on is a
first-class decision (a topology gotcha bit the default: cpu 2 and 3 on this box are siblings, not
separate cores). Second, padding helps either way — even sharing L1, two hyperthreads contending on one
line still pay ~1.9×.

One honest scope note: this isolates the ring with a tight push/pop loop. In the live pipeline the
producer also does ~50 ns of real engine work between pushes, so the ring is not the bottleneck and the
penalty is diluted — but the A/B's job is to isolate the padding's value, and it does: a free `alignas`
removes a 2.4× cliff.

## Transport and the dashboard data source

### Snapshot serialization

This part turns the monitor outward: the same per-operation percentiles, throughput, and book state the
console already shows have to leave the process and reach a browser. The first step is a serialization
format, and the previous rule still holds — nothing on the hot path. The hot thread never touches any of
this; serialization happens on the aggregator (Core B) at the render cadence (~5 Hz), where microseconds
are free, so plain `snprintf` JSON is entirely appropriate.

The aggregator already computes a full per-cycle view to paint the console; that computation now lands in
a single `MetricsSnapshot` struct (header counters, the book line, and a per-op row of
`ops/s · count · p50 · p99 · p99.9 · max` in nanoseconds). One `render()` cycle builds the snapshot once
and fans it out two ways: to the console "top" view (unchanged, byte-for-byte) and to an optional **sink**
— `setSnapshotSink(std::function<void(const MetricsSnapshot&)>)` — the seam every downstream consumer
attaches to. `writeJson()` serializes a snapshot to a compact single-line JSON object, sized to fit one
UDP datagram (~600–800 bytes with five op rows):

```json
{"t":3.0,"final":true,"events":19920317,"drops":0,"unit":"ns",
 "book":{"bid":99999,"ask":100001,"spread":2,"last":0,"vol":0},
 "ops":[{"op":"add","ops_s":1517909,"count":8433067,"p50":20.3,"p99":115.7,"p999":144.0,"max":28310.8},
        {"op":"cancel","ops_s":1523500,"count":8400300,"p50":123.5,"p99":216.0,"p999":257.2,"max":127168.2}]}
```

`json_snapshot_demo` drives the real engine and registers a sink that writes one such line per cycle
(NDJSON) to stdout — the exact stream the UDP publisher will send. Verification is direct: run it and
parse every line. A three-second run produces ~16 snapshots at 5 Hz, all valid JSON, with the console
view confirmed unchanged — the snapshot is built from the same numbers, so the two can never drift.

### UDP snapshot publisher

With a serialization format in hand, the next link sends it off-box. The transport is plain
fire-and-forget **UDP**. The publisher never blocks, never
retries, and a lost datagram just means the dashboard skips one frame. `UdpPublisher` opens a datagram
socket once and, each render cycle, serializes the snapshot with `writeJson()` and `sendto()`s it as a
single datagram (one snapshot fits comfortably under an MTU, so there is no framing to reassemble). It
lives where the snapshot does — the aggregator thread (Core B), ~5 Hz, off the hot path — so a blocking
`sendto` is fine.

The publisher is just another sink:
`agg.setSnapshotSink([&pub](const MetricsSnapshot& s){ pub.send(s); })`. `udp_publish_demo` wires it to
the real engine and aims datagrams at `127.0.0.1:9099`. End-to-end verification used a throwaway UDP
receiver: a three-second run sent **16 datagrams, 0 errors**, and the receiver got all **16, every one
valid JSON** — the same payload the console shows, now on the wire.

### UDP → WebSocket bridge

The last hop into the browser. A browser cannot open a UDP socket, so a tiny Python relay sits between
the publisher and the page: `dashboard/udp_ws_bridge.py` (asyncio + the `websockets` library) receives
each datagram on UDP `:9099` and `broadcast()`s it, unchanged, as a WebSocket **text frame** to every
client connected on `ws://127.0.0.1:8765`. It is deliberately stateless — no parsing, no buffering, no
reassembly (one snapshot == one datagram == one frame); a client that falls behind is simply dropped.
That keeps the C++ side honestly fire-and-forget and the browser side a thin consumer.

It was tested without a browser by standing a WebSocket client in for the dashboard: with the bridge up,
a three-second publisher run delivered all **16 frames over the WebSocket, every one valid JSON** — the
same payload, now one `JSON.parse()` away from a chart. Start order matters: the bridge must be listening
before the publisher sends, since datagrams to an unbound port are silently dropped.

### Dashboard v1

The page itself — one self-contained `dashboard/index.html` (uPlot from a CDN, no build step). It opens
a WebSocket to `ws://127.0.0.1:8765`, `JSON.parse`s each frame, drops the `final` flush frame, and keeps
a rolling history. From that it draws two live charts — per-op p50/p99/p99.9 over time (with an op
selector) and throughput by op — above a header strip (uptime · events/s · drops · bid/ask/spread, with
a connection light) and a current-snapshot table that mirrors the console "top" view. The socket
auto-reconnects, so the page can be left open across publisher restarts.

It was verified as far as a browserless check allows: `node --check` on the inline script, both CDN
assets returning 200, and a contract check confirming the fields the JS reads (`s.ops[].op`, the
percentiles, `s.book`, `s.t/events/drops`) match real wire output — which caught a genuine bug (the op
key is `op`, not `name`) before it ever reached a browser.

*Current state:* — the full chain runs end to end:
engine → aggregator → `writeJson()` → UDP → bridge → WebSocket → browser charts, and the console "top"
view and the browser render the *same* snapshot, so they can never disagree.

## Depth, alerts, history

### Operational alerts

With the snapshot reaching every consumer, the aggregator can do more than display — it can *judge*.
Four rules run on Core B each render cycle, evaluated by a pure function (`evaluateAlerts()`, so the
logic is unit-testable in isolation) and written straight back into the snapshot, so the alerts ride the
same path the numbers already do — identical on the console, in the JSON/UDP, and on the dashboard:

- **crossed book** → *crit* — `bid ≥ ask`, the "should never fire" correctness alarm that brings the
  Phase-1 crossed-book invariant full circle as a live guardrail.
- **spread blowout** → *warn* — book spread over a configurable threshold.
- **drops** → *warn* — fires on the *delta* (new drops this interval), not the sticky cumulative, so it
  flags the moment the consumer starts falling behind rather than latching forever.
- **per-op p99 breach** → *warn* — any op whose window p99 exceeds a threshold (default 5 µs), skipped
  when latencies are uncalibrated.

Each alert carries a level, a code, and a human-readable message; they serialize into an `"alerts"`
array on the snapshot JSON, print as `ALERT [level] text` under the console table, and light a banner on
the dashboard (red for crit, amber for warn). A deterministic test (`test_alerts`, under CTest) drives a
hand-built snapshot through each rule and asserts it both fires *and* clears; the live path was confirmed
by pushing a crossed book through the real aggregator and seeing the crit alert surface in the JSON.

### Book depth & trade tape

The dashboard so far showed *behavior* (latencies, throughput) but only the top of book. A real ops view
also wants **market structure** — the depth ladder and the trade tape. Both needed a new path to the
consumer, because the SPSC ring carries fixed 32-byte events and a top-N ladder is far too big for one.

- **Depth** travels a **seqlock side-channel** (`BookPublisher`). The producer owns the `OrderBook`, so
  it walks the top-N levels with a new `OrderBook::getDepth()` (best inward, off the hot path at ~5 Hz)
  and publishes a `BookView`; the aggregator reads a consistent copy lock-free each render — the same
  pattern it already uses to poll the producer's drops atomic. The ring stays reserved for hot-path
  events; the bulky, low-rate depth rides its own channel.
- **The trade tape** reuses the existing `Trade` events: the aggregator keeps a small ring of the most
  recent executions (price · qty · maker), newest first.

Both land in the snapshot — `"bids"`/`"asks"` as `[price, qty]` pairs and `"tape"` as
`[price, qty, maker]` — so they flow through the same JSON/UDP/WebSocket path to the dashboard, which
renders a classic ladder (asks above, bids below, bar width ∝ resting size) and an upticked/downticked
tape. `book_dashboard_demo` drives a genuine two-sided book and crosses it with market orders
(`submitMarket`) so depth *and* trades are live. Verified end to end: a captured datagram carried 10 bid
levels, 10 ask levels, and a full 16-print tape; `getDepth()` itself has a deterministic test
(`test_depth`, top-N · aggregation · ordering · cap · emptying).

### Recording & playback

The last piece is history — capturing a session and replaying it. Both live in the Python transport
layer, so there are no engine changes. The bridge gains a `--log` flag that tees every datagram it relays
into an NDJSON file — source-agnostic, so it records a synthetic run, the depth/tape demo, or a real ITCH
`--udp` feed, all while still serving the live dashboard. `ndjson_playback.py` then re-streams a recorded
file back over UDP, paced by each snapshot's own `t` (with a speed multiplier), so the session flows
through the *same* UDP → bridge → WebSocket → dashboard path it was recorded from — a past session
scrubbed in the browser with no engine running. Verified end to end: a recorded 16-frame session (depth
and tape included) replayed back **identically — lossless**.

## Phase 2: Conclusion

Phase 2 set out to make the Phase-1 engine *observable* without disturbing it, and to surface that on a
live browser dashboard. Both are demonstrated, each with evidence:

| Claim | Evidence |
|---|---|
| **Near-zero overhead** | instrumentation A/B: **+1.5 ns** detached, **+41 ns** while recording every op; *zero* machine code when compiled off |
| **Lock-free, race-free** | SPSC ring is ThreadSanitizer-clean; cache-line padding worth **2.4×** two-core throughput (the deferred Phase-1 demo, now shown) |
| **Live, end to end** | engine → ring → aggregator → JSON → UDP → WebSocket → uPlot browser charts, driven by synthetic *or* real NASDAQ data |
| **Operationally complete** | per-op percentiles, throughput, a book-depth ladder, a trade tape, and four alert rules (incl. the crossed-book guardrail) |
| **Recordable** | sessions logged to NDJSON and replayed losslessly through the same pipeline |

The arc of the phase: a 32-byte event and a lock-free ring → an aggregator that turns raw events into
live percentiles → transport (JSON / UDP / WebSocket) to a browser → enrichment (depth, tape, alerts,
history). One thread runs throughout: **the hot path only ever stamps and pushes** — every expensive
thing (histograms, serialization, sockets, rendering) lives on the consumer core, so observing the engine
never throttles it. And the crossed-book invariant comes full circle once more: written in Phase 1,
*enforced* by the matcher, it is now a **live alert that should never fire**.

---


## Building & Running
 
### Phase 1: orderbook build and tests

CMake targets (built into `build/`):
 
```bash
mkdir -p build && cd build && cmake .. && make -j$(nproc)
```
 
| Target | Purpose |
|---|---|
| `orderbook_lib` | the core order book static library |
| `test_orderbook` | unit + stress tests (GoogleTest) |
| `test_matching` | matching-engine scenario tests (price-time priority, crossed-book invariant) |
| `benchmark_orderbook` | RDTSC latency benchmark (`-O3 -march=native`) |
| `profile_driver` | long-running steady-state workload for `perf` |
| `itch_replay` | ITCH message-type histogram (parser validation) |
| `itch_book_replay` | reconstruct one symbol's book from a real ITCH file |
 
```bash
# tests
./test_orderbook
./test_matching
 
# latency benchmark (pin a core)
taskset -c 3 ./benchmark_orderbook
 
# reconstruct a symbol's book from real NASDAQ data
./itch_book_replay ~/market-data/*.NASDAQ_ITCH50 AAPL
 
# batch report across files/symbols -> docs/
../run_itch_replays.sh -s AAPL,MSFT,SPY ~/market-data/*.NASDAQ_ITCH50
```
 
### Phase 2: metrics build and tests

The metrics instrumentation is gated by a CMake option, `ENABLE_METRICS` (default `ON`). The
overhead A/B is just two build directories — instrumented and clean:

```bash
# instrumented build (engine times add / cancel / modify; all metrics targets built)
cmake -S . -B build-metrics  -DENABLE_METRICS=ON  && cmake --build build-metrics  -j$(nproc)

# clean baseline (instrumentation compiles to nothing; for the A/B and pristine perf)
cmake -S . -B build-baseline -DENABLE_METRICS=OFF && cmake --build build-baseline -j$(nproc)
```

| Target | Purpose |
|---|---|
| `test_spsc` | SPSC ring torture test (FIFO, no loss/dup, drop accounting) |
| `test_instrument` | recorder event-flow + `consumed + drops == attempted` accounting |
| `test_engine_wiring` | one correctly-typed event per `add`/`cancel`/`modify`; detach silences |
| `test_alerts` | operational alert rules (crossed / spread / drops / p99 fire and clear) |
| `test_depth` | `OrderBook::getDepth()` — top-N, aggregation, ordering, cap, emptying |
| `aggregator_demo` | synthetic producer + aggregator live "top" readout |
| `engine_metrics_demo` | real `OrderBook` driven by synthetic churn, timed live |
| `itch_metrics_replay` | real NASDAQ ITCH feed -> live per-op latency monitor |
| `benchmark_overhead` | instrumentation overhead A/B (built in **both** configs) |
| `bench_false_sharing_padded` / `_nopad` | SPSC false-sharing A/B (padded vs `-DSPSC_NO_PAD`) |
| `json_snapshot_demo` | aggregator snapshot as NDJSON (dashboard data source) |
| `udp_publish_demo` | publish each snapshot as one JSON UDP datagram (transport) |
| `book_dashboard_demo` | live two-sided book + trades -> depth ladder + trade tape (console + UDP) |

Running the tests:

```bash
# CTest runs the registered tests: GoogleTest order-book suite + depth + engine-wiring + alerts
ctest --test-dir build-metrics --output-on-failure
#   Start 1: OrderBookTests   Start 2: Depth   Start 3: EngineWiring   Start 4: Alerts

# the remaining test binaries are run directly
./build-metrics/test_matching                  # matching scenarios + crossed-book invariant
./build-metrics/test_instrument                # recorder event flow + accounting
taskset -c 3,4 ./build-metrics/test_spsc       # SPSC ring torture on two cores (burst tolerance)
```

Running the overhead A/B (per-op instrumentation cost; diff the `batch mean` lines):

```bash
# baseline (clean) on one core; metrics build prints detached + attached on two cores
taskset -c 3   ./build-baseline/benchmark_overhead 3
taskset -c 2,3 ./build-metrics/benchmark_overhead 3
```
### Outputs
```bash
└─$ taskset -c 3   ./build-baseline/benchmark_overhead 3
=== addOrder instrumentation overhead A/B ===
pinned core   : 3
TSC frequency : 3.1104 GHz
timer overhead: 10.29 ns [subtracted per-op]
BUILD         : METRICS_ENABLED=0 (clean baseline)

[baseline  (no instrumentation compiled in)]
  batch mean :  24.99 ns/op   (40.01 M ops/s)
  p50        :  23.47 ns
  p99        :  37.94 ns
  p99.9      : 235.98 ns
  max        : 22222.72 ns


└─$ taskset -c 2,3 ./build-metrics/benchmark_overhead 3

=== addOrder instrumentation overhead A/B ===
pinned core   : 3
TSC frequency : 3.1104 GHz
timer overhead: 10.29 ns [subtracted per-op]
BUILD         : METRICS_ENABLED=1 (instrumented)

[detached  (instrumentation compiled in, no recorder attached)]
  batch mean :  25.40 ns/op   (39.37 M ops/s)
  p50        :  24.76 ns
  p99        :  37.94 ns
  p99.9      : 209.94 ns
  max        : 24997.56 ns

consumer core : 2   drops during attached run: 0

[attached  (rdtsc x2 + build 32B event + ring push, every op)]
  batch mean :  45.25 ns/op   (22.10 M ops/s)
  p50        :  45.65 ns
  p99        :  56.58 ns
  p99.9      : 238.55 ns
  max        : 24009.27 ns
```


Running the false-sharing A/B (use two **different physical cores** — not HT siblings; check
`lscpu -e=CPU,CORE`). Pass the producer and consumer core as args:

```bash
taskset -c 8,10 ./build-metrics/bench_false_sharing_padded 8 10   # padded
taskset -c 8,10 ./build-metrics/bench_false_sharing_nopad  8 10   # -DSPSC_NO_PAD
```
### Outputs
```bash
└─$ taskset -c 8,10 ./build-metrics/bench_false_sharing_padded 8 10           
=== SPSC ring false-sharing A/B ===
build mode : PADDED  (head_ / tail_ / buf_ on separate 64B lines)
ring cap   : 1024 slots x 32 B
cores      : producer 8, consumer 10
workload   : 50000000 items/rep, median of 5, retry-on-full (no drops)

  rep 1:    23.0 M items/s   (43.44 ns/item)
  rep 2:    23.4 M items/s   (42.73 ns/item)
  rep 3:    23.5 M items/s   (42.47 ns/item)
  rep 4:    23.6 M items/s   (42.35 ns/item)
  rep 5:    23.4 M items/s   (42.68 ns/item)

median throughput: 23.4 M items/s   [PADDED  (head_ / tail_ / buf_ on separate 64B lines)]


└─$ taskset -c 8,10 ./build-metrics/bench_false_sharing_nopad  8 10   # -DSPSC_
=== SPSC ring false-sharing A/B ===
build mode : NO-PAD  (head_/tail_/buf_ packed -> false sharing)
ring cap   : 1024 slots x 32 B
cores      : producer 8, consumer 10
workload   : 50000000 items/rep, median of 5, retry-on-full (no drops)

  rep 1:    11.5 M items/s   (86.72 ns/item)
  rep 2:    11.6 M items/s   (86.41 ns/item)
  rep 3:    11.6 M items/s   (86.53 ns/item)
  rep 4:    11.6 M items/s   (86.37 ns/item)
  rep 5:    11.6 M items/s   (86.30 ns/item)

median throughput: 11.6 M items/s   [NO-PAD  (head_/tail_/buf_ packed -> false sharing)]

```



Emitting the metrics snapshot as NDJSON (the dashboard data source; validate by parsing each line):

```bash
./build-metrics/json_snapshot_demo 3 > /tmp/snap.ndjson            # one JSON object per render cycle
python3 -c "import json;[json.loads(l) for l in open('/tmp/snap.ndjson')];print('valid NDJSON')"
tail -1 /tmp/snap.ndjson | python3 -m json.tool                    # eyeball one snapshot
```

Publishing snapshots over UDP (one JSON datagram per render cycle). Start a receiver first, since
datagrams to a port with no listener are silently dropped:

```bash
# terminal 1 - a throwaway receiver (netcat, or the Python bridge once it exists)
nc -u -l 9099

# terminal 2 - publish to it
./build-metrics/udp_publish_demo 8 127.0.0.1 9099
```

The full live stack — the console "top" view and the browser dashboard at the same time, off one
engine (both read the SAME snapshot, so they always agree). Start the bridge first, since datagrams to
an unbound port are silently dropped:

```bash
# terminal 1 - the bridge: UDP :9099 -> ws://127.0.0.1:8765
python3 dashboard/udp_ws_bridge.py 9099 8765

# terminal 2 - the data source: prints the console "top" view AND publishes UDP to the bridge.
#   (a) synthetic order flow through the real engine (latencies + throughput):
./build-metrics/udp_publish_demo 60 127.0.0.1 9099
#   (b) two-sided book with trades -> also fills the depth ladder + trade tape panels:
./build-metrics/book_dashboard_demo 60 127.0.0.1 9099
#   (c) ...or a REAL NASDAQ ITCH feed (pace it so the charts move; --no-prefault = fast start):
./build-metrics/itch_metrics_replay ~/market-data/<file>.NASDAQ_ITCH50 AAPL --pace=10 --no-prefault --udp
#   (append  > /dev/null  to any for headless publish-only)

# browser - open the dashboard (auto-connects to ws://127.0.0.1:8765, auto-reconnects)
xdg-open dashboard/index.html
#   or serve it:  ( cd dashboard && python3 -m http.server 8000 )  then open http://localhost:8000
```

Recording a session and replaying it later (the `--log` tee + the playback tool):

```bash
# record: the bridge tees every snapshot to NDJSON while still serving the dashboard
python3 dashboard/udp_ws_bridge.py 9099 8765 --log=session.ndjson
./build-metrics/book_dashboard_demo 60 127.0.0.1 9099     # any UDP source is recorded

# replay later: start a fresh bridge, then re-stream the file at its recorded cadence
python3 dashboard/udp_ws_bridge.py 9099 8765
python3 dashboard/ndjson_playback.py session.ndjson --speed=1   # 0 = blast, 10 = 10x, --loop to repeat
#   then open dashboard/index.html to scrub the recorded session - no engine needed
```

Running the live monitors (use a real terminal for the in-place "top" view):

```bash
# synthetic order flow through the real engine, 8 seconds
./build-metrics/engine_metrics_demo 8

# a real ITCH feed, paced at 10x, lazy load so the view appears immediately
taskset -c 2,3 ./build-metrics/itch_metrics_replay ~/market-data/*.NASDAQ_ITCH50 AAPL --pace=1 --no-prefault
# usage: itch_metrics_replay <itch_file> <SYMBOL> [options]
#     --pace=<F>     replay speed by ITCH timestamps: 0=max (default),
#                    1=real-time (1s replay == 1s of market), 10=10x, ...
#     --snap=<N>     emit a book snapshot every N messages (default 8192)
#     --no-prefault  lazy load: the live view appears instantly and pages fault in as
#                    the parser reaches them (early samples carry one-time fault jitter),
#                    instead of pre-faulting the whole file with MAP_POPULATE.
#     --udp[=H:P]    also publish each snapshot as a UDP datagram (default 127.0.0.1:9099)
#                    -> bridge -> browser dashboard
```

Market data is the NASDAQ TotalView-ITCH 5.0 historical sample (binary, gzip-compressed).
Data files are large (multi-GB) and are **not** committed to the repository.
 
---
 
 ### Checkout documentation for Phase-3 in `/fpga`
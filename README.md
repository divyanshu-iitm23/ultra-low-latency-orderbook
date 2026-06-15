# Ultra-Low-Latency Order Book

A high-performance limit order book written in C++, built as the software core of a
three-part HFT systems- orderbook, FPGA packet parser and real-time latency dashboard. The focus is not just on *being* fast, but on being fast
**predictably** — minimizing the latency *tail*, which is what actually matters in
high-frequency trading, where a single multi-microsecond spike is a lost trade.

Every performance claim in this document was measured on real hardware. Here's my setup :
- OS/Kernel : Kali Linux
- CPU Architecture : x86 (16 cores, profiling done on single pinned core)
- IDE : vsCode
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
│   ├──  Instrument book (ops/sec, latency percentiles, depth, spread)
│   ├──  Lock-free metrics collection (ring buffer / shared memory)
│   ├──  Apply the false-sharing padding pattern HERE (writer vs reader thread)
│   └──  Export mechanism
│
└── # Real-time visualization
    |
    ├──  Metrics-streaming backend
    ├──  Dashboard (latency histograms, book depth, throughput)
    └──  Alerting + historical playback

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

### 1. Object pool allocator — **kept**
Removed `new`/`delete` from the hot path. Eliminated the dominant allocation-induced tail
spike (early chrono-based measurement showed a ~775 µs worst-case sample collapse). Also
improved typical-case latency via cache locality. *The first and most impactful correctness-
for-determinism change.*

### 2. Cache-line alignment of `Order` — **kept (neutral)**
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

### 4. `std::map` → direct-mapped array + occupancy bitmap — **kept (largest win)**

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

### 6. Symbol filter: `strcmp` → `uint64_t` compare — **kept**
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
- **Scope judgment.** The matcher implements the hard core (price-time priority, partial
  fills, maker pricing) and *names* its omissions (IOC/FOK, self-trade prevention, icebergs,
  the trade-vector allocation) instead of hiding them. The ladder's memory trade-off and the
  cents-conversion are likewise documented limits, not surprises.
- **Honest verification discipline.** Code is verified for correctness in a sandbox; every
  performance number is measured on real hardware under controlled conditions.
---
 
## Building & Running
 
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
./itch_book_replay ~/market-data/sample_500mb.ITCH50 AAPL
 
# batch report across files/symbols -> docs/
../run_itch_replays.sh -s AAPL,MSFT,SPY ~/market-data/*.NASDAQ_ITCH50
```
 
Market data is the NASDAQ TotalView-ITCH 5.0 historical sample (binary, gzip-compressed).
Data files are large (multi-GB) and are **not** committed to the repository.
 
---
 
 
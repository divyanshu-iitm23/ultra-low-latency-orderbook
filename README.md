# Ultra-Low-Latency Order Book

A high-performance limit order book written in C++, built as the software core of a
three-part HFT systems-engineering portfolio (order book → FPGA packet parser →
real-time latency dashboard). The focus is not just on *being* fast, but on being fast
**predictably** — minimizing the latency *tail*, which is what actually matters in
high-frequency trading, where a single multi-microsecond spike is a lost trade.

Every performance claim in this document was measured on real hardware (16-core bare-metal
Linux) using a calibrated, cycle-accurate benchmark harness with a pinned core and
median-of-seven runs. Numbers from a constrained verification environment are never quoted
as results.

---

## Roadmap

```
PHASE 1: Order Book Engine (Software) ──────────────────────── IN PROGRESS
│
├── Week 1-2: Foundation & Basic Implementation ............... COMPLETE
│   ├── Core types, Order, PriceLevel, OrderBook
│   ├── add / cancel / modify / execute (market) operations
│   ├── Fixed-point pricing, intrusive FIFO price levels
│   └── 13 unit + stress tests (correctness verified to 1M ops)
│
├── Week 3-4: Performance Optimization ...................... ~70% COMPLETE
│   ├── Object pool allocator ............................... DONE
│   ├── Cache-line alignment of Order ...................... DONE (kept as documentation)
│   ├── False-sharing elimination .......................... DEFERRED → Phase 2
│   ├── RDTSC benchmark harness ............................ DONE
│   ├── std::map → direct-mapped array + bitmap ............ DONE (largest win)
│   ├── Replace orders_ hash map ........................... TRIED & REVERTED (made it worse)
│   ├── perf profiling + flamegraphs ...................... NEXT
│   └── Final before/after write-up ....................... pending
│
└── Week 5: Market Data Integration & Matching Engine ........ NOT STARTED
    ├── NASDAQ ITCH 5.0 binary parser
    ├── Replay real feed + validate book state
    └── Limit-order matching engine (crossing detection, trades)

PHASE 2: Real-Time Monitoring Dashboard (Ops) ............... NOT STARTED
PHASE 3: FPGA UDP Packet Parser (Hardware) ................. NOT STARTED
```

**Current position:** end of Phase 1's optimization work — roughly 25-30% through the
full project. `addOrder` has gone from a **104.8 ns mean with a 665 µs worst case** to a
**26.6 ns mean with a 24 µs worst case**, measured cleanly on the target hardware.

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

### Known remaining allocation source

`std::unordered_map` (the order-lookup index) still allocates a node per insert. Profiling
is the next step to determine whether this — or something else (e.g. first-touch page
faults) — is the source of the remaining ~24 µs worst-case sample.

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

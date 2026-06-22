# Project Architecture / Pipeline

### Phase-1 ---> **Ultra Low Latency C++ Orderbook** *(Software)*
### Phase-2 ---> **Real Time Monitoring Dashboard** *(Operations)*
### Phase-3 ---> **FPGA UDP Packet Parser** *(Hardware/Simulations)*

> Legend:  `═` built & tested   ·   `┄` planned (not yet built)

---

## PHASE 1 — Order Book Engine

### 1A · How an order is built, stored, and retired

```
   addOrder(side, price, qty, id)              cancelOrder(id) / modifyOrder(id)
              │                                              │
              ▼                                              ▼
   ┌────────────────────────────────────────────────────────────────────────┐
   │                                OrderBook                               │
   │             (owns the 4 structures below; orchestrates every op)       │
   └────────────────────────────────────────────────────────────────────────┘
       (1) allocate        (2) append          (3) set/clear        (4) index
            │                   │                    │                   │
            ▼                   ▼                    ▼                   ▼
   ┌────────────────┐  ┌────────────────┐  ┌────────────────┐  ┌────────────────┐
   │ ObjectPool     │  │  PRICE LADDER  │  │ OCCUPANCY      │  │  ID INDEX      │
   │ <Order>        │  │ vector<        │  │ BITMAP         │  │ unordered_map  │
   │                │  │   PriceLevel>  │  │ vector<u64>    │  │ <OrderId,      │
   │ intrusive free │  │                │  │                │  │  Order*>       │
   │ list · O(1)    │  │ idx = price    │  │ 1 bit / level  │  │                │
   │ no hot malloc  │  │     - min_price│  │ clz/ctz scan → │  │ O(1) lookup    │
   │                │  │ O(1) price→lvl │  │ best bid / ask │  │ for cancel/mod │
   └────────────────┘  └────────────────┘  └────────────────┘  └────────────────┘
         │                     │                    │
     Order* (64B)        bid_levels_[i]        best_bid_idx_
     handed out          ask_levels_[i]        best_ask_idx_
```

Hot-path steps — **add:** (1) pull an `Order` slot from the pool → (2) append it to
the `PriceLevel` at `idx=price-min_price` → (3) set that level's occupancy bit →
(4) record `id→Order*`. **cancel/modify:** map lookup → unlink from the level →
if the level emptied, clear its bit and bit-scan a new best → return the slot to the pool.

### 1B · In-memory layout (the structures, related)

```
  ObjectPool<Order> : contiguous Slot[] chunks on the heap
  ┌──────────────────────────────────────────────────────────┐
  │ Slot = union { Order (64B, one cache line) | Slot* next }│   free slots are
  │  [Order][Order][ free ]──next──►[ free ]──next──► … null │   threaded into an
  └──────────────────────────────────────────────────────────┘   intrusive free list
            ▲          ▲
            │ live Order pointers
            │
  PRICE LADDER (direct-mapped)            OCCUPANCY BITMAP (parallel to ladder)
  idx:  …  9947   9948   9949  …          word: …01000100…  ← 1 bit per level
        ┌──────┬──────┬──────┐                  └──┬────┘
        │ PL   │ PL   │ PL   │   ask_levels_       clz → lowest set = best ask
        └──────┴───┬──┴──────┘                     ctz → highest set = best bid
                   │ PriceLevel @ price
                   ▼
        head_ ─► Order ─► Order ─► Order ◄─ tail_     intrusive doubly-linked
                  ▲  next ▲  next                      FIFO  (price-time priority);
                  └─ prev └─ prev                      next/prev live IN the Order
        total_quantity_ · order_count_

  ID INDEX:  unordered_map<OrderId, Order*> ─-─┐
             (cancel/modify entry point)       └──► the same Order objects above
```

### 1C · Header / source dependency graph (`A → B` = *A includes B*)

```
                         ┌────────────────────────────┐
                         │  src/orderbook.cpp         │  ← the compiled TU
                         │  (impl: add/cancel/modify/ │     → orderbook_lib
                         │   match/submitLimit/Market)│
                         └──────────────┬─────────────┘
                                        ▼
                              ┌────────────────────┐
                              │   orderbook.hpp    │  OrderBook, Trade,
                              │ (ladder+bitmap+map)│  price↔index, bit-scan
                              └───────┬──────┬─────┘
                                      ▼      ▼
                        ┌──────────────────┐  ┌────────────────────┐
                        │ price_level.hpp  │  │  object_pool.hpp   │  (std only)
                        │   PriceLevel     │  │   ObjectPool<T>    │
                        └────────┬─────────┘  └────────────────────┘
                                 ▼
                          ┌────────────┐
                          │ order.hpp  │   Order (alignas(64))
                          └─────┬──────┘
                                ▼
                          ┌────────────┐
                          │ types.hpp  │   Price/Quantity/OrderId, Side/OrderType
                          └────────────┘

   Standalone (not in the include tree above):
     rdtsc_timer.hpp   calibrated TscClock + RDTSC harness  (used by benchmarks)
     itch_parser.hpp / book_replay.hpp   NASDAQ ITCH-5.0 feed → book ops
```

---

## PHASE 2 — Real-Time Monitoring Dashboard

### 2A · Runtime: hot thread → ring → metrics thread → transport

```
        CORE A  — latency-critical hot path        │   CORE B — analysis (may be slow)
  ════════════════════════════════════════════     │  ═══════════════════════════════════
  ┌───────────────────────────────────────────┐    │   ┌──────────────────────────────────┐
  │ OrderBook   add / cancel / modify / match │    │   │            Aggregator            │
  └────────────────────┬──────────────────────┘    │   │                                  │
                       │ wrapped by                │   │  try_pop  ── burst drain ──┐     │
                       ▼                           │   │     │                      │     │
              ┌──────────────────┐                 │   │     ▼                      │     │
              │  ScopedLatency   │ rdtsc t0 / t1   │   │  ingest():                 │     │
              │  (RAII, on stack)│                 │   │   LatencyHistogram[op]     │     │
              └────────┬─────────┘                 │   │     interval + cumulative  │     │
                       ▼                           │   │   book / trade / volume    │     │
              ┌──────────────────┐                 │   │   drops (polled) ◄─────────┼──┐  │
              │ MetricsRecorder  │ record{Latency, │   │     │                      │  │  │
              │                  │ Snapshot, Trade}│   │     ▼                      │  │  │
              │ build Metrics    │ → 32-byte POD   │   │  render @≈5Hz →            │  │  │
              │ Event (32B)      │                 │   │   console "top" view ──────┘  │  │
              └────────┬─────────┘                 │   │   (p50/p99/p99.9/max · ops/s) │  │
                       │ try_push                  │   └───────────────┬───────────────┘  │
                       ▼                           │                   ┊ snapshot (JSON ~10Hz)
        ╔═══════════════════════════════╗          │                   ▼          [planned]
        ║  EventRing = SPSCRing<        ║          │             ┌---------------------┐
        ║   MetricsEvent, 64K>          ║◄─────────┘             | UDP publisher (C++) |
        ║  lock-free · release/acquire  ║   try_pop              └──────────┬──────────┘
        ║  padded · cached indices      ║                                  ▼  [planned]
        ╚═══════════════════════════════╝                         ┌---------------------┐
                       ▲                                          | UDP→WebSocket bridge|
              full? → drops_++ (atomic, never blocks)             |       (Python)      |
                       │ dropsCounter()  ────────────────────────►|          │          |
                       (read live by the aggregator)              └----------|----------┘
                                                                             ▼  [planned]
                                                                  ┌---------------------┐
                                                                  | Browser dashboard   |
                                                                  | (uPlot, percentiles |
                                                                  | · histogram · book) |
                                                                  └─────────────────────┘
```

Key property: the hot path only **stamps and pushes** (a couple of `rdtsc` reads, a
32-byte fill, one `release` store — or, when full, a single atomic `drops_++`). It
**never blocks, allocates, locks, or syscalls.** The SPSC ring is the cross-core
boundary; everything expensive (histograms, percentiles, rendering, transport) lives
on Core B where it can never throttle the engine. Drop-on-full is the release valve —
`consumed + drops == produced` always holds.

### 2B · Header / source dependency graph (`A → B` = *A includes B*)

```
   metrics/aggregator_demo.cpp        test_instrument.cpp      test_spsc.cpp
   (producer + aggregator demo)       (event-flow check)       (ring torture)
        │        │       │                      │                │       │
        │        │       └──► rdtsc_timer.hpp   │                │       │
        │        │            (Phase-1 TscClock)|                │       │
        ▼        ▼                              ▼                ▼       ▼
  ┌───────────┐  ┌──────────────────-──┐   ┌──────────────┐  ┌──────────┐ │
  │aggregator │  │ metrics_recorder.hpp│   │metrics_      │  │spsc_ring │ │
  │  .hpp     │  │  MetricsRecorder,   │   │recorder.hpp  │  │  .hpp    │◄┘
  │           │  │  ScopedLatency      │   └──────┬───────┘  └────┬─────┘
  │ Aggregator│  │  (METRICS_ENABLED)  │          │               │
  └──┬─────┬──┘  └─────────┬────┬──────┘          │               │
     │     │               │    └──► tsc.hpp ◄────┘               │
     │     │               ▼        (rdtsc)                       │
     │     │     ┌────────────────────┐                           │
     │     └────►│ metrics_config.hpp │  EventRing typedef,       │
     │           │  RING_CAP = 64K    │  ring depth               │
     │           └─────────┬──────────┘                           │
     │                     ├───────────────► spsc_ring.hpp ◄──────┘
     │                     │                  SPSCRing<T,Cap>
     │                     └───────────────► metrics_event.hpp
     │                                        MetricsEvent (32B POD)
     └────────────────────────────────────► latency_histogram.hpp
                                              LatencyHistogram (HDR-style)
```

```
   Module map (metrics/):
     tsc.hpp                cheap rdtsc() read
     metrics_event.hpp      32-byte POD event (Latency / Snapshot / Trade)
     spsc_ring.hpp          SPSC lock-free ring (SPSC_NO_PAD toggles padding)
     metrics_config.hpp     wires ring + event → EventRing, sets depth
     metrics_recorder.hpp   hot-path producer: ScopedLatency + MetricsRecorder
     latency_histogram.hpp  log-linear percentile histogram (consumer side)
     aggregator.hpp         consumer thread: drain → histograms → "top" readout
     aggregator_demo.cpp    end-to-end demo (synthetic producer + aggregator)
```

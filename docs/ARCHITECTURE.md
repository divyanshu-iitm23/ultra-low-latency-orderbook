# Project Architecture / Pipeline

### Phase-1 ---> **Ultra Low Latency C++ Orderbook** *(Software)*
### Phase-2 ---> **Real Time Monitoring Dashboard** *(Operations)*
### Phase-3 ---> **FPGA UDP Packet Parser** *(Hardware/Simulations)*

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
   │ no hot malloc  │  │     - min_price│  │ clz/ctz scan-> │  │ O(1) lookup    │
   │                │  │O(1) price->lvl │  │ best bid / ask │  │ for cancel/mod │
   └────────────────┘  └────────────────┘  └────────────────┘  └────────────────┘
         │                     │                    │
     Order* (64B)        bid_levels_[i]        best_bid_idx_
     handed out          ask_levels_[i]        best_ask_idx_
```

Hot-path steps — **add:** (1) pull an `Order` slot from the pool -> (2) append it to
the `PriceLevel` at `idx=price-min_price` -> (3) set that level's occupancy bit ->
(4) record `id->Order*`. **cancel/modify:** map lookup -> unlink from the level ->
if the level emptied, clear its bit and bit-scan a new best -> return the slot to the pool.

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
        │ PL   │ PL   │ PL   │   ask_levels_       clz -> lowest set = best ask
        └──────┴───┬──┴──────┘                     ctz -> highest set = best bid
                   │ PriceLevel @ price
                   ▼
        head_ ─► Order ─► Order ─► Order ◄─ tail_     intrusive doubly-linked
                  ▲  next ▲  next                      FIFO  (price-time priority);
                  └─ prev └─ prev                      next/prev live IN the Order
        total_quantity_ · order_count_

  ID INDEX:  unordered_map<OrderId, Order*> ─-─┐
             (cancel/modify entry point)       └──► the same Order objects above
```

### 1C · Header / source dependency graph (`A -> B` = *A includes B*)

```
                         ┌────────────────────────────┐
                         │  src/orderbook.cpp         │  ← the compiled TU
                         │  (impl: add/cancel/modify/ │     -> orderbook_lib
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
     itch_parser.hpp / book_replay.hpp   NASDAQ ITCH-5.0 feed -> book ops
```

---

## PHASE 2 — Real-Time Monitoring Dashboard

### 2A · Runtime: hot thread -> ring -> metrics thread -> transport

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
              │ build Metrics    │ -> 32-byte POD  │   │  render @≈5Hz ->           │  │  │
              │ Event (32B)      │                 │   │   console "top" view ──────┘  │  │
              └────────┬─────────┘                 │   │   (p50/p99/p99.9/max · ops/s) │  │
                       │ try_push                  │   └───────────────┬───────────────┘  │
                       ▼                           │                   ┊ snapshot (JSON ~5Hz)
        ╔═══════════════════════════════╗          │                   ▼          
        ║  EventRing = SPSCRing<        ║          │             ┌---------------------┐
        ║   MetricsEvent, 64K>          ║◄─────────┘             | UDP publisher (C++) |
        ║  lock-free · release/acquire  ║   try_pop              └──────────┬──────────┘
        ║  padded · cached indices      ║                                  ▼  
        ╚═══════════════════════════════╝                         ┌---------------------┐
                       ▲                                          |UDP->WebSocket bridge|
              full? -> drops_++ (atomic, never blocks)            |       (Python)      |
                       │ dropsCounter()  ────────────────────────►|          │          |
                       (read live by the aggregator)              └----------|----------┘
                                                                             ▼  
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

### 2B · Header / source dependency graph (`A -> B` = *A includes B*)

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
      include/
        |- tsc.hpp                cheap rdtsc() read
        |- metrics_event.hpp      32-byte POD event (Latency / Snapshot / Trade)
        |- spsc_ring.hpp          SPSC lock-free ring (SPSC_NO_PAD toggles padding)
        |- metrics_config.hpp     wires ring + event -> EventRing, sets depth
        |- metrics_recorder.hpp   hot-path producer: ScopedLatency + MetricsRecorder
        |- latency_histogram.hpp  log-linear percentile histogram (consumer side)
        |- aggregator.hpp         consumer thread: drain -> histograms -> "top" readout
        |- metrics_snapshot.hpp   MetricsSnapshot + writeJson() (consumer-side, ~5 Hz)
        |- udp_publisher.hpp      UdpPublisher: one snapshot -> one JSON UDP datagram
     aggregator_demo.cpp    end-to-end demo (synthetic producer + aggregator)
     engine_metrics_demo.cpp  real OrderBook + synthetic churn, timed live
     itch_metrics_replay.cpp  real NASDAQ ITCH feed -> BookReplay -> live readout (paced)
     json_snapshot_demo.cpp   aggregator snapshot -> NDJSON on stdout
     udp_publish_demo.cpp     aggregator snapshot -> JSON UDP datagrams
     test_engine_wiring.cpp   deterministic: one typed event per add/cancel/modify
     test_instrument.cpp
     test_spsc.cpp
```

Note: the engine now depends on the metrics headers — `orderbook.hpp` includes
`metrics_recorder.hpp`, so `ScopedLatency` / `METRICS_SCOPE` are visible inside
`src/orderbook.cpp`. With `METRICS_ENABLED=0` that include resolves to empty stubs.

### 2C · Wiring metrics into the engine

```
  BUILD SWITCH   ENABLE_METRICS (CMake option)  ->  METRICS_ENABLED -> {0,1}
    ON   build-metrics/    instrumentation compiled into the engine
    OFF  build-baseline/   METRICS_SCOPE -> nothing, recorder member #if'd out
                           (the clean control; overhead A/B = these two builds)

  INCLUDE        orderbook.hpp -> metrics_recorder.hpp   (ScopedLatency, METRICS_SCOPE)

  RUNTIME HOOK   OrderBook::setMetrics(MetricsRecorder*)   attach / detach (null = off)

     addOrder    ┐
     cancelOrder ├─ each body opens with  METRICS_SCOPE(metrics_, OpType::…)
     modifyOrder ┘        │
                          ├─ metrics_ == nullptr -> one predictable branch, no record
                          └─ metrics_ != nullptr -> ScopedLatency times the body
                                                    -> recordLatency -> ring.try_push
```

Instrumenting at the engine (not each caller) is what lets *any* driver — synthetic churn,
`engine_metrics_demo`, or a real ITCH feed — be timed for free. `match` / `executeMarketOrder`
are intentionally **not** wired yet: they reuse `cancelOrder` / `addOrder` internally, so
timing them now would double-count (the public timed entry must first be split from an
untimed internal path).

### 2D · Live replay from a real ITCH feed (`itch_metrics_replay`)

The payoff of engine-level wiring: pointing the live monitor at a real NASDAQ feed needs no
new engine or recorder code — only a producer that parses ITCH and drives `BookReplay`.

```
 $ itch_metrics_replay  file.NASDAQ_ITCH50  AAPL  [--pace=N] [--no-prefault] [--snap=N]

 SETUP (main thread)

   open + fstat -> mmap (MAP_POPULATE unless --no-prefault)
       prefault: touch every 4 KB page (whole file -> RAM; this is the startup wait)
       lazy:     pages fault in during the parse (live view appears at once)

   TscClock calibrate -> ns_per_tick
   EventRing · MetricsRecorder rec · OrderBook book · book.setMetrics(&rec)
   BookReplay rp(book,"AAPL") · Aggregator agg(ring, rec.dropsCounter(), ns_per_tick)
                 │
                 |
        std::thread consumer([]{ agg.run(); })  ------>  CORE B
                 |
                 │  (main thread = producer on CORE A)
                 |
 STEADY STATE

   itch::parseBuffer(data, size, lambda):                  CORE B  agg.run():
     lambda(msg):                                             try_pop (burst drain)
       --pace>0 ? pace_to(wall0 + (msg.ts−itch0)/pace)   ingest -> histograms · book · drops
       rp.onMessage(msg):                                render "top" @ ~5 Hz
         A/F->addOrder  D->cancel  X/E/C->modify|cancel  U->cancel+add
            └─ TARGET SYMBOL ONLY -> ScopedLatency -> ring.try_push -->  (drained by B)
       every --snap msgs -> recordSnapshot(bestBid, bestAsk)
                 │
                 |
 SHUTDOWN (end of file, or Ctrl-C -> g_stop)
   final recordSnapshot -> agg.stop() -> consumer.join() (final drain + last frame)
   summary: messages · adds/deletes/reduces/replaces · consumed / drops · final book
```

Two properties worth noting:

- **Pacing is outside the timed region.** `--pace` (0 = max, 1 = real-time, N = N×) inserts
  waits *between* operations via `pace_to`, so it never enters a `ScopedLatency` scope — the
  per-op latencies are identical regardless of replay speed; only the spacing changes.
- **Drops are rate-driven, not size-driven.** The single-symbol filter means the vast
  majority of even a multi-GB file produces *no* events (non-target messages return before
  touching the instrumented engine). Producing one event also costs more than draining it, so
  the consumer keeps up and the 64K ring absorbs bursts -> typically `drops = 0`. Forcing drops
  means outpacing the consumer: shrink the ring, pick a hot symbol at `--pace=0`, or share one
  core.

### 2E · Snapshot serialization and UDP transport

`render()` already computes a full per-cycle view to paint the console. That view is now captured
once as a `MetricsSnapshot` (header counters · book line · per-op ops/s · count · p50 / p99 / p99.9 ·
max, in ns) and fanned out: the console "top" view is one consumer, an optional **sink** is the other
— the seam every off-box transport attaches to.

```
  Aggregator.render()   (CORE B, ~5 Hz, off the hot path)
        |
        │ (builds ONE MetricsSnapshot per cycle)
        |
        ├──────────────> console "top" view          setConsole(true)   (default)
        └──────────────> sink(const MetricsSnapshot&) setSnapshotSink(...)
                              │
                 ┌────────────┴─────────────┐ 
                 |                          |                                           
        writeJson() -> NDJSON        UdpPublisher::send()
        json_snapshot_demo           writeJson() -> sendto() ONE datagram
        (one JSON line / cycle)      udp_publish_demo -> 127.0.0.1:9099
                                              │  fire-and-forget UDP datagram
                                              v
                                  dashboard/udp_ws_bridge.py  (asyncio + websockets)
                                  UDP :9099  ->  ws://127.0.0.1:8765
                                              │  one snapshot -> one WS text frame
                                              v
                                  dashboard/index.html (uPlot, browser)
```

Design notes:
- **One snapshot, many sinks.** The same struct feeds the console, the NDJSON logger, and the UDP
  publisher, so they can never disagree — all three read the identical numbers.
- **Everything off the hot path.** Snapshot build, JSON, and `sendto` all run on Core B at the render
  cadence; the producer never serializes and never touches a socket.
- **One snapshot == one UDP datagram.** `writeJson()` stays well under an MTU (~600-800 B with five
  op rows), so there is no framing to reassemble; a lost datagram just skips a dashboard frame.
- **The bridge is a stateless relay.** `dashboard/udp_ws_bridge.py` (asyncio + `websockets`) receives
  each datagram on UDP :9099 and `broadcast()`s it unchanged as a WebSocket text frame to every client
  on `ws://127.0.0.1:8765` — browsers cannot read UDP, so this is the only glue to the page; a slow
  client is simply dropped.
- **The dashboard is a thin consumer.** `dashboard/index.html` (uPlot from a CDN, no build step) opens
  the WebSocket, `JSON.parse`s each frame, skips the `final` flush frame, and plots per-op p50/p99/p99.9
  over time + throughput by op, with a live header and a current-snapshot table. It reads the same
  numbers the console "top" view does, so the two can never disagree.

Built: hot path · ring · aggregator · console view · snapshot JSON · UDP publisher ·
Python UDP->WebSocket bridge · uPlot browser dashboard. The full chain runs end to end:
  engine -> aggregator -> writeJson -> UDP :9099 -> udp_ws_bridge.py -> ws://:8765 -> browser.
`udp_publish_demo` keeps the console on while publishing, so the console and browser show the same
snapshot at once. Next: book-depth ladder + trade tape, alert rules inside the aggregator,
NDJSON logging + historical playback.

   Web layer (dashboard/):
     udp_ws_bridge.py   Python asyncio relay: UDP datagrams -> WebSocket text frames
     index.html         single-file uPlot dashboard (charts + header + table)

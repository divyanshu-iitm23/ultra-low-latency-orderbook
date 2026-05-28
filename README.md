# Ultra-Low-Latency Order Book

A high-performance limit order book implementation in C++ designed for low-latency trading systems.

## Features
- Sub-microsecond order operations (add, cancel, execute)
- FIFO price-time priority
- Zero heap allocations in hot path (coming in Week 3)
- Support for limit and market orders

## Build
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

## Run Tests
```bash
./test_orderbook
```

## Usage Example
```cpp
#include "orderbook.hpp"

orderbook::OrderBook book;

// Add orders
auto bid_id = book.addOrder(Side::BUY, dollarToPrice(100.50), 1000);
auto ask_id = book.addOrder(Side::SELL, dollarToPrice(100.51), 500);

// Execute market order
Quantity filled = book.executeMarketOrder(Side::BUY, 300);

// Cancel order
book.cancelOrder(bid_id);

// Print book
book.printBook();
```

## Performance (Week 3+)
Coming soon: benchmarks and optimization details.

## Architecture
See `docs/ARCHITECTURE.md` for design decisions.
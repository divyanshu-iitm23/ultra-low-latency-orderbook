# Architecture

## Design Decisions

### Price Representation
- Fixed-point integers (int64_t) to avoid floating-point errors
- Price in "ticks": $100.50 = 10050 (tick size $0.01)

### Data Structures
- **std::map** for price levels (ordered by price)
- **std::unordered_map** for order lookup by ID
- **Intrusive linked list** for orders at each price level (FIFO)

### Time Complexity
- Add order: O(log P) where P = number of price levels
- Cancel order: O(log P)
- Execute market: O(log P + N) where N = orders filled
- Get best bid/ask: O(1)

### Memory Layout
- Orders contain next/prev pointers (intrusive list)
- No separate node allocations for linked list
- Direct pointer-based navigation

## Future Optimizations (Week 3-4)
- Memory pool for orders (eliminate new/delete)
- Cache-line alignment
- Lock-free structures for concurrent access
- Array-based price levels for bounded price range
# Optimization 2: Direct-Mapped Price Ladder (replacing std::map)

## Result: addOrder, RDTSC harness, pinned core, median-of-7

| Metric        | std::map  | array+bitmap | Improvement |
|---------------|----------:|-------------:|------------:|
| batch mean    | 104.8 ns  |  26.6 ns     | 3.94x       |
| throughput    | 9.55 M/s  | 37.57 M/s    | 3.94x       |
| p99.9         | 150.7 ns  |  <fill in>   |             |
| max           | 665081 ns |  <fill in>   |             |

## Why
- O(log P) tree-walk → O(1) array index
- scattered tree nodes → one contiguous array (cache-friendly)
- per-level malloc → zero (one allocation up front)

## Remaining
- orders_ hash map still allocates per insert (next target)
- ladder reserves memory across the price range (single-instrument tradeoff)

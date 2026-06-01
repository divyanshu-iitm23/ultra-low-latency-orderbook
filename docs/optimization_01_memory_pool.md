# Optimization 1: Object Pool Allocator

## Result: addOrder latency (100k samples)

| Metric | Baseline (new/delete) | Pooled | Improvement |
|--------|----------------------:|-------:|------------:|
| Min    |    84 ns |  37 ns | 2.3x |
| p50    |   187 ns |  81 ns | 2.3x |
| p90    |   229 ns |  97 ns | 2.4x |
| p99    |   423 ns | 124 ns | 3.4x |
| p99.9  | 2,657 ns | 438 ns | 6.1x |
| Max    | 1,598,573 ns | 587,933 ns | 2.7x |

## Why it worked
- Eliminated malloc/free from the hot path (no kernel detours in steady state)
- Contiguous slab → Order objects share cache lines → fewer misses on list walks

## What remains (Max still ~0.6ms)
- std::map allocates a tree node per new price level (still hits malloc)
- First-touch page faults on fresh pool pages
- Next steps: pooled/array-based price levels + pre-faulting

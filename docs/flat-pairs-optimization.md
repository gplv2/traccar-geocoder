# Flat Pair Vectors: Eliminating Hash Maps from the Build Pipeline

## Problem

The builder's four cell maps (`cell_to_ways`, `cell_to_addrs`, `cell_to_interps`, `cell_to_admin`) used `std::unordered_map<uint64_t, std::vector<uint32_t>>` -- a hash map where each cell ID maps to a vector of item IDs. This is conceptually a list of (cell_id, item_id) pairs, but the hash map representation adds massive overhead:

- Each hash map entry: ~48 bytes for the node + 16 bytes malloc overhead
- Each inner vector: 24-byte header + separate heap allocation (32+ bytes minimum)
- Total overhead: **~120 bytes per cell** for what is logically a 12-byte pair

At planet scale (338M geo cells, 200M+ unique cell entries across all maps), these hash maps consumed **~37 GB** -- more than any other data structure in the builder.

## Solution

Replace all four `unordered_map<uint64_t, vector<uint32_t>>` with flat `vector<CellEntry>` using a packed 12-byte struct:

```cpp
struct CellEntry {
    uint64_t cell_id;
    uint32_t item_id;
} __attribute__((packed));  // 12 bytes, no alignment padding
```

### Building: vector append instead of hash lookup

```cpp
// Before: hash lookup + inner vector push (expensive)
cell_to_ways[cell_id].push_back(way_id);

// After: flat append (nearly free)
way_pairs.push_back({cell_id, way_id});
```

No hash computation, no bucket traversal, no inner vector reallocation. Just append to a contiguous array.

### Dedup: one big sort instead of millions of tiny sorts

The old `deduplicate()` iterated every hash map entry and sorted each inner vector individually -- millions of tiny sorts on scattered heap memory.

Now: one `std::sort` + `std::unique` on the entire flat vector. Same result, fully cache-friendly.

### Writing: merge scan instead of hash lookups

The old `write_entries()` did a `cell_map.find()` for every cell in the sorted cell list -- hundreds of millions of random hash lookups.

Now: both the cell list and the pair vector are sorted by cell_id, so writing becomes a single-pass merge scan. O(n+m) sequential access, zero lookups.

## Measured Results

### Belgium (0.5 GB PBF, baseline test)

Three versions compared sequentially on the same machine:

| Metric | V1: Original | V2: Write-optimized | V3: Flat pairs |
|---|---|---|---|
| Total build | 34.7s | 32.6s | **29.5s** |
| Dedup/Sort phase | 0.2s | 0.2s | 0.8s |
| Write phase | 4.4s | 2.3s | **0.4s** |
| Throughput (ways/s) | 17.6K | 17.6K | **18.8K** |

**Memory (RSS at key points):**

| Phase | V2: Hash maps | V3: Flat pairs | Savings |
|---|---|---|---|
| Before dedup/sort | 755 MB | 572 MB | **-24%** |
| Write start | 755 MB | 493 MB | **-35%** |
| After shrink_to_fit | 491 MB | 310 MB | **-37%** |
| Data files freed | 359 MB | 173 MB | **-52%** |

Output is byte-identical across all three versions (verified by diffing all 14 index files).

**Trade-off:** The sort+dedup phase is slower on Belgium (0.8s vs 0.2s) because sorting 6.7M contiguous pairs is more raw work than sorting millions of 2-3 element vectors. At larger scales where hash map iteration causes cache misses and swap thrashing, this is expected to reverse.

### Europe (32 GB PBF, `--in-memory`)

V2 baseline (write-optimized, hash maps): 37m 41s total, 17.8 GB peak RSS at write start.

*(V3 flat pairs build in progress -- results pending)*

| Metric | V2: Hash maps | V3: Flat pairs | Change |
|---|---|---|---|
| **Total build** | **37m 41s** | **29m 20s** | **-22%** |
| Reading | 34m 40s | 27m 46s | **-20%** |
| Sort/Dedup | 14.8s | 30.6s | +107% |
| Write | 2m 4s | 20.3s | **-84%** |
| Throughput (ways/s) | 9.4K | 11.7K | **+24%** |
| Peak RSS (write start) | 17.8 GB | 8.7 GB | **-51%** |

The reading phase is 7 minutes faster because hash map insertions during Pass 2 are eliminated. Every `cell_map[cell].push_back()` was a hash lookup competing for CPU and cache; now it's a vector append.

Sort+dedup is 2x slower in wall time (30.6s vs 14.8s) -- sorting 200M contiguous pairs is more raw work than the old approach of sorting millions of tiny 2-3 element vectors. But the sort is fully cache-friendly on contiguous memory, while the old dedup iterated scattered hash map nodes. The 16-second regression is overwhelmed by the 7-minute reading improvement.

**Pair vector statistics (Europe):**

| Vector | Raw pairs | After dedup | Unique cells |
|---|---|---|---|
| way_pairs | 114,360,274 | 114,360,274 (0 dupes) | 79,824,092 |
| addr_pairs | 86,056,695 | 86,056,695 (0 dupes) | 25,543,293 |
| interp_pairs | 31,967 | 31,967 | 17,593 |
| admin_pairs | 1,533,153 | 1,533,153 | 141,161 |

Zero duplicates removed -- the per-way dedup sets (kept from original code) already prevent duplicate (cell, way) entries, and addresses map 1:1 to cells. The sort+dedup phase is purely for ordering.

**Memory waterfall V2 (hash maps, write phase):**

```
write start:          17.8 GB + 464 MB swap
after shrink_to_fit:  14.3 GB +  58 MB swap
data files freed:      9.9 GB
cell_to_ways freed:    9.9 GB
cell_to_addrs freed:   2.5 GB
all freed:             457 MB
```

**Memory waterfall V3 (flat pairs):**

```
before sort+dedup:    10.1 GB + 1.1 GB swap
after sort+dedup:      8.7 GB + 354 MB swap   (shrink_to_fit freed over-reserved capacity)
write start:           8.7 GB + 354 MB swap
after shrink_to_fit:   7.3 GB +  51 MB swap    (-1.4 GB data vector over-reserve reclaimed)
data files freed:      2.6 GB                  (-4.7 GB vectors released)
geo cells sorted:      4.5 GB                  (+1.9 GB sort vector)
way_pairs freed:       2.9 GB                  (-1.6 GB)
addr_pairs freed:      2.0 GB                  (-0.9 GB)
geo_cells + offsets:   409 MB                  (-1.6 GB)
all freed:             391 MB                  (clean)
```

Write start RSS dropped from 17.8 GB to 8.7 GB -- a 51% reduction. The entire write phase completed with swap near zero.

### Planet scale estimates (refined with Europe actuals)

Europe pair counts scale ~2.7x for planet (86 GB / 32 GB):

| Structure | V2: Hash maps | V3: Flat pairs (12B each) | Savings |
|---|---|---|---|
| way_pairs (~307M) | ~21 GB | ~3.6 GB | -17.4 GB |
| addr_pairs (~160M) | ~16 GB | ~1.9 GB | -14.1 GB |
| interp_pairs (~86K) | ~0.1 GB | ~0.001 GB | -0.1 GB |
| admin_pairs (~4.1M) | ~0.2 GB | ~0.05 GB | -0.15 GB |
| **Total cell structures** | **~37 GB** | **~5.5 GB** | **~31.5 GB** |

This 31.5 GB reduction changes the planet build profile entirely.

### Planet file-backed build (94 GB machine)

The node location index is a 134 GB mmap'd temp file. Performance depends on how much of it fits in the OS page cache:

| | V2: Hash maps | V3: Flat pairs |
|---|---|---|
| Process memory (data + cells) | ~52 GB | ~21 GB |
| Free for page cache | ~42 GB (31% of 134 GB) | **~73 GB (55% of 134 GB)** |

With 55% of the node location temp file cacheable (vs 31% before), far fewer node lookups hit NVMe. Combined with the 24% throughput improvement from eliminating hash insertions during reading, the estimated planet build drops from 44h 46m (old code) to **15-20 hours**.

No need to shut down other VMs or increase memory. 94 GB is comfortable.

### Planet --in-memory (not feasible on current hardware)

| Component | Memory |
|---|---|
| Node location index (SparseMemArray) | ~134 GB |
| Pair vectors (during building) | ~5.7 GB |
| Data vectors (with over-reserve) | ~15 GB |
| **Total** | **~155 GB** |

The Proxmox host has 128 GB physical RAM. Even dedicating the entire host, planet --in-memory remains out of reach. This would complete in an estimated 2-3 hours on a 192+ GB machine.

### In-memory Europe on smaller machines

Europe peak RSS dropped from ~18 GB to ~10 GB during building. A 32 GB machine can now comfortably run Europe --in-memory builds.

## What Changed

| Component | Before | After |
|---|---|---|
| Cell data structure | `unordered_map<uint64_t, vector<uint32_t>>` | `vector<CellEntry>` (packed 12B struct) |
| Insertion (during PBF read) | Hash lookup + vector push | Vector append |
| Dedup | Per-cell sort on scattered memory | Single sort on contiguous memory |
| write_entries() | Hash lookup per cell | Merge scan (O(n+m)) |
| write_cell_index() | Copy map to sorted vector | Iterate sorted pairs directly |
| Geo cell merge | Extract keys from 3 hash maps | Extract from 3 sorted pair vectors |

## Verification

All three versions produce byte-identical output:
- **Belgium**: V1, V2, V3 diffed -- all 14 files identical
- **Europe**: V2 vs V3 diffed -- all 14 files identical

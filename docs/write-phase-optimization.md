# Write Phase Memory Optimization

## Problem

The builder's `write_index()` function was the bottleneck for planet-scale builds. On a 94 GB machine building from an 86 GB planet PBF, the write phase created massive memory pressure through three design issues:

1. **`std::set` for geo cell merge** -- A red-black tree with 338.7M nodes consuming ~22 GB, with every insert causing random pointer chases across swap
2. **Hash map offset tables** -- Three `unordered_map<uint64_t, uint32_t>` offset maps consuming ~17 GB, with 1 billion random hash lookups during geo_cells.bin write
3. **No progressive memory release** -- All data structures survived until the function returned, keeping ~48 GB alive simultaneously

The planet build took 44h 46m, with the write phase accounting for a disproportionate share due to page fault storms from random-access patterns on swapped-out data.

## Solution

Five changes to `write_index()` in `builder/src/build_index.cpp`:

### 1. Replace `std::set` with `vector + sort + unique`

The geo cell merge collected unique cell IDs from three hash maps into a `std::set<uint64_t>`. For planet scale (338.7M cells), the RB-tree consumed ~22 GB with terrible cache locality.

Replaced with a flat vector, sort, and unique pass. Same result, contiguous memory, cache-friendly sort.

**Memory saved: ~22 GB peak**

### 2. Replace offset hash maps with position-indexed vectors

`write_entries()` returned `unordered_map<uint64_t, uint32_t>` mapping cell IDs to byte offsets. Three such maps (street, addr, interp) consumed ~17 GB combined.

Since the cell IDs are in a known sorted order, offsets are now stored by position index in a `vector<uint32_t>`. The geo_cells.bin write loop becomes a fully sequential scan with zero hash lookups.

**Memory saved: ~13 GB**

### 3. Write data files first, free vectors immediately

Data vectors (street_nodes, addr_points, admin_vertices, etc.) totaling ~10.6 GB were idle during the entire cell index phase. Moved their writes to the top of `write_index()` with immediate deallocation after each write.

### 4. `shrink_to_fit()` before writing

Pre-allocation estimates based on per-GB PBF ratios can over-reserve significantly (addr_points reserved 10.3 GB for planet but only used 2.6 GB). Calling `shrink_to_fit()` on all vectors before writing reclaims this waste regardless of PBF source.

Per-GB estimation ratios were also updated: street_nodes raised from 4.5M to 6.5M/GB, admin_vertices from 500K to 5M/GB (based on planet actuals), addr_points lowered from 7.5M to 3M/GB.

### 5. `malloc_trim(0)` after each deallocation batch

Forces glibc to return freed pages to the OS immediately rather than keeping them in free lists.

## Measured Results

### Belgium (0.5 GB PBF, baseline test)

**Write phase timing:**

| Version | Write time | Total build |
|---|---|---|
| Before | 4.3s | 36.4s |
| After | 2.3s | 31.9s |

**46% faster write phase, 12% faster total build.** Output is byte-identical across all 14 index files.

**Memory waterfall (RSS during write phase):**

```
write start:           757 MB
after shrink_to_fit:   489 MB  (-268 MB over-reservation reclaimed)
data files freed:      356 MB  (-133 MB vectors released)
geo cells sorted:      395 MB  (+39 MB sort vector allocated)
cell_to_ways freed:    362 MB  (-33 MB)
cell_to_addrs freed:   132 MB  (-230 MB largest hash map gone)
geo_cells + offsets:    94 MB  (-38 MB)
all freed:              91 MB  (clean slate)
```

### Europe (32 GB PBF, 86.9M geo cells, `--in-memory`)

**Previous baseline (before all builder optimizations):** 3h 1m 17s total, ~74 GB peak RSS.

The previous Europe build predates multiple accumulated optimizations (read_meta::no, static coverer reuse, pre-allocation, and now write-phase memory management). The 3h baseline did not separately time the write phase.

| Metric | Before (all old code) | After (all optimizations) |
|---|---|---|
| Total build time | 3h 1m 17s | **37m 41s** |
| Write phase | not separately timed | **2m 4s** |
| Peak RSS | ~74 GB | **17.8 GB** (write start) |
| Output | 6.9 GB index | 7.0 GB index |

**Phase breakdown (optimized):**

| Phase | Time |
|---|---|
| Reading (2 passes) | 34m 40s |
| Resolve interpolation | 41.6s |
| Dedup | 14.8s |
| **Write** | **2m 4s** |
| Throughput | 9.4K/s ways, 41.4K/s addrs, 217/s polygons |

**Memory waterfall (RSS during write phase):**

```
write start:          17.8 GB + 464 MB swap
after shrink_to_fit:  14.3 GB +  58 MB swap  (-3.5 GB reclaimed, swap nearly eliminated)
data files freed:      9.9 GB                (-4.4 GB vectors released)
geo cells sorted:     11.3 GB                (+1.4 GB sort vector)
cell_to_ways freed:    9.9 GB                (-1.4 GB)
cell_to_addrs freed:   2.5 GB                (-7.4 GB largest hash map gone)
cell_to_interps freed: 2.4 GB                (-0.1 GB)
geo_cells + offsets:   898 MB                (-1.5 GB offset vectors + cell list)
all freed:             457 MB                (clean slate)
```

The write phase started with 464 MB in swap. After `shrink_to_fit()` and `malloc_trim(0)`, swap dropped to 58 MB and stayed near zero for the rest of the write. No swap thrashing during the cell index sort or entry writes -- the entire write phase fit in RAM.

The biggest single drop: freeing `cell_to_addrs` released 7.4 GB in one shot (2.5 GB RSS remaining from 9.9 GB). This hash map held ~50M address cell entries with all their inner vectors and node allocations.

### Planet (86 GB PBF, estimated impact)

Based on measured data structure sizes from the completed planet build:

| Structure | Current peak | Optimized peak |
|---|---|---|
| `std::set` geo cell merge | ~22 GB | ~2.8 GB (vector) |
| Offset hash maps (3x) | ~17 GB | ~4.1 GB (vectors) |
| Data vectors during cell index | ~10.6 GB | 0 (freed first) |
| Over-reserved addr_points | ~7.7 GB waste | 0 (shrink_to_fit) |
| **Total write phase peak** | **~70+ GB** | **~32 GB** |

The optimized write phase should fit entirely in RAM on the 94 GB machine with no swap needed. The previous planet build's write phase was heavily swap-bound, with page fault storms on every hash map access and tree traversal.

## Planet Build Index (measured file sizes)

| File | Size | Elements |
|---|---|---|
| street_ways.bin | 575 MB | 47,904,485 ways |
| street_nodes.bin | 4,052 MB | 506,463,499 nodes |
| addr_points.bin | 2,568 MB | 160,512,246 addresses |
| admin_polygons.bin | 23 MB | 944,746 polygons |
| admin_vertices.bin | 2,909 MB | 363,565,458 vertices |
| geo_cells.bin | 6,775 MB | 338,730,312 geo cells |
| admin_cells.bin | 25 MB | 2,056,395 admin cells |
| strings.bin | 215 MB | 214 MB of interned strings |
| street_entries.bin | 2,363 MB | variable-length entries |
| addr_entries.bin | 740 MB | variable-length entries |
| interp_entries.bin | 1.5 MB | variable-length entries |
| admin_entries.bin | 47 MB | variable-length entries |
| interp_ways.bin | 1.7 MB | 72,885 interpolations |
| interp_nodes.bin | 3.5 MB | 435,678 nodes |
| **Total** | **20.3 GB** | |

## Verification

All optimizations produce byte-identical output. Verified by building Belgium PBF with both old and new code and diffing all 14 output files.

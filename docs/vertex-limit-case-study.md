# Vertex Limit Case Study: 500 vs 10,000 vs 50,000

## Context

The builder's `simplify_polygon()` function uses Douglas-Peucker simplification to reduce admin boundary polygons to a maximum vertex count before computing S2 cell coverings. The upstream default of 500 vertices was found to be too aggressive, creating self-intersecting edges that S2 then fails to validate.

This case study compares three vertex limits (500, 10,000, and 50,000) across Italy and France to quantify the impact on polygon accuracy, S2 repair rates, index size, and build performance.

The `--max-vertices` flag (default 50,000, capped at 65,535 by the uint16_t vertex_count field) allows testing different limits without recompiling.

---

## Italy (2.1 GB PBF, 13,414 polygon rings)

### Build timing

All builds used `--in-memory` on the same machine (94 GB RAM, NVMe-cached PBF). Run sequentially to ensure fair comparison.

| Limit | Total | Reading | Dedup | Write | Throughput (ways) |
|---|---|---|---|---|---|
| 500 | **11m 24s** | 10m 52s | 2.6s | 27.5s | 2.8K/s |
| 10,000 | **11m 4s** | 10m 36s | 1.9s | 23.9s | 2.9K/s |
| 50,000 | **11m 0s** | 10m 32s | 1.9s | 23.4s | 2.9K/s |

Build time is essentially identical. The simplification and S2 repair overhead is negligible compared to PBF reading.

### Pipeline results

| Metric | 500 | 10,000 | 50,000 |
|---|---|---|---|
| Polygons stored | 13,414 | 13,414 | 13,414 |
| **S2 valid (direct)** | 13,315 | 13,404 | **13,414** |
| **S2 repaired** | **99** | **10** | **0** |
| Bbox fallback | 0 | 0 | 0 |
| Dropped | 0 | 0 | 0 |
| **Polygons simplified** | **4,512** | **70** | **4** |

At 50,000 vertices, every single polygon passes S2 validation on the first try. Zero repairs needed. Only 4 polygons in all of Italy exceed 50K vertices.

### Admin cell index

| Metric | 500 | 10,000 | 50,000 |
|---|---|---|---|
| Admin cells | **6,291,456** | 8,079 | 8,079 |
| **Total index size** | **582 MB** | **505 MB** | **510 MB** |

The 500-limit cell index explosion (6.3M cells vs 8K) is caused by jagged simplified polygon edges zig-zagging across S2 cell boundaries, producing far more boundary cells. At 10K and 50K the coverings are identical because the polygons are smooth enough to produce tight coverings.

The 5 MB difference between 10K (505 MB) and 50K (510 MB) is the slightly larger admin_vertices.bin from the 4 polygons that are stored at ~49K vertices instead of ~10K.

### Polygons simplified at each limit

**500 vertices (4,512 simplifications):**

Overwhelmingly level 8 (municipalities): 3,840 of 4,512 simplifications are at level 8. These are small Italian communes with 500-1,500 vertices being reduced to ~500. Even level 2 (the country boundary) was simplified.

| Admin level | Count | Examples |
|---|---|---|
| Level 2 (country) | 3 | Italia |
| Level 4 (region) | 64 | Piemonte, Sicilia, Toscana, Liguria |
| Level 6 (province) | 155 | Genova, Torino, Palermo, Siracusa |
| Level 7 (union) | 102 | Unione Delta del Po, Comunita montana della valle Camonica |
| Level 8 (municipality) | 3,840 | Campofiorito, Sanremo, Otranto, Favignana |
| Level 10 (district) | 171 | |
| Level 11 (postcode) | 177 | |

**10,000 vertices (70 simplifications):**

Only provinces and regions with complex coastlines or mountain boundaries. No municipalities affected.

| Admin level | Count | Largest polygon | Original vertices |
|---|---|---|---|
| Level 2 (country) | 1 | Italia | 60,487 -> 9,996 |
| Level 4 (region) | 15 | Liguria (64,567), Sicilia (55,778), Puglia (49,166) |
| Level 6 (province) | 44 | Bolzano-Bozen (30,567), Livorno (26,531), Genova (21,484) |
| Level 7 (union) | 2 | Comunita montana della valle Camonica (17,411) |
| Level 8 (municipality) | 1 | Genova (10,513) |
| Level 11 (postcode) | 1 | 39030 (13,216) |

**50,000 vertices (4 simplifications):**

Only the largest coastal/island regions exceed 50K:

| Polygon | Level | Original | Simplified |
|---|---|---|---|
| Liguria | 4 (region) | 64,567 | 49,253 |
| Sardigna/Sardegna | 4 (region) | 62,319 | 49,535 |
| Italia | 2 (country) | 60,487 | 48,768 |
| Sicilia | 4 (region) | 55,778 | 49,215 |

These 4 polygons are large island/coastal boundaries. At 49K+ vertices after simplification, they retain extremely high fidelity.

### Polygons repaired at each limit

**500 vertices (99 repairs):**

All repairs are municipalities (level 8) and provinces (level 6) that were over-simplified, creating self-intersecting edges. Sample:

| Polygon | Level | Vertices | Error |
|---|---|---|---|
| Calasetta | 8 (municipality) | 497 | Edge 191 crosses edge 194 |
| Favignana | 8 (municipality) | 496 | Edge 438 crosses edge 441 |
| Otranto | 8 (municipality) | 497 | Edge 2 crosses edge 493 |
| Sanremo | 8 (municipality) | 499 | Edge 214 crosses edge 216 |
| Latina | 6 (province) | 499 | Edge 160 crosses edge 162 |
| Ferrara | 6 (province) | 498 | Edge 233 crosses edge 235 |
| Ragusa | 6 (province) | 498 | Edge 111 crosses edge 113 |

These are coastal municipalities and provinces where the 500-vertex simplification compressed complex coastline geometry into self-intersecting zigzags.

**10,000 vertices (10 repairs):**

Only regions (level 4) and provinces (level 6) with very complex boundaries:

| Polygon | Level | Vertices | Error |
|---|---|---|---|
| Genova | 6 (province) | 9,974 | Edge 3558 crosses edge 3560 |
| Lazio | 4 (region) | 9,985 | Edge 1093 crosses edge 1095 |
| Siracusa | 6 (province) | 9,933 | Edge 4701 crosses edge 4704 |
| Sicilia | 4 (region) | 9,981 | Edge 3076 crosses edge 3078 |
| Gallura Nord-Est Sardegna | 6 (province) | 9,925 | Edge 4848 crosses edge 4850 |
| Friuli-Venezia Giulia | 4 (region) | 9,981 | Edge 3680 crosses edge 3683 |
| Puglia | 4 (region) | 9,985 | Edge 2534 crosses edge 2536 |
| Toscana | 4 (region) | 9,988 | Edge 2205 crosses edge 2207 |
| Liguria | 4 (region) | 9,947 | Edge 576 crosses edge 578 |
| Sardigna/Sardegna | 4 (region) | 9,982 | Edge 6033 crosses edge 6035 |

All are large coastal regions reduced from 20,000-64,000 vertices to ~10,000.

**50,000 vertices (0 repairs):**

No repairs needed. Every polygon passes S2 validation on the first try.

---

## France (4.7 GB PBF, 56,713 polygon rings)

### Build timing

| Limit | Total | Reading | Dedup | Write | Throughput (ways) |
|---|---|---|---|---|---|
| 500 | **25m 22s** | 24m 22s | 4.4s | 52.6s | 2.1K/s |
| 10,000 | **24m 47s** | 23m 51s | 3.5s | 49.6s | 2.1K/s |
| 50,000 | **24m 32s** | 23m 36s | 3.5s | 49.9s | 2.1K/s |

Build time nearly identical across all limits. The 500 limit is marginally slower due to the massive cell index sort during dedup and write.

### Pipeline results

| Metric | 500 | 10,000 | 50,000 |
|---|---|---|---|
| Polygons stored | 56,713 | 56,713 | 56,713 |
| **S2 valid (direct)** | 56,499 | 56,690 | **56,711** |
| **S2 repaired** | **214** | **23** | **2** |
| Bbox fallback | 0 | 0 | 0 |
| Dropped | 0 | 0 | 0 |
| **Polygons simplified** | **20,634** | **224** | **13** |
| Admin cells | **6,291,456** | 12,149 | 12,149 |
| **Total index size** | **1.1 GB** | **1.1 GB** | **1.1 GB** |

France has more complex admin geometry than Italy -- even at 50K, 2 polygons still need repair. The admin cell explosion at 500 (6.3M vs 12K) is the same pattern seen in Italy.

### Simplification by admin level

**500 vertices (20,634 simplifications):**

| Admin level | Count | Description |
|---|---|---|
| Level 2 (country) | 1 | France |
| Level 3 (metropolitan) | 2 | |
| Level 4 (region) | 59 | All major regions |
| Level 5 (department group) | 2 | |
| Level 6 (department) | 145 | Most departments |
| Level 7 (arrondissement) | 368 | |
| Level 8 (commune) | 13,729 | ~24% of all communes simplified |
| Level 9 (canton) | 1,735 | |
| Level 10 (district) | 70 | |
| Level 11 (postcode) | 4,523 | |

Over 20,000 polygons butchered -- nearly a third of all French boundaries.

**10,000 vertices (224 simplifications):**

Only departments (level 6), arrondissements (level 7), and regions (level 4):

| Admin level | Count | Examples |
|---|---|---|
| Level 3 | 1 | France metropolitaine |
| Level 4 (region) | 15 | Bretagne (160K!), Nouvelle-Aquitaine (103K), Corse (79K) |
| Level 5 | 2 | |
| Level 6 (department) | 85 | Finistere (78K), Morbihan (66K), Cotes-d'Armor (54K) |
| Level 7 (arrondissement) | 116 | Mostly Breton arrondissements |
| Level 8 (commune) | 2 | |
| Level 11 (postcode) | 3 | |

**50,000 vertices (13 simplifications):**

Only the largest coastal/island regions:

| Polygon | Level | Original | Simplified |
|---|---|---|---|
| Bretagne | 4 (region) | **160,150** | 49,949 |
| Nouvelle-Aquitaine | 4 (region) | **103,758** | 49,244 |
| Corse | 4 (region) | 79,376 | 49,211 |
| Finistere | 6 (department) | 78,323 | 49,042 |
| Provence-Alpes-Cote d'Azur | 4 (region) | 78,238 | 49,344 |
| Auvergne-Rhone-Alpes | 4 (region) | 71,275 | 49,956 |
| France metropolitaine | 3 | 69,461 | 49,627 |
| Pays de la Loire | 4 (region) | 69,141 | 49,077 |
| Morbihan | 6 (department) | 66,254 | 49,963 |
| Occitanie | 4 (region) | 65,546 | 49,413 |
| Cotes-d'Armor | 6 (department) | 54,238 | 49,449 |
| Normandie | 4 (region) | 53,300 | 49,283 |
| Corse-du-Sud | 6 (department) | 57,303 | 49,413 |

Bretagne has the most complex admin boundary in France: **160,150 vertices**. This is the jagged Breton coastline with hundreds of inlets, peninsulas, and islands.

### Polygons repaired at each limit

**500 vertices (214 repairs):**

Communes and postcodes throughout France. Sample:

| Polygon | Level | Vertices | Error |
|---|---|---|---|
| Chavanat | 8 (commune) | 498 | Edge 48 crosses edge 50 |
| Vidaillat | 8 (commune) | 496 | Edge 188 crosses edge 190 |
| Mazayes | 8 (commune) | 496 | Edge 167 crosses edge 169 |
| Trigueres | 8 (commune) | 497 | Edge 384 crosses edge 386 |
| Confolens | 8 (commune) | 498 | Edge 258 crosses edge 260 |
| Marquise | 8 (commune) | 499 | Edge 488 crosses edge 490 |
| Frehel | 8 (commune) | 491 | Edge 60 crosses edge 63 |
| Guimaec | 8 (commune) | 495 | Edge 212 crosses edge 214 |

**10,000 vertices (23 repairs):**

All major coastal regions and departments:

| Polygon | Level | Vertices | Original |
|---|---|---|---|
| Bretagne | 4 (region) | 9,976 | 160,150 |
| Hauts-de-France | 4 (region) | 9,968 | |
| Nouvelle-Aquitaine | 4 (region) | 9,982 | 103,758 |
| Provence-Alpes-Cote d'Azur | 4 (region) | 9,974 | 78,238 |
| Normandie | 4 (region) | 9,989 | 53,300 |
| Occitanie | 4 (region) | 9,991 | 65,546 |
| Pays de la Loire | 4 (region) | 9,961 | 69,141 |
| Corse | 4 (region) | 9,971 | 79,376 |
| Finistere | 6 (department) | 9,992 | 78,323 |
| Morbihan | 6 (department) | 9,988 | 66,254 |
| Cotes-d'Armor | 6 (department) | 9,941 | 54,238 |
| Gironde | 6 (department) | 9,970 | |
| Corse-du-Sud | 6 (department) | 9,956 | 57,303 |
| Loire-Atlantique | 6 (department) | 9,928 | |
| Nord | 6 (department) | 9,940 | |
| Var | 6 (department) | 9,999 | |
| Saint-Nazaire | 7 (arrondissement) | 9,831 | |
| Chateaulin | 7 (arrondissement) | 9,915 | |
| Dinan | 7 (arrondissement) | 9,973 | |
| Saint-Brieuc | 7 (arrondissement) | 9,956 | |
| Ajaccio | 7 (arrondissement) | 9,997 | |
| Quimper | 7 (arrondissement) | 9,940 | |
| Brest | 7 (arrondissement) | 9,930 | |

These are the exact regions that were missing from geocoding queries before our fix. Bretagne, Hauts-de-France, Nouvelle-Aquitaine, Provence-Alpes-Cote d'Azur -- all silently dropped by the upstream 500-limit builder.

**50,000 vertices (2 repairs):**

Only the two most complex regions in France:

| Polygon | Level | Vertices | Original | Error |
|---|---|---|---|---|
| Nouvelle-Aquitaine | 4 (region) | 49,243 | 103,758 | Edge 46949 crosses edge 46960 |
| Bretagne | 4 (region) | 49,948 | 160,150 | Edge 36125 crosses edge 36127 |

Bretagne's 160,150-vertex boundary (the jagged Breton coastline) still produces one self-intersection even at 50K simplification. At the full 65,535 uint16_t limit, it would still need simplification (160K > 65K). This polygon will always require S2Builder repair unless the vertex_count field is widened to uint32_t.

---

## Cross-Country Comparison

| Metric | Italy 500 | Italy 10K | Italy 50K | France 500 | France 10K | France 50K |
|---|---|---|---|---|---|---|
| Build time | 11m 24s | 11m 4s | 11m 0s | 25m 22s | 24m 47s | 24m 32s |
| Simplified | 4,512 | 70 | 4 | 20,634 | 224 | 13 |
| **Repaired** | **99** | **10** | **0** | **214** | **23** | **2** |
| Admin cells | 6.3M | 8K | 8K | 6.3M | 12K | 12K |
| Index size | 582 MB | 505 MB | 510 MB | 1.1 GB | 1.1 GB | 1.1 GB |

France has more complex geometry than Italy (Breton coastline, Corsica, Atlantic coast), resulting in more simplifications and repairs at every limit. Even at 50K, France's two most complex regions (Bretagne at 160K vertices, Nouvelle-Aquitaine at 103K) still need repair.

## Key Findings

1. **50,000 vertices eliminates all S2 repairs for Italy** and reduces France to just 2 (Bretagne and Nouvelle-Aquitaine). These two regions have the most complex admin boundaries in Western Europe.

2. **500 vertices causes massive damage**: 20,634 polygons simplified in France (over a third of all boundaries), 214 needing S2Builder repair, and a 500x admin cell index explosion.

3. **10,000 is good but not great**: 23 French regions/departments still need repair, including all the major coastal regions (Bretagne, Normandie, PACA, Corse). These are exactly the regions that were missing from queries before our fix.

4. **Build time is unaffected** by vertex limit choice. Italy builds in ~11 min and France in ~25 min regardless of limit.

5. **The admin cell explosion at 500 is dramatic**: 6.3M cells (both Italy and France) vs 8-12K at higher limits. This is a 500-778x increase caused by jagged simplified edges producing excessive boundary cells.

6. **Bretagne is the stress test**: 160,150 vertices for a single admin boundary. Even the uint16_t maximum of 65,535 cannot hold this polygon without simplification. It will always need S2Builder repair. A future improvement could widen vertex_count to uint32_t to eliminate this last edge case.

7. **The S2Builder repair pipeline is essential**: Even at 50K, 2 polygons in France need repair. Without the pipeline, these regions would be silently invisible to queries.

## Recommendation

Use `--max-vertices 50000` (the default). It eliminates virtually all S2 repairs (zero for Italy, 2 for France) while having no measurable impact on build time or index size. The S2Builder repair pipeline handles the remaining edge cases.

For bandwidth-constrained deployments, `--max-vertices 10000` is acceptable -- the repair pipeline catches the ~23 French coastal regions that still self-intersect.

The upstream default of 500 should never be used. It destroys polygon topology across thousands of boundaries, creates an index 500-778x larger in the admin cell layer, and requires hundreds of S2Builder repairs per country.

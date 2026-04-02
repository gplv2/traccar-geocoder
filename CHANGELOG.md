# Changelog

All notable changes to this project will be documented in this file.

This is a fork of [traccar/traccar-geocoder](https://github.com/traccar/traccar-geocoder).
Changes below are relative to the upstream project.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

## [1.1.0] - 2026-04-02

### Added

#### Builder
- S2Builder polygon repair pipeline: three-tier recovery (S2Loop direct > S2Builder split_crossing_edges > bounding-box fallback) instead of silently dropping invalid polygons
- `--verbose` flag for per-polygon diagnostic output and pipeline summary counters
- `--debug` flag for osmium assembler problem reporting
- `--in-memory` flag for RAM-backed node location index (eliminates node_locations.tmp disk I/O)
- `--tmpdir DIR` flag to place temp file on a different filesystem
- Vector pre-allocation from PBF file size heuristics
- Reuse of S2RegionCoverer instances (was constructing millions per build)
- Skip PBF metadata parsing (`read_meta::no`) in both reader passes

#### Server
- Country-boundaries fallback for missing country data from continent/regional PBF extracts (France, Spain, Netherlands affected)
- `--no-country-fallback` flag to disable fallback for planet PBF builds
- 40-test suite covering format_address, auth, rate limiter, coordinate validation, country fallback
- Pre-push git hook running clippy, fmt, and audit on every push

#### Documentation
- `docs/builder-polygon-repair-findings.md`: full investigation report with root cause analysis, benchmark data, and geocoding verification across Italy, France, and Benelux
- `NOTICE` file for Apache 2.0 license compliance

### Changed

#### Builder
- Raised polygon simplification limit from 500 to 10,000 vertices (eliminates self-intersecting edges, often produces smaller index due to smoother S2 coverings)
- Enabled lenient multipolygon assembly (`ignore_invalid_locations`) on libosmium assembler
- Enabled `ignore_errors()` on node location handler for PBF extract tolerance
- Highway filter uses `unordered_set` for O(1) lookup instead of O(9) linear scan

#### Server
- Rewrote `format_address` to use single String buffer (fewer allocations)
- Precomputed mmap slice lengths at load time instead of per-query division
- Eliminated redundant Vec allocation in cell neighbor lookup
- Rate limiter: 0 means unlimited (convention for admin accounts), remove token to revoke access
- Applied rustfmt and clippy across all source files

### Fixed

#### Server
- Rate limiter race condition: replaced atomics with Mutex (#c846b2a)
- Street deduplication hash collisions in dense urban areas (#3d21563)
- Added bounds checking on all mmap slice accesses to prevent panics (#2faf0dd)
- Validate lat/lon parameters before querying index (reject NaN, infinity, out-of-bounds) (#c867333)
- `Db` deserialization crash on empty JSON `{}` (added `#[serde(default)]`)

#### Deployment
- Use positional parameters in entrypoint.sh for safe argument passing (#699b5ef)

### Security
- Updated aws-lc-sys 0.38.0 to 0.39.1 (RUSTSEC-2026-0044, RUSTSEC-2026-0048: X.509 name constraints bypass, CRL scope check, severity 7.4 high)
- Updated rustls-webpki 0.103.9 to 0.103.10 (RUSTSEC-2026-0049: CRL distribution point matching)

## [1.0.0] - 2026-04-01

Initial fork from [traccar/traccar-geocoder](https://github.com/traccar/traccar-geocoder) at commit `6feb14e`.

### Added
- Country-boundaries fallback for missing country data (later refined in 1.1.0)
- Worldwide country code to name mapping (~250 entries)

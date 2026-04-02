# Traccar Geocoder

A fast, self-hosted reverse geocoding service built from OpenStreetMap data. Given latitude and longitude coordinates, it returns the nearest street address including house number, street name, city, state, county, postcode, and country.

This is a fork of [traccar/traccar-geocoder](https://github.com/traccar/traccar-geocoder) with polygon repair, performance optimizations, security fixes, and a country-boundaries fallback for continent-level PBF extracts. See [CHANGELOG.md](CHANGELOG.md) for details.

## Features

- Street-level reverse geocoding from OSM data
- Address point, street name, and address interpolation lookup
- Administrative boundary resolution (country, state, county, city, postcode)
- S2Builder polygon repair pipeline -- never silently drops admin boundaries
- Country-boundaries fallback for continent/regional PBF extracts (France, Spain, Netherlands)
- Sub-millisecond query latency with memory-mapped index files
- In-memory node location index for fast builds on high-RAM machines
- Automatic HTTPS with Let's Encrypt
- Token-based authentication with per-user rate limiting
- Docker support with automatic PBF download and indexing

## Quick Start

### Docker Compose

```yaml
services:
  geocoder:
    image: traccar/traccar-geocoder
    environment:
      - PBF_URLS=https://download.geofabrik.de/europe/monaco-latest.osm.pbf
    ports:
      - "3000:3000"
    volumes:
      - geocoder-data:/data

volumes:
  geocoder-data:
```

```bash
docker compose up
```

### Docker

```bash
# All-in-one: download, build index, and serve
docker run -e PBF_URLS="https://download.geofabrik.de/europe-latest.osm.pbf" \
  -v geocoder-data:/data -p 3000:3000 traccar/traccar-geocoder

# Build index only
docker run -e PBF_URLS="https://download.geofabrik.de/europe-latest.osm.pbf" \
  -v geocoder-data:/data traccar/traccar-geocoder build

# Serve only (from pre-built index)
docker run -v geocoder-data:/data -p 3000:3000 traccar/traccar-geocoder serve

# Multiple PBF files
docker run -e PBF_URLS="https://download.geofabrik.de/europe/france-latest.osm.pbf https://download.geofabrik.de/europe/germany-latest.osm.pbf" \
  -v geocoder-data:/data -p 3000:3000 traccar/traccar-geocoder

# With automatic HTTPS
docker run -e PBF_URLS="https://planet.openstreetmap.org/pbf/planet-latest.osm.pbf" \
  -e DOMAIN=geocoder.example.com \
  -v geocoder-data:/data -p 443:443 traccar/traccar-geocoder
```

PBF files can be downloaded from [Geofabrik](https://download.geofabrik.de/).

### Disk and Memory Requirements

| Extract | PBF Size | Index Size | Temp file | RAM (file-backed) | RAM (--in-memory) |
|---------|----------|------------|-----------|--------------------|--------------------|
| Monaco | 2 MB | ~10 MB | ~50 MB | < 1 GB | < 1 GB |
| Belgium | 765 MB | ~200 MB | ~4 GB | 2 GB | 4 GB |
| France | 4.7 GB | ~1 GB | ~20 GB | 4 GB | 20 GB |
| Italy | 2.1 GB | ~500 MB | ~10 GB | 4 GB | 12 GB |
| Europe | 32 GB | ~7 GB | ~55 GB | 8 GB + swap | 74 GB (measured) |
| Planet | ~70 GB | ~18 GB | ~134 GB | 16 GB + swap | ~174 GB (estimated) |

RAM estimates for `--in-memory` are based on ~16 bytes per node for the SparseMemArray index. Europe has 3.7 billion nodes (55 GB index + 19 GB other structures). Planet has ~9 billion nodes (134 GB index + ~40 GB other structures).

The "Temp file" column shows the `node_locations.tmp` size when using the default file-backed mode. For large builds on machines with limited RAM, use `--tmpdir` to place this file on fast local storage (NVMe) rather than the output directory.

For high performance, use NVMe storage or a machine with enough RAM for `--in-memory` mode.

## API

### GET /reverse

Query parameters:
- `lat` - latitude (required, -90 to 90)
- `lon` - longitude (required, -180 to 180)
- `key` - API key (required)

Example request:

```
GET /reverse?lat=43.7384&lon=7.4246&key=YOUR_API_KEY
```

Response follows [Nominatim](https://nominatim.org/release-docs/latest/api/Reverse/) format:

```json
{
  "display_name": "Avenue de la Costa 42, 98000 Monaco, Monaco",
  "address": {
    "house_number": "42",
    "road": "Avenue de la Costa",
    "city": "Monaco",
    "state": "Monaco",
    "county": "Monaco",
    "postcode": "98000",
    "country": "Monaco",
    "country_code": "MC"
  }
}
```

Fields are omitted when not available. The `display_name` field is formatted according to the country's addressing convention (e.g., number after street in Europe, before street in the US).

Status codes:
- `200` - success (application/json)
- `400` - invalid coordinates (NaN, infinity, or out of range)
- `401` - missing or invalid API key
- `429` - rate limit exceeded

### Authentication

The server includes a web dashboard for managing API keys. On first launch, navigate to the server URL in a browser to create an admin account. Once logged in, you can generate API keys and create additional users with configurable rate limits.

Rate limits are per-user. Setting `rate_per_second` or `rate_per_day` to `0` means unlimited (no rate limiting applied). To revoke access, remove the user's token from the auth database.

## Architecture

The project consists of two components:

- **Builder** (C++) - Parses OSM PBF files and creates a compact binary index using S2 geometry cells for spatial lookup. Includes a three-tier polygon repair pipeline (S2Loop > S2Builder > bounding-box fallback) that recovers invalid admin boundaries instead of silently dropping them.
- **Server** (Rust) - Memory-maps the index files and serves queries via HTTP/HTTPS with sub-millisecond latency. Includes an embedded country-boundaries fallback for when continent extracts have incomplete national boundaries.

### Index Structure

The builder produces 14 binary files:

| File | Description |
|------|-------------|
| `geo_cells.bin` | Merged S2 cell index for streets, addresses, and interpolations |
| `street_entries.bin` | Street way IDs per cell |
| `street_ways.bin` | Street way headers (node offset, name) |
| `street_nodes.bin` | Street node coordinates |
| `addr_entries.bin` | Address point IDs per cell |
| `addr_points.bin` | Address point data (coordinates, house number, street) |
| `interp_entries.bin` | Interpolation way IDs per cell |
| `interp_ways.bin` | Interpolation way headers |
| `interp_nodes.bin` | Interpolation node coordinates |
| `admin_cells.bin` | S2 cell index for admin boundaries |
| `admin_entries.bin` | Admin polygon IDs per cell (high bit marks interior cells) |
| `admin_polygons.bin` | Admin polygon metadata (name, level, area, country code) |
| `admin_vertices.bin` | Admin polygon vertices for point-in-polygon tests |
| `strings.bin` | Deduplicated string pool |

## Building from Source

### Prerequisites

**Builder (C++):**
- CMake 3.16+
- C++17 compiler
- libosmium2-dev, libprotozero-dev, libs2-dev, zlib1g-dev, libbz2-dev, libexpat1-dev, liblz4-dev

**Server (Rust):**
- Rust toolchain (stable)

Tested on aarch64 (Apple Silicon, ARM64) and x86_64 (AMD64) platforms. Index files are portable between both architectures (little-endian, IEEE 754 floats, no padding differences in the binary structs).

### Build

```bash
# Build the indexer
cd builder && mkdir -p build && cd build && cmake .. && make -j$(nproc)

# Build the server
cd server && cargo build --release
```

### Builder Usage

```
build-index <output-dir> <input.osm.pbf> [input2.osm.pbf ...] [options]
```

| Option | Description |
|--------|-------------|
| `--street-level N` | S2 cell level for streets (default: 17) |
| `--admin-level N` | S2 cell level for admin boundaries (default: 10) |
| `--verbose` | Per-polygon diagnostic output and pipeline summary counters |
| `--debug` | Enable osmium assembler problem reporting (very verbose) |
| `--in-memory` | Use RAM-backed node location index instead of disk-backed temp file. Eliminates `node_locations.tmp` I/O. Needs ~16 bytes per node (~40 GB for Europe, ~80 GB for planet). |
| `--tmpdir DIR` | Place `node_locations.tmp` on a different filesystem. Useful when the output directory is on slow storage but fast local storage is available. Ignored when `--in-memory` is used. |

Examples:

```bash
# Basic build
./build-index /data/index europe-latest.osm.pbf

# Multiple PBFs with diagnostics
./build-index /data/index france.osm.pbf germany.osm.pbf --verbose

# Fast build on high-RAM machine (Europe, ~40 GB RAM needed)
./build-index /data/index europe-latest.osm.pbf --in-memory --verbose

# Cloud VM: output to block storage, temp file on local NVMe
./build-index /mnt/block/index europe-latest.osm.pbf --tmpdir /mnt/nvme/tmp
```

### Server Usage

```
query-server <index-dir> [bind-address] [options]
```

| Option | Description |
|--------|-------------|
| `--street-level N` | S2 cell level for streets (default: 17, must match builder) |
| `--admin-level N` | S2 cell level for admin boundaries (default: 10, must match builder) |
| `--search-distance N` | Max search distance in degrees (default: 0.002) |
| `--no-country-fallback` | Disable the embedded country boundary fallback. Use when building from planet PBF where all boundary relations are complete. |
| `--domain DOMAIN` | Enable automatic HTTPS via Let's Encrypt ACME |
| `--cache DIR` | ACME certificate cache directory (default: acme-cache) |

Examples:

```bash
# Basic HTTP server
./query-server /data/index 0.0.0.0:3000

# With HTTPS
./query-server /data/index --domain geocoder.example.com

# Planet PBF build (all boundaries complete, no fallback needed)
./query-server /data/index 0.0.0.0:3000 --no-country-fallback
```

## Environment Variables (Docker)

| Variable | Description | Default |
|----------|-------------|---------|
| `PBF_URLS` | Space-separated list of PBF download URLs | (required for auto/build) |
| `DOMAIN` | Domain name for automatic HTTPS via Let's Encrypt | (disabled) |
| `BIND_ADDR` | HTTP bind address | `0.0.0.0:3000` |
| `DATA_DIR` | Data directory for PBF files and index | `/data` |
| `CACHE_DIR` | ACME certificate cache directory | `acme-cache` |
| `STREET_LEVEL` | S2 cell level for streets | `17` |
| `ADMIN_LEVEL` | S2 cell level for admin boundaries | `10` |
| `SEARCH_DISTANCE` | Max search distance in degrees | `0.002` |

## Known Limitations

When building from continent or regional PBF extracts (e.g., `europe-latest.osm.pbf`), countries with overseas territories outside the extract boundary will have incomplete admin_level=2 boundary relations. The libosmium assembler cannot form closed polygon rings when member ways are missing, so these countries will have no `country` or `country_code` in query results.

The server's country-boundaries fallback compensates for this at query time using an embedded global boundary dataset (~1 MB). This is enabled by default and covers all affected countries (France, Spain, Netherlands, and others with overseas territories).

See [docs/builder-polygon-repair-findings.md](docs/builder-polygon-repair-findings.md) for a detailed analysis.

## License

    Apache License, Version 2.0

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

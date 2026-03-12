use axum::{extract::Query, routing::get, Router};
use memmap2::Mmap;
use s2::cellid::CellID;
use s2::latlng::LatLng;
use serde::{Deserialize, Serialize};
use std::borrow::Cow;
use std::fs::File;
use std::sync::Arc;

// --- S2 helpers ---

const STREET_CELL_LEVEL: u64 = 17;
const ADMIN_CELL_LEVEL: u64 = 10;

fn cell_id_at_level(lat: f64, lng: f64, level: u64) -> u64 {
    let ll = LatLng::from_degrees(lat, lng);
    CellID::from(ll).parent(level).0
}

fn cell_neighbors_at_level(cell_id: u64, level: u64) -> Vec<u64> {
    let cell = CellID(cell_id);
    cell.all_neighbors(level).into_iter().map(|c| c.0).collect()
}

// --- Binary format structs (must match C++ build pipeline) ---

#[repr(C)]
#[derive(Clone, Copy)]
struct WayHeader {
    node_offset: u32,
    node_count: u8,
    name_id: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct AddrPoint {
    lat: f32,
    lng: f32,
    housenumber_id: u32,
    street_id: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct InterpWay {
    node_offset: u32,
    node_count: u8,
    street_id: u32,
    start_number: u32,
    end_number: u32,
    interpolation: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct AdminPolygon {
    vertex_offset: u32,
    vertex_count: u16,
    name_id: u32,
    admin_level: u8,
    area: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct NodeCoord {
    lat: f32,
    lng: f32,
}

// --- Index data ---

struct Index {
    geo_cells: Mmap,
    street_entries: Mmap,
    street_ways: Mmap,
    street_nodes: Mmap,
    addr_entries: Mmap,
    addr_points: Mmap,
    interp_entries: Mmap,
    interp_ways: Mmap,
    interp_nodes: Mmap,
    admin_cells: Mmap,
    admin_entries: Mmap,
    admin_polygons: Mmap,
    admin_vertices: Mmap,
    strings: Mmap,
}

const NO_DATA: u32 = 0xFFFFFFFF;

struct GeoCellOffsets {
    street: u32,
    addr: u32,
    interp: u32,
}

fn mmap_file(path: &str) -> Mmap {
    let file = File::open(path).unwrap_or_else(|e| panic!("Failed to open {}: {}", path, e));
    unsafe { Mmap::map(&file).unwrap_or_else(|e| panic!("Failed to mmap {}: {}", path, e)) }
}

impl Index {
    fn load(dir: &str) -> Self {
        Index {
            geo_cells: mmap_file(&format!("{}/geo_cells.bin", dir)),
            street_entries: mmap_file(&format!("{}/street_entries.bin", dir)),
            street_ways: mmap_file(&format!("{}/street_ways.bin", dir)),
            street_nodes: mmap_file(&format!("{}/street_nodes.bin", dir)),
            addr_entries: mmap_file(&format!("{}/addr_entries.bin", dir)),
            addr_points: mmap_file(&format!("{}/addr_points.bin", dir)),
            interp_entries: mmap_file(&format!("{}/interp_entries.bin", dir)),
            interp_ways: mmap_file(&format!("{}/interp_ways.bin", dir)),
            interp_nodes: mmap_file(&format!("{}/interp_nodes.bin", dir)),
            admin_cells: mmap_file(&format!("{}/admin_cells.bin", dir)),
            admin_entries: mmap_file(&format!("{}/admin_entries.bin", dir)),
            admin_polygons: mmap_file(&format!("{}/admin_polygons.bin", dir)),
            admin_vertices: mmap_file(&format!("{}/admin_vertices.bin", dir)),
            strings: mmap_file(&format!("{}/strings.bin", dir)),
        }
    }

    fn get_string(&self, offset: u32) -> &str {
        let bytes = &self.strings[offset as usize..];
        let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
        std::str::from_utf8(&bytes[..end]).unwrap_or("")
    }

    fn read_u16(data: &[u8], offset: usize) -> u16 {
        u16::from_le_bytes([data[offset], data[offset + 1]])
    }

    fn read_u32(data: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
    }

    fn read_u64(data: &[u8], offset: usize) -> u64 {
        u64::from_le_bytes(data[offset..offset + 8].try_into().unwrap())
    }

    // Iterate entry IDs inline from entries file at given offset
    fn for_each_entry(entries: &[u8], offset: u32, mut f: impl FnMut(u32)) {
        if offset == NO_DATA { return; }
        let offset = offset as usize;
        if offset + 2 > entries.len() { return; }

        let id_count = Self::read_u16(entries, offset) as usize;
        let data_start = offset + 2;
        if data_start + id_count * 4 > entries.len() { return; }

        for i in 0..id_count {
            f(Self::read_u32(entries, data_start + i * 4));
        }
    }

    // Binary search geo cell index: 20 bytes per entry (u64 cell_id + u32 street + u32 addr + u32 interp)
    fn lookup_geo_cell(cells: &[u8], cell_id: u64) -> GeoCellOffsets {
        let entry_size: usize = 20;
        let count = cells.len() / entry_size;
        let empty = GeoCellOffsets { street: NO_DATA, addr: NO_DATA, interp: NO_DATA };
        if count == 0 { return empty; }

        let mut lo = 0usize;
        let mut hi = count;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let mid_id = Self::read_u64(cells, mid * entry_size);
            if mid_id == cell_id {
                return GeoCellOffsets {
                    street: Self::read_u32(cells, mid * entry_size + 8),
                    addr: Self::read_u32(cells, mid * entry_size + 12),
                    interp: Self::read_u32(cells, mid * entry_size + 16),
                };
            } else if mid_id < cell_id {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        empty
    }

    // Binary search admin cell index: 12 bytes per entry (u64 cell_id + u32 offset)
    fn lookup_admin_cell(cells: &[u8], cell_id: u64) -> u32 {
        let entry_size: usize = 12;
        let count = cells.len() / entry_size;
        if count == 0 { return NO_DATA; }

        let mut lo = 0usize;
        let mut hi = count;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let mid_id = Self::read_u64(cells, mid * entry_size);
            if mid_id == cell_id {
                return Self::read_u32(cells, mid * entry_size + 8);
            } else if mid_id < cell_id {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        NO_DATA
    }

    // --- Geo lookup (streets, addresses, interpolation from merged index) ---

    fn query_geo(&self, lat: f64, lng: f64) -> (Option<(f64, &AddrPoint)>, Option<(f64, &str, u32)>, Option<(f64, &WayHeader)>) {
        let cell = cell_id_at_level(lat, lng, STREET_CELL_LEVEL);
        let neighbors = cell_neighbors_at_level(cell, STREET_CELL_LEVEL);

        let all_points: &[AddrPoint] = unsafe {
            std::slice::from_raw_parts(
                self.addr_points.as_ptr() as *const AddrPoint,
                self.addr_points.len() / std::mem::size_of::<AddrPoint>(),
            )
        };
        let all_ways: &[WayHeader] = unsafe {
            std::slice::from_raw_parts(
                self.street_ways.as_ptr() as *const WayHeader,
                self.street_ways.len() / std::mem::size_of::<WayHeader>(),
            )
        };
        let all_street_nodes: &[NodeCoord] = unsafe {
            std::slice::from_raw_parts(
                self.street_nodes.as_ptr() as *const NodeCoord,
                self.street_nodes.len() / std::mem::size_of::<NodeCoord>(),
            )
        };
        let all_interps: &[InterpWay] = unsafe {
            std::slice::from_raw_parts(
                self.interp_ways.as_ptr() as *const InterpWay,
                self.interp_ways.len() / std::mem::size_of::<InterpWay>(),
            )
        };
        let all_interp_nodes: &[NodeCoord] = unsafe {
            std::slice::from_raw_parts(
                self.interp_nodes.as_ptr() as *const NodeCoord,
                self.interp_nodes.len() / std::mem::size_of::<NodeCoord>(),
            )
        };

        let cos_lat = lat.to_radians().cos();

        let mut best_addr_dist = f64::MAX;
        let mut best_addr: Option<&AddrPoint> = None;
        let mut best_street_dist = f64::MAX;
        let mut best_street: Option<&WayHeader> = None;
        let mut best_interp_dist = f64::MAX;
        let mut best_interp: Option<&InterpWay> = None;
        let mut best_interp_t: f64 = 0.0;

        for c in std::iter::once(cell).chain(neighbors.into_iter()) {
            let offsets = Self::lookup_geo_cell(&self.geo_cells, c);

            // Addresses
            Self::for_each_entry(&self.addr_entries, offsets.addr, |id| {
                let point = &all_points[id as usize];
                let dlat = (point.lat as f64 - lat).to_radians();
                let dlng = (point.lng as f64 - lng).to_radians();
                let dist = dist_sq(dlat, dlng, cos_lat);
                if dist < best_addr_dist {
                    best_addr_dist = dist;
                    best_addr = Some(point);
                }
            });

            // Streets
            Self::for_each_entry(&self.street_entries, offsets.street, |id| {
                let way = &all_ways[id as usize];
                let offset = way.node_offset as usize;
                let count = way.node_count as usize;
                let nodes = &all_street_nodes[offset..offset + count];

                for i in 0..nodes.len() - 1 {
                    let dist = point_to_segment_distance(
                        lat, lng,
                        nodes[i].lat as f64, nodes[i].lng as f64,
                        nodes[i + 1].lat as f64, nodes[i + 1].lng as f64,
                        cos_lat,
                    );
                    if dist < best_street_dist {
                        best_street_dist = dist;
                        best_street = Some(way);
                    }
                }
            });

            // Interpolation
            Self::for_each_entry(&self.interp_entries, offsets.interp, |id| {
                let iw = &all_interps[id as usize];
                if iw.start_number == 0 || iw.end_number == 0 { return; }

                let offset = iw.node_offset as usize;
                let count = iw.node_count as usize;
                let nodes = &all_interp_nodes[offset..offset + count];

                let mut total_len: f64 = 0.0;
                for i in 0..nodes.len() - 1 {
                    let dlat = (nodes[i + 1].lat as f64 - nodes[i].lat as f64).to_radians();
                    let dlng = (nodes[i + 1].lng as f64 - nodes[i].lng as f64).to_radians();
                    total_len += dist_sq(dlat, dlng, cos_lat);
                }
                if total_len == 0.0 { return; }

                let mut best_seg_dist = f64::MAX;
                let mut best_seg_t: f64 = 0.0;
                let mut prev_accumulated: f64 = 0.0;

                for i in 0..nodes.len() - 1 {
                    let dlat = (nodes[i + 1].lat as f64 - nodes[i].lat as f64).to_radians();
                    let dlng = (nodes[i + 1].lng as f64 - nodes[i].lng as f64).to_radians();
                    let seg_len = dist_sq(dlat, dlng, cos_lat);
                    let (dist, seg_t) = point_to_segment_with_t(
                        lat, lng,
                        nodes[i].lat as f64, nodes[i].lng as f64,
                        nodes[i + 1].lat as f64, nodes[i + 1].lng as f64,
                        cos_lat,
                    );
                    if dist < best_seg_dist {
                        best_seg_dist = dist;
                        best_seg_t = (prev_accumulated + seg_t * seg_len) / total_len;
                    }
                    prev_accumulated += seg_len;
                }

                if best_seg_dist < best_interp_dist {
                    best_interp_dist = best_seg_dist;
                    best_interp = Some(iw);
                    best_interp_t = best_seg_t;
                }
            });
        }

        let addr_result = best_addr.map(|p| (best_addr_dist, p));
        let street_result = best_street.map(|w| (best_street_dist, w));
        let interp_result = best_interp.map(|iw| {
            let start = iw.start_number as f64;
            let end = iw.end_number as f64;
            let raw = start + best_interp_t * (end - start);

            let step: u32 = match iw.interpolation {
                1 | 2 => 2,
                _ => 1,
            };

            let number = if step == 2 {
                let base = iw.start_number;
                let offset = ((raw - base as f64) / step as f64).round() as u32 * step;
                base + offset
            } else {
                raw.round() as u32
            };

            (best_interp_dist, self.get_string(iw.street_id), number)
        });

        (addr_result, interp_result, street_result)
    }

    // --- Admin boundary lookup (point-in-polygon) ---

    fn find_admin(&self, lat: f64, lng: f64) -> AdminResult<'_> {
        let cell = cell_id_at_level(lat, lng, ADMIN_CELL_LEVEL);
        let neighbors = cell_neighbors_at_level(cell, ADMIN_CELL_LEVEL);

        let all_polygons: &[AdminPolygon] = unsafe {
            std::slice::from_raw_parts(
                self.admin_polygons.as_ptr() as *const AdminPolygon,
                self.admin_polygons.len() / std::mem::size_of::<AdminPolygon>(),
            )
        };
        let all_vertices: &[NodeCoord] = unsafe {
            std::slice::from_raw_parts(
                self.admin_vertices.as_ptr() as *const NodeCoord,
                self.admin_vertices.len() / std::mem::size_of::<NodeCoord>(),
            )
        };

        // For each admin level, find the smallest-area polygon containing the point
        let mut best_by_level: [Option<(f32, &AdminPolygon)>; 12] = [None; 12];

        const INTERIOR_FLAG: u32 = 0x80000000;
        const ID_MASK: u32 = 0x7FFFFFFF;

        for c in std::iter::once(cell).chain(neighbors.into_iter()) {
            Self::for_each_entry(&self.admin_entries, Self::lookup_admin_cell(&self.admin_cells, c), |id| {
                let is_interior = (id & INTERIOR_FLAG) != 0;
                let poly_id = (id & ID_MASK) as usize;
                let poly = &all_polygons[poly_id];
                let level = poly.admin_level as usize;
                if level >= 12 { return; }

                // Skip if we already have a smaller polygon at this level
                if let Some((best_area, _)) = best_by_level[level] {
                    if poly.area >= best_area { return; }
                }

                // Interior cells skip point-in-polygon test
                if is_interior || point_in_polygon(lat as f32, lng as f32, {
                    let offset = poly.vertex_offset as usize;
                    let count = poly.vertex_count as usize;
                    &all_vertices[offset..offset + count]
                }) {
                    best_by_level[level] = Some((poly.area, poly));
                }
            });
        }

        let mut result = AdminResult::default();

        for level in 0..12 {
            if let Some((_, poly)) = best_by_level[level] {
                let name = self.get_string(poly.name_id);
                match poly.admin_level {
                    2 => result.country = Some(name),
                    4 => result.state = Some(name),
                    6 => result.county = Some(name),
                    8 => result.city = Some(name),
                    11 => result.postcode = Some(name),
                    _ => {}
                }
            }
        }

        result
    }

    // --- Combined query ---

    fn query(&self, lat: f64, lng: f64) -> Address<'_> {
        let max_addr_dist = 0.0005 * 0.0005; // ~50m, squared
        let max_street_dist = 0.0007 * 0.0007; // ~75m, squared

        let admin = self.find_admin(lat, lng);
        let (addr, interp, street) = self.query_geo(lat, lng);

        // 1. Try nearest address point
        if let Some((dist, point)) = addr {
            if dist < max_addr_dist {
                return Address {
                    housenumber: Some(Cow::Borrowed(self.get_string(point.housenumber_id))),
                    street: Some(self.get_string(point.street_id)),
                    city: admin.city,
                    state: admin.state,
                    postcode: admin.postcode,
                    country: admin.country,
                };
            }
        }

        // 2. Try interpolation
        if let Some((dist, street_name, number)) = interp {
            if dist < max_addr_dist {
                return Address {
                    housenumber: Some(Cow::Owned(number.to_string())),
                    street: Some(street_name),
                    city: admin.city,
                    state: admin.state,
                    postcode: admin.postcode,
                    country: admin.country,
                };
            }
        }

        // 3. Fall back to nearest street
        if let Some((dist, way)) = street {
            if dist < max_street_dist {
                return Address {
                    housenumber: None,
                    street: Some(self.get_string(way.name_id)),
                    city: admin.city,
                    state: admin.state,
                    postcode: admin.postcode,
                    country: admin.country,
                };
            }
        }

        // 4. Admin only
        if admin.country.is_some() || admin.city.is_some() {
            return Address {
                housenumber: None,
                street: None,
                city: admin.city,
                state: admin.state,
                postcode: admin.postcode,
                country: admin.country,
            };
        }

        Address::default()
    }
}

// --- Geometry helpers ---

fn dist_sq(dlat: f64, dlng: f64, cos_lat: f64) -> f64 {
    dlat * dlat + dlng * dlng * cos_lat * cos_lat
}

fn point_to_segment_with_t(
    px: f64, py: f64,
    ax: f64, ay: f64,
    bx: f64, by: f64,
    cos_lat: f64,
) -> (f64, f64) {
    let dx = bx - ax;
    let dy = by - ay;
    let len_sq = dx * dx + dy * dy;

    let t = if len_sq == 0.0 {
        0.0
    } else {
        (((px - ax) * dx + (py - ay) * dy) / len_sq).clamp(0.0, 1.0)
    };

    let proj_x = ax + t * dx;
    let proj_y = ay + t * dy;
    let dlat = (px - proj_x).to_radians();
    let dlng = (py - proj_y).to_radians();
    (dist_sq(dlat, dlng, cos_lat), t)
}

fn point_to_segment_distance(
    px: f64, py: f64,
    ax: f64, ay: f64,
    bx: f64, by: f64,
    cos_lat: f64,
) -> f64 {
    point_to_segment_with_t(px, py, ax, ay, bx, by, cos_lat).0
}

// Ray casting point-in-polygon test
fn point_in_polygon(lat: f32, lng: f32, vertices: &[NodeCoord]) -> bool {
    let mut inside = false;
    let n = vertices.len();
    let mut j = n - 1;

    for i in 0..n {
        let vi = &vertices[i];
        let vj = &vertices[j];

        if ((vi.lng > lng) != (vj.lng > lng))
            && (lat < (vj.lat - vi.lat) * (lng - vi.lng) / (vj.lng - vi.lng) + vi.lat)
        {
            inside = !inside;
        }
        j = i;
    }

    inside
}

// --- API types ---

#[derive(Default)]
struct AdminResult<'a> {
    country: Option<&'a str>,
    state: Option<&'a str>,
    county: Option<&'a str>,
    city: Option<&'a str>,
    postcode: Option<&'a str>,
}

#[derive(Serialize, Default)]
struct Address<'a> {
    #[serde(skip_serializing_if = "Option::is_none")]
    housenumber: Option<Cow<'a, str>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    street: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    city: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    state: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    postcode: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    country: Option<&'a str>,
}

#[derive(Deserialize)]
struct QueryParams {
    lat: f64,
    lng: f64,
}

async fn reverse_geocode(
    Query(params): Query<QueryParams>,
    index: axum::extract::State<Arc<Index>>,
) -> ([(axum::http::header::HeaderName, &'static str); 1], String) {
    let address = index.query(params.lat, params.lng);
    let json = serde_json::to_string(&address).unwrap_or_default();
    ([(axum::http::header::CONTENT_TYPE, "application/json")], json)
}

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    let data_dir = args.get(1).map(|s| s.as_str()).unwrap_or(".");
    let bind_addr = args.get(2).map(|s| s.as_str()).unwrap_or("0.0.0.0:3000");

    eprintln!("Loading index from {}...", data_dir);
    let index = Arc::new(Index::load(data_dir));
    eprintln!("Index loaded. Starting server on {}...", bind_addr);

    let app = Router::new()
        .route("/reverse", get(reverse_geocode))
        .with_state(index);

    let listener = tokio::net::TcpListener::bind(bind_addr).await.unwrap();
    axum::serve(listener, app).await.unwrap();
}

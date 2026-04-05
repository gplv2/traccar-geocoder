#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <osmium/handler.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/visitor.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <fcntl.h>
#include <iomanip>
#include <unistd.h>
#include <sys/stat.h>
#include <osmium/index/map/sparse_file_array.hpp>
#include <osmium/index/map/sparse_mem_array.hpp>
#include <osmium/area/assembler.hpp>
#include <osmium/area/multipolygon_manager.hpp>
#include <osmium/area/problem_reporter_stream.hpp>

#include <s2/s2cell_id.h>
#include <s2/s2latlng.h>
#include <s2/s2region_coverer.h>
#include <s2/s2polyline.h>
#include <s2/s2polygon.h>
#include <s2/s2loop.h>
#include <s2/s2builder.h>
#include <s2/s2builderutil_s2polygon_layer.h>
#include <s2/s2latlng_rect.h>

// --- Binary format structs ---

struct WayHeader {
    uint32_t node_offset;
    uint8_t node_count;
    uint32_t name_id;
};

struct AddrPoint {
    float lat;
    float lng;
    uint32_t housenumber_id;
    uint32_t street_id;
};

struct InterpWay {
    uint32_t node_offset;
    uint8_t node_count;
    uint32_t street_id;
    uint32_t start_number;
    uint32_t end_number;
    uint8_t interpolation;
};

struct AdminPolygon {
    uint32_t vertex_offset;
    uint16_t vertex_count;
    uint32_t name_id;
    uint8_t admin_level;
    float area;
    uint16_t country_code;
};

struct NodeCoord {
    float lat;
    float lng;
};

static const uint32_t INTERIOR_FLAG = 0x80000000u;
static const uint32_t ID_MASK = 0x7FFFFFFFu;

// --- String interning ---

class StringPool {
public:
    uint32_t intern(const std::string& s) {
        auto it = index_.find(s);
        if (it != index_.end()) {
            return it->second;
        }
        uint32_t offset = static_cast<uint32_t>(data_.size());
        index_[s] = offset;
        data_.insert(data_.end(), s.begin(), s.end());
        data_.push_back('\0');
        return offset;
    }

    const std::vector<char>& data() const { return data_; }

private:
    std::unordered_map<std::string, uint32_t> index_;
    std::vector<char> data_;
};

// --- Flat cell entry for sorted pair vectors ---

struct CellEntry {
    uint64_t cell_id;
    uint32_t item_id;
} __attribute__((packed));

static bool cell_entry_less(const CellEntry& a, const CellEntry& b) {
    return a.cell_id < b.cell_id || (a.cell_id == b.cell_id && a.item_id < b.item_id);
}

static bool cell_entry_equal(const CellEntry& a, const CellEntry& b) {
    return a.cell_id == b.cell_id && a.item_id == b.item_id;
}

static void sort_and_dedup(std::vector<CellEntry>& pairs) {
    std::sort(pairs.begin(), pairs.end(), cell_entry_less);
    pairs.erase(std::unique(pairs.begin(), pairs.end(), cell_entry_equal), pairs.end());
    pairs.shrink_to_fit();
}

static size_t count_unique_cells(const std::vector<CellEntry>& pairs) {
    size_t count = 0;
    for (size_t i = 0; i < pairs.size(); ) {
        count++;
        uint64_t cur = pairs[i].cell_id;
        while (i < pairs.size() && pairs[i].cell_id == cur) i++;
    }
    return count;
}

// --- Collected data ---

static StringPool strings;

// Streets
static std::vector<WayHeader> ways;
static std::vector<NodeCoord> street_nodes;
static std::vector<CellEntry> way_pairs;

// Addresses
static std::vector<AddrPoint> addr_points;
static std::vector<CellEntry> addr_pairs;

// Interpolation
static std::vector<InterpWay> interp_ways;
static std::vector<NodeCoord> interp_nodes;
static std::vector<CellEntry> interp_pairs;

// Admin boundaries
static std::vector<AdminPolygon> admin_polygons;
static std::vector<NodeCoord> admin_vertices;
static std::vector<CellEntry> admin_pairs;

// --- S2 helpers ---

static int kStreetCellLevel = 17;
static int kAdminCellLevel = 10;
static size_t kMaxVertices = 50000;

// Diagnostic flags and counters
static bool verbose = false;
static bool debug = false;
static bool in_memory = false;
static std::string tmp_dir;

// Timer helper
using Clock = std::chrono::steady_clock;
using Duration = std::chrono::duration<double>;

static std::string format_duration(double secs) {
    char buf[64];
    if (secs < 60) {
        snprintf(buf, sizeof(buf), "%.1fs", secs);
    } else if (secs < 3600) {
        snprintf(buf, sizeof(buf), "%dm %ds", (int)(secs / 60), (int)secs % 60);
    } else {
        snprintf(buf, sizeof(buf), "%dh %dm %ds", (int)(secs / 3600), (int)(secs / 60) % 60, (int)secs % 60);
    }
    return buf;
}

static std::string format_rate(uint64_t count, double secs) {
    char buf[64];
    if (secs <= 0) return "N/A";
    double per_sec = count / secs;
    if (per_sec >= 1000000) {
        snprintf(buf, sizeof(buf), "%.1fM/s", per_sec / 1000000);
    } else if (per_sec >= 1000) {
        snprintf(buf, sizeof(buf), "%.1fK/s", per_sec / 1000);
    } else {
        snprintf(buf, sizeof(buf), "%.0f/s", per_sec);
    }
    return buf;
}

static uint64_t admin_stored = 0;
static uint64_t admin_s2_direct = 0;
static uint64_t admin_s2_repaired = 0;
static uint64_t admin_bbox_fallback = 0;
static uint64_t admin_simplified_dropped = 0;
static uint64_t admin_too_few_points = 0;
static uint64_t admin_invalid_nodes_skipped = 0;

static std::vector<S2CellId> cover_edge(double lat1, double lng1, double lat2, double lng2) {
    static S2RegionCoverer coverer([](){
        S2RegionCoverer::Options options;
        options.set_fixed_level(kStreetCellLevel);
        return options;
    }());

    S2Point p1 = S2LatLng::FromDegrees(lat1, lng1).ToPoint();
    S2Point p2 = S2LatLng::FromDegrees(lat2, lng2).ToPoint();

    if (p1 == p2) {
        return {S2CellId(p1).parent(kStreetCellLevel)};
    }

    std::vector<S2Point> points = {p1, p2};
    S2Polyline polyline(points);
    S2CellUnion covering = coverer.GetCovering(polyline);
    return covering.cell_ids();
}

static S2CellId point_to_cell(double lat, double lng) {
    return S2CellId(S2LatLng::FromDegrees(lat, lng)).parent(kStreetCellLevel);
}

// Fallback cell covering using the polygon's bounding box.
// Used when both S2Loop validation and S2Builder repair fail.
// All cells are non-interior (forces point-in-polygon test at query time).
// Uses set_fixed_level to avoid cell explosion from expanding coarse cells.
static std::vector<std::pair<S2CellId, bool>> cover_polygon_bbox(
    const std::vector<std::pair<double,double>>& vertices) {
    double min_lat = 90, max_lat = -90, min_lng = 180, max_lng = -180;
    for (const auto& [lat, lng] : vertices) {
        min_lat = std::min(min_lat, lat);
        max_lat = std::max(max_lat, lat);
        min_lng = std::min(min_lng, lng);
        max_lng = std::max(max_lng, lng);
    }
    S2LatLngRect rect(
        S2LatLng::FromDegrees(min_lat, min_lng),
        S2LatLng::FromDegrees(max_lat, max_lng));

    static S2RegionCoverer coverer([](){
        S2RegionCoverer::Options options;
        options.set_fixed_level(kAdminCellLevel);
        options.set_max_cells(500);
        return options;
    }());
    S2CellUnion covering = coverer.GetCovering(rect);

    std::vector<std::pair<S2CellId, bool>> result;
    result.reserve(covering.size());
    for (const auto& cell : covering.cell_ids()) {
        result.emplace_back(cell, false);
    }
    return result;
}

// Returns pairs of (cell_id, is_interior)
static std::vector<std::pair<S2CellId, bool>> cover_polygon(
    const std::vector<std::pair<double,double>>& vertices,
    const char* name = "", uint8_t admin_level = 0) {
    std::vector<S2Point> points;
    points.reserve(vertices.size());
    for (const auto& [lat, lng] : vertices) {
        S2Point p = S2LatLng::FromDegrees(lat, lng).ToPoint();
        if (!points.empty() && points.back() == p) continue;
        points.push_back(p);
    }
    // Remove closing duplicate if first == last
    if (points.size() > 1 && points.front() == points.back()) {
        points.pop_back();
    }
    if (points.size() < 3) {
        admin_too_few_points++;
        return {};
    }

    // Build S2Loop, attempt repair if invalid, bbox fallback as last resort
    S2Error error;
    auto loop = std::make_unique<S2Loop>(points, S2Debug::DISABLE);
    loop->Normalize();

    S2Polygon polygon;
    if (!loop->FindValidationError(&error)) {
        // Fast path: valid polygon
        polygon = S2Polygon(std::move(loop));
        admin_s2_direct++;
    } else {
        // Repair path: use S2Builder to fix crossing edges
        S2Builder::Options builder_options;
        builder_options.set_split_crossing_edges(true);
        S2Builder builder(builder_options);
        S2Polygon repaired;
        builder.StartLayer(
            std::make_unique<s2builderutil::S2PolygonLayer>(&repaired));
        for (size_t i = 0; i < points.size(); i++) {
            builder.AddEdge(points[i], points[(i + 1) % points.size()]);
        }
        S2Error build_error;
        if (builder.Build(&build_error) && repaired.num_loops() > 0) {
            polygon = std::move(repaired);
            admin_s2_repaired++;
            if (verbose) {
                std::cerr << "  REPAIR: \"" << name << "\" (level " << (int)admin_level
                          << ", " << points.size() << " vertices): "
                          << error << std::endl;
            }
        } else {
            // Last resort: bbox covering
            admin_bbox_fallback++;
            if (verbose) {
                std::cerr << "  BBOX: \"" << name << "\" (level " << (int)admin_level
                          << ", " << points.size() << " vertices): S2=" << error;
                if (build_error.ok()) {
                    std::cerr << ", builder produced 0 loops";
                } else {
                    std::cerr << ", builder=" << build_error;
                }
                std::cerr << std::endl;
            }
            return cover_polygon_bbox(vertices);
        }
    }

    static S2RegionCoverer coverer([](){
        S2RegionCoverer::Options options;
        options.set_max_level(kAdminCellLevel);
        options.set_max_cells(200);
        return options;
    }());
    S2CellUnion covering = coverer.GetCovering(polygon);
    S2CellUnion interior = coverer.GetInteriorCovering(polygon);

    // Build set of interior cell IDs for fast lookup
    std::unordered_set<uint64_t> interior_set;
    for (const auto& cell : interior.cell_ids()) {
        if (cell.level() <= kAdminCellLevel) {
            auto begin = cell.range_min().parent(kAdminCellLevel);
            auto end = cell.range_max().parent(kAdminCellLevel);
            for (auto c = begin; c != end; c = c.next()) {
                interior_set.insert(c.id());
            }
            interior_set.insert(end.id());
        } else {
            interior_set.insert(cell.parent(kAdminCellLevel).id());
        }
    }

    // Normalize all covering cells to kAdminCellLevel
    std::vector<std::pair<S2CellId, bool>> result;
    for (const auto& cell : covering.cell_ids()) {
        if (cell.level() <= kAdminCellLevel) {
            auto begin = cell.range_min().parent(kAdminCellLevel);
            auto end = cell.range_max().parent(kAdminCellLevel);
            for (auto c = begin; c != end; c = c.next()) {
                result.emplace_back(c, interior_set.count(c.id()) > 0);
            }
            result.emplace_back(end, interior_set.count(end.id()) > 0);
        } else {
            auto parent = cell.parent(kAdminCellLevel);
            result.emplace_back(parent, interior_set.count(parent.id()) > 0);
        }
    }

    // Deduplicate by cell_id, keeping interior=true if any duplicate is interior
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    auto it = result.begin();
    for (auto curr = result.begin(); curr != result.end(); ) {
        auto next = curr + 1;
        bool is_interior = curr->second;
        while (next != result.end() && next->first == curr->first) {
            is_interior = is_interior || next->second;
            ++next;
        }
        *it = {curr->first, is_interior};
        ++it;
        curr = next;
    }
    result.erase(it, result.end());
    return result;
}

// Approximate polygon area in square degrees
static float polygon_area(const std::vector<std::pair<double,double>>& vertices) {
    double area = 0;
    size_t n = vertices.size();
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        area += vertices[i].first * vertices[j].second;
        area -= vertices[j].first * vertices[i].second;
    }
    return static_cast<float>(std::fabs(area) / 2.0);
}

// Douglas-Peucker simplification
static void dp_simplify(const std::vector<std::pair<double,double>>& pts,
                        size_t start, size_t end, double epsilon,
                        std::vector<bool>& keep) {
    if (end <= start + 1) return;

    double max_dist = 0;
    size_t max_idx = start;

    double ax = pts[start].first, ay = pts[start].second;
    double bx = pts[end].first, by = pts[end].second;
    double dx = bx - ax, dy = by - ay;
    double len_sq = dx * dx + dy * dy;

    for (size_t i = start + 1; i < end; i++) {
        double px = pts[i].first - ax, py = pts[i].second - ay;
        double dist;
        if (len_sq == 0) {
            dist = std::sqrt(px * px + py * py);
        } else {
            double t = std::max(0.0, std::min(1.0, (px * dx + py * dy) / len_sq));
            double proj_x = t * dx - px, proj_y = t * dy - py;
            dist = std::sqrt(proj_x * proj_x + proj_y * proj_y);
        }
        if (dist > max_dist) {
            max_dist = dist;
            max_idx = i;
        }
    }

    if (max_dist > epsilon) {
        keep[max_idx] = true;
        dp_simplify(pts, start, max_idx, epsilon, keep);
        dp_simplify(pts, max_idx, end, epsilon, keep);
    }
}

static std::vector<std::pair<double,double>> simplify_polygon(
    const std::vector<std::pair<double,double>>& pts, size_t max_vertices) {
    if (pts.size() <= max_vertices) return pts;

    // Binary search for epsilon that gives ~max_vertices
    double lo = 0, hi = 1.0;
    std::vector<std::pair<double,double>> result;

    for (int iter = 0; iter < 20; iter++) {
        double epsilon = (lo + hi) / 2;
        std::vector<bool> keep(pts.size(), false);
        keep[0] = true;
        keep[pts.size() - 1] = true;
        dp_simplify(pts, 0, pts.size() - 1, epsilon, keep);

        size_t count = 0;
        for (bool k : keep) if (k) count++;

        if (count > max_vertices) {
            lo = epsilon;
        } else {
            hi = epsilon;
        }
    }

    std::vector<bool> keep(pts.size(), false);
    keep[0] = true;
    keep[pts.size() - 1] = true;
    dp_simplify(pts, 0, pts.size() - 1, hi, keep);

    result.clear();
    for (size_t i = 0; i < pts.size(); i++) {
        if (keep[i]) result.push_back(pts[i]);
    }
    return result;
}

// --- Highway filter ---

static const std::unordered_set<std::string> kExcludedHighways = {
    "footway", "path", "track", "steps", "cycleway",
    "service", "pedestrian", "bridleway", "construction"
};

static bool is_included_highway(const char* value) {
    return kExcludedHighways.find(value) == kExcludedHighways.end();
}

// --- Parse house number (leading digits) ---

static uint32_t parse_house_number(const char* s) {
    if (!s) return 0;
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

// --- Add an address point ---

static uint64_t addr_count_total = 0;

static void add_addr_point(double lat, double lng, const char* housenumber, const char* street) {
    uint32_t addr_id = static_cast<uint32_t>(addr_points.size());
    addr_points.push_back({
        static_cast<float>(lat),
        static_cast<float>(lng),
        strings.intern(housenumber),
        strings.intern(street)
    });

    S2CellId cell = point_to_cell(lat, lng);
    addr_pairs.push_back({cell.id(), addr_id});

    addr_count_total++;
    if (addr_count_total % 1000000 == 0) {
        std::cerr << "Collected " << addr_count_total / 1000000 << "M addresses..." << std::endl;
    }
}

// --- Add an admin polygon ---

static void add_admin_polygon(const std::vector<std::pair<double,double>>& vertices,
                               const char* name, uint8_t admin_level,
                               const char* country_code) {
    // Simplify large polygons
    auto simplified = simplify_polygon(vertices, kMaxVertices);
    if (verbose && simplified.size() < vertices.size()) {
        std::cerr << "  SIMPLIFY: \"" << name << "\" (level " << (int)admin_level
                  << "): " << vertices.size() << " -> " << simplified.size()
                  << " vertices" << std::endl;
    }
    if (simplified.size() < 3) {
        admin_simplified_dropped++;
        if (verbose) {
            std::cerr << "  DROP: \"" << name << "\" (level " << (int)admin_level
                      << "): simplified from " << vertices.size() << " to "
                      << simplified.size() << " vertices" << std::endl;
        }
        return;
    }

    uint32_t poly_id = static_cast<uint32_t>(admin_polygons.size());
    uint32_t vertex_offset = static_cast<uint32_t>(admin_vertices.size());

    for (const auto& [lat, lng] : simplified) {
        admin_vertices.push_back({static_cast<float>(lat), static_cast<float>(lng)});
    }

    AdminPolygon poly{};
    poly.vertex_offset = vertex_offset;
    poly.vertex_count = static_cast<uint16_t>(std::min(simplified.size(), size_t(65535)));
    poly.name_id = strings.intern(name);
    poly.admin_level = admin_level;
    poly.area = polygon_area(simplified);
    poly.country_code = (country_code && country_code[0] && country_code[1])
        ? static_cast<uint16_t>((country_code[0] << 8) | country_code[1])
        : 0;
    admin_polygons.push_back(poly);
    admin_stored++;

    // S2 cell coverage (high bit marks interior cells)
    auto cell_ids = cover_polygon(simplified, name, admin_level);
    for (const auto& [cell_id, is_interior] : cell_ids) {
        uint32_t entry = is_interior ? (poly_id | INTERIOR_FLAG) : poly_id;
        admin_pairs.push_back({cell_id.id(), entry});
    }
}

// --- OSM handler (pass 2) ---

class BuildHandler : public osmium::handler::Handler {
public:
    void node(const osmium::Node& node) {
        const char* housenumber = node.tags()["addr:housenumber"];
        if (!housenumber) return;
        const char* street = node.tags()["addr:street"];
        if (!street) return;
        if (!node.location().valid()) return;

        add_addr_point(node.location().lat(), node.location().lon(), housenumber, street);
    }

    void way(const osmium::Way& way) {
        // Address interpolation
        const char* interpolation = way.tags()["addr:interpolation"];
        if (interpolation) {
            process_interpolation_way(way, interpolation);
            return;
        }

        // Building addresses
        const char* housenumber = way.tags()["addr:housenumber"];
        if (housenumber) {
            const char* street = way.tags()["addr:street"];
            if (street) {
                process_building_address(way, housenumber, street);
            }
        }

        // Highway ways
        const char* highway = way.tags()["highway"];
        if (highway && is_included_highway(highway)) {
            const char* name = way.tags()["name"];
            if (name) {
                process_highway(way, name);
            }
        }
    }

    void area(const osmium::Area& area) {
        const char* boundary = area.tags()["boundary"];
        if (!boundary) return;

        bool is_admin = (std::strcmp(boundary, "administrative") == 0);
        bool is_postal = (std::strcmp(boundary, "postal_code") == 0);
        if (!is_admin && !is_postal) return;

        uint8_t admin_level = 0;
        if (is_admin) {
            const char* level_str = area.tags()["admin_level"];
            if (!level_str) return;
            admin_level = static_cast<uint8_t>(std::atoi(level_str));
            if (admin_level < 2 || admin_level > 10) return;
        } else {
            admin_level = 11; // use 11 for postal codes
        }

        const char* name = area.tags()["name"];
        if (!name && is_admin) return;

        // For postal codes, use postal_code tag as name
        std::string name_str;
        if (is_postal) {
            const char* postal_code = area.tags()["postal_code"];
            if (!postal_code) postal_code = name;
            if (!postal_code) return;
            name_str = postal_code;
        } else {
            name_str = name;
        }

        // Extract country code for level 2 boundaries
        const char* country_code = (admin_level == 2)
            ? area.tags()["ISO3166-1:alpha2"]
            : nullptr;
        // Extract outer ring vertices
        for (const auto& outer_ring : area.outer_rings()) {
            std::vector<std::pair<double,double>> vertices;
            uint32_t skipped_nodes = 0;
            for (const auto& node_ref : outer_ring) {
                if (node_ref.location().valid()) {
                    vertices.emplace_back(node_ref.location().lat(), node_ref.location().lon());
                } else {
                    skipped_nodes++;
                }
            }
            if (verbose && skipped_nodes > 0) {
                std::cerr << "  WARN: " << name_str << " (level " << (int)admin_level
                          << "): skipped " << skipped_nodes << " invalid nodes in outer ring"
                          << std::endl;
            }
            admin_invalid_nodes_skipped += skipped_nodes;
            if (vertices.size() >= 3) {
                add_admin_polygon(vertices, name_str.c_str(), admin_level, country_code);
            }
        }

        admin_count_++;
        if (admin_count_ % 10000 == 0) {
            std::cerr << "Processed " << admin_count_ / 1000 << "K admin boundaries..." << std::endl;
        }
    }

    uint64_t way_count() const { return way_count_; }
    uint64_t building_addr_count() const { return building_addr_count_; }
    uint64_t interp_count() const { return interp_count_; }
    uint64_t admin_count() const { return admin_count_; }

private:
    uint64_t way_count_ = 0;
    uint64_t building_addr_count_ = 0;
    uint64_t interp_count_ = 0;
    uint64_t admin_count_ = 0;

    void process_building_address(const osmium::Way& way, const char* housenumber, const char* street) {
        const auto& wnodes = way.nodes();
        if (wnodes.empty()) return;

        double sum_lat = 0, sum_lng = 0;
        int valid = 0;
        for (const auto& nr : wnodes) {
            if (!nr.location().valid()) continue;
            sum_lat += nr.location().lat();
            sum_lng += nr.location().lon();
            valid++;
        }
        if (valid == 0) return;

        add_addr_point(sum_lat / valid, sum_lng / valid, housenumber, street);
        building_addr_count_++;
    }

    void process_interpolation_way(const osmium::Way& way, const char* interpolation) {
        const auto& wnodes = way.nodes();
        if (wnodes.size() < 2) return;

        for (const auto& nr : wnodes) {
            if (!nr.location().valid()) return;
        }

        const char* street = way.tags()["addr:street"];
        if (!street) return;

        uint8_t interp_type = 0;
        if (std::strcmp(interpolation, "even") == 0) interp_type = 1;
        else if (std::strcmp(interpolation, "odd") == 0) interp_type = 2;

        uint32_t interp_id = static_cast<uint32_t>(interp_ways.size());
        uint32_t node_offset = static_cast<uint32_t>(interp_nodes.size());

        for (const auto& nr : wnodes) {
            interp_nodes.push_back({
                static_cast<float>(nr.location().lat()),
                static_cast<float>(nr.location().lon())
            });
        }

        InterpWay iw{};
        iw.node_offset = node_offset;
        iw.node_count = static_cast<uint8_t>(std::min(wnodes.size(), size_t(255)));
        iw.street_id = strings.intern(street);
        iw.start_number = 0;
        iw.end_number = 0;
        iw.interpolation = interp_type;
        interp_ways.push_back(iw);

        std::unordered_set<uint64_t> interp_cells;
        for (size_t i = 0; i + 1 < wnodes.size(); i++) {
            double lat1 = wnodes[i].location().lat();
            double lng1 = wnodes[i].location().lon();
            double lat2 = wnodes[i + 1].location().lat();
            double lng2 = wnodes[i + 1].location().lon();

            auto cell_ids = cover_edge(lat1, lng1, lat2, lng2);
            for (const auto& cell_id : cell_ids) {
                interp_cells.insert(cell_id.id());
            }
        }
        for (uint64_t cell_id : interp_cells) {
            interp_pairs.push_back({cell_id, interp_id});
        }

        interp_count_++;
    }

    void process_highway(const osmium::Way& way, const char* name) {
        const auto& wnodes = way.nodes();
        if (wnodes.size() < 2) return;

        for (const auto& nr : wnodes) {
            if (!nr.location().valid()) return;
        }

        uint32_t way_id = static_cast<uint32_t>(ways.size());
        uint32_t node_offset = static_cast<uint32_t>(street_nodes.size());

        for (const auto& nr : wnodes) {
            street_nodes.push_back({
                static_cast<float>(nr.location().lat()),
                static_cast<float>(nr.location().lon())
            });
        }

        WayHeader header{};
        header.node_offset = node_offset;
        header.node_count = static_cast<uint8_t>(std::min(wnodes.size(), size_t(255)));
        header.name_id = strings.intern(name);
        ways.push_back(header);

        std::unordered_set<uint64_t> way_cells;
        for (size_t i = 0; i + 1 < wnodes.size(); i++) {
            double lat1 = wnodes[i].location().lat();
            double lng1 = wnodes[i].location().lon();
            double lat2 = wnodes[i + 1].location().lat();
            double lng2 = wnodes[i + 1].location().lon();

            auto cell_ids = cover_edge(lat1, lng1, lat2, lng2);
            for (const auto& cell_id : cell_ids) {
                way_cells.insert(cell_id.id());
            }
        }
        for (uint64_t cell_id : way_cells) {
            way_pairs.push_back({cell_id, way_id});
        }

        way_count_++;
        if (way_count_ % 1000000 == 0) {
            std::cerr << "Processed " << way_count_ / 1000000 << "M street ways..." << std::endl;
        }
    }
};

// --- Pass 1.5: Node filtering for memory-efficient builds ---

// Bitmap storing 1 bit per possible node ID. Dynamic resizing.
class NodeBitmap {
    std::vector<uint64_t> bits_;
public:
    void set(uint64_t id) {
        size_t idx = id / 64;
        if (idx >= bits_.size()) bits_.resize(idx + 65536, 0);
        bits_[idx] |= (1ULL << (id % 64));
    }
    bool test(uint64_t id) const {
        size_t idx = id / 64;
        if (idx >= bits_.size()) return false;
        return (bits_[idx] >> (id % 64)) & 1ULL;
    }
    size_t count() const {
        size_t n = 0;
        for (auto w : bits_) n += __builtin_popcountll(w);
        return n;
    }
    size_t memory_mb() const { return bits_.size() * sizeof(uint64_t) / (1024 * 1024); }
    void clear() { std::vector<uint64_t>{}.swap(bits_); }
};

// Collects way IDs that are members of admin/postal boundary relations (runs in Pass 1)
class RelationWayCollector : public osmium::handler::Handler {
    std::unordered_set<uint64_t> admin_way_ids_;
public:
    void relation(const osmium::Relation& relation) {
        const char* boundary = relation.tags()["boundary"];
        if (!boundary) return;
        bool is_admin = (std::strcmp(boundary, "administrative") == 0);
        bool is_postal = (std::strcmp(boundary, "postal_code") == 0);
        if (!is_admin && !is_postal) return;
        if (is_admin) {
            const char* level = relation.tags()["admin_level"];
            if (!level) return;
            int l = std::atoi(level);
            if (l < 2 || l > 10) return;
        }
        for (const auto& member : relation.members()) {
            if (member.type() == osmium::item_type::way)
                admin_way_ids_.insert(member.positive_ref());
        }
    }
    const std::unordered_set<uint64_t>& admin_way_ids() const { return admin_way_ids_; }
    size_t size() const { return admin_way_ids_.size(); }
};

// Scans ways to collect needed node IDs into a bitmap (runs in Pass 1.5)
class WayNodeCollector : public osmium::handler::Handler {
    NodeBitmap& bitmap_;
    const std::unordered_set<uint64_t>& admin_way_ids_;
    uint64_t collected_ways_ = 0;
public:
    WayNodeCollector(NodeBitmap& bitmap, const std::unordered_set<uint64_t>& admin_way_ids)
        : bitmap_(bitmap), admin_way_ids_(admin_way_ids) {}

    void way(const osmium::Way& way) {
        bool needed = false;
        // Admin/postal boundary member ways (from relations)
        if (admin_way_ids_.count(way.positive_id())) needed = true;
        // Simple closed way boundaries (not part of a relation)
        if (!needed) {
            const char* boundary = way.tags()["boundary"];
            if (boundary && (std::strcmp(boundary, "administrative") == 0 ||
                             std::strcmp(boundary, "postal_code") == 0)) needed = true;
        }
        // Building addresses (way with addr:housenumber + addr:street)
        if (!needed && way.tags()["addr:housenumber"] && way.tags()["addr:street"]) needed = true;
        // Address interpolation
        if (!needed && way.tags()["addr:interpolation"]) needed = true;
        // Named highways
        if (!needed) {
            const char* hw = way.tags()["highway"];
            if (hw && is_included_highway(hw) && way.tags()["name"]) needed = true;
        }
        if (!needed) return;
        for (const auto& nr : way.nodes())
            bitmap_.set(nr.positive_ref());
        collected_ways_++;
    }
    uint64_t collected_ways() const { return collected_ways_; }
};

// Filtered version of osmium::handler::NodeLocationsForWays.
// Only stores locations for nodes present in the bitmap.
template <typename TIndex>
class FilteredNodeLocationsForWays : public osmium::handler::Handler {
    TIndex& m_storage;
    const NodeBitmap& m_bitmap;
    osmium::unsigned_object_id_type m_last_id = 0;
    bool m_ignore_errors = false;
    bool m_must_sort = false;
    uint64_t m_stored = 0;
    uint64_t m_skipped = 0;

public:
    FilteredNodeLocationsForWays(TIndex& storage, const NodeBitmap& bitmap)
        : m_storage(storage), m_bitmap(bitmap) {}

    void ignore_errors() { m_ignore_errors = true; }
    uint64_t stored() const { return m_stored; }
    uint64_t skipped() const { return m_skipped; }

    void node(const osmium::Node& node) {
        const auto id = node.positive_id();
        if (id < m_last_id) m_must_sort = true;
        m_last_id = id;
        if (!m_bitmap.test(id)) { m_skipped++; return; }
        m_storage.set(static_cast<osmium::unsigned_object_id_type>(
            node.id() >= 0 ? node.id() : -node.id()), node.location());
        m_stored++;
    }

    void way(osmium::Way& way) {
        if (m_must_sort) {
            m_storage.sort();
            m_must_sort = false;
            m_last_id = std::numeric_limits<osmium::unsigned_object_id_type>::max();
        }
        bool error = false;
        for (auto& nr : way.nodes()) {
            const auto id = nr.ref();
            nr.set_location(m_storage.get_noexcept(
                static_cast<osmium::unsigned_object_id_type>(id >= 0 ? id : -id)));
            if (!nr.location()) error = true;
        }
        if (!m_ignore_errors && error)
            throw osmium::not_found{"location not found in filtered node location index"};
    }
};

// --- Resolve interpolation way endpoint house numbers ---

static void resolve_interpolation_endpoints() {
    struct CoordKey {
        int32_t lat;
        int32_t lng;
        bool operator==(const CoordKey& o) const { return lat == o.lat && lng == o.lng; }
    };
    struct CoordHash {
        size_t operator()(const CoordKey& k) const {
            return std::hash<int64_t>()(((int64_t)k.lat << 32) | (uint32_t)k.lng);
        }
    };

    std::unordered_map<CoordKey, uint32_t, CoordHash> addr_by_coord;
    for (uint32_t i = 0; i < addr_points.size(); i++) {
        CoordKey key{
            static_cast<int32_t>(addr_points[i].lat * 100000),
            static_cast<int32_t>(addr_points[i].lng * 100000)
        };
        addr_by_coord[key] = i;
    }

    uint32_t resolved = 0;
    for (auto& iw : interp_ways) {
        if (iw.node_count < 2) continue;

        const auto& start = interp_nodes[iw.node_offset];
        CoordKey start_key{
            static_cast<int32_t>(start.lat * 100000),
            static_cast<int32_t>(start.lng * 100000)
        };
        auto it_start = addr_by_coord.find(start_key);

        const auto& end = interp_nodes[iw.node_offset + iw.node_count - 1];
        CoordKey end_key{
            static_cast<int32_t>(end.lat * 100000),
            static_cast<int32_t>(end.lng * 100000)
        };
        auto it_end = addr_by_coord.find(end_key);

        if (it_start != addr_by_coord.end()) {
            const char* hn = strings.data().data() + addr_points[it_start->second].housenumber_id;
            iw.start_number = parse_house_number(hn);
        }
        if (it_end != addr_by_coord.end()) {
            const char* hn = strings.data().data() + addr_points[it_end->second].housenumber_id;
            iw.end_number = parse_house_number(hn);
        }

        if (iw.start_number > 0 && iw.end_number > 0) resolved++;
    }

    std::cerr << "Resolved " << resolved << "/" << interp_ways.size()
              << " interpolation ways" << std::endl;
}

// --- Deduplicate: sort flat pair vectors and remove duplicate (cell_id, item_id) pairs ---
// (replaces per-cell hash map iteration with one cache-friendly sort)

// --- Memory stats from /proc/self/status ---

struct MemStats {
    size_t vm_rss_kb = 0;
    size_t vm_swap_kb = 0;
    size_t vm_size_kb = 0;
};

static MemStats get_mem_stats() {
    MemStats stats;
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0)
            stats.vm_rss_kb = std::stoul(line.substr(6));
        else if (line.compare(0, 7, "VmSwap:") == 0)
            stats.vm_swap_kb = std::stoul(line.substr(7));
        else if (line.compare(0, 7, "VmSize:") == 0)
            stats.vm_size_kb = std::stoul(line.substr(7));
    }
    return stats;
}

static std::string format_mem(size_t kb) {
    if (kb >= 1048576) return std::to_string(kb / 1048576) + "." + std::to_string((kb % 1048576) * 10 / 1048576) + " GB";
    if (kb >= 1024) return std::to_string(kb / 1024) + " MB";
    return std::to_string(kb) + " KB";
}

static void log_mem(const char* label) {
    auto m = get_mem_stats();
    std::cerr << "  [mem] " << label << ": RSS=" << format_mem(m.vm_rss_kb)
              << " Swap=" << format_mem(m.vm_swap_kb)
              << " VM=" << format_mem(m.vm_size_kb) << std::endl;
}

// --- Write cell index ---

static const uint32_t NO_DATA = 0xFFFFFFFFu;

// Write entries file using merge scan over sorted cell list and sorted pairs
static void write_entries(
    const std::string& path,
    const std::vector<uint64_t>& sorted_cells,
    const std::vector<CellEntry>& pairs,
    std::vector<uint32_t>& offsets
) {
    std::ofstream f(path, std::ios::binary);
    uint32_t current = 0;
    size_t j = 0;
    for (size_t i = 0; i < sorted_cells.size(); i++) {
        while (j < pairs.size() && pairs[j].cell_id < sorted_cells[i]) j++;
        if (j >= pairs.size() || pairs[j].cell_id != sorted_cells[i]) continue;

        offsets[i] = current;
        size_t start = j;
        while (j < pairs.size() && pairs[j].cell_id == sorted_cells[i]) j++;
        uint16_t count = static_cast<uint16_t>(std::min(j - start, size_t(65535)));
        f.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (size_t k = start; k < start + count; k++) {
            f.write(reinterpret_cast<const char*>(&pairs[k].item_id), sizeof(uint32_t));
        }
        current += sizeof(uint16_t) + count * sizeof(uint32_t);
    }
}

static void write_cell_index(
    const std::string& cells_path,
    const std::string& entries_path,
    const std::vector<CellEntry>& pairs
) {
    // First pass: compute cell boundaries and offsets for cells.bin
    {
        std::ofstream f(cells_path, std::ios::binary);
        uint32_t current_offset = 0;
        for (size_t i = 0; i < pairs.size(); ) {
            uint64_t cell_id = pairs[i].cell_id;
            size_t start = i;
            while (i < pairs.size() && pairs[i].cell_id == cell_id) i++;
            size_t count = std::min(i - start, size_t(65535));
            f.write(reinterpret_cast<const char*>(&cell_id), sizeof(cell_id));
            f.write(reinterpret_cast<const char*>(&current_offset), sizeof(current_offset));
            current_offset += sizeof(uint16_t) + count * sizeof(uint32_t);
        }
    }

    // Second pass: write entries.bin
    {
        std::ofstream f(entries_path, std::ios::binary);
        for (size_t i = 0; i < pairs.size(); ) {
            uint64_t cell_id = pairs[i].cell_id;
            size_t start = i;
            while (i < pairs.size() && pairs[i].cell_id == cell_id) i++;
            uint16_t count = static_cast<uint16_t>(std::min(i - start, size_t(65535)));
            f.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (size_t k = start; k < start + count; k++) {
                f.write(reinterpret_cast<const char*>(&pairs[k].item_id), sizeof(uint32_t));
            }
        }
    }
}

// --- Write all index files ---

static void write_index(const std::string& output_dir) {
    auto phase_start = std::chrono::steady_clock::now();
    auto phase_timer = [&]() {
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - phase_start).count();
        phase_start = now;
        return secs;
    };

    // --- Phase 1: Shrink vectors to reclaim over-reserved capacity ---
    log_mem("write start");
    std::cerr << "  Shrinking vectors to fit..." << std::endl;
    ways.shrink_to_fit();
    street_nodes.shrink_to_fit();
    addr_points.shrink_to_fit();
    admin_polygons.shrink_to_fit();
    admin_vertices.shrink_to_fit();
    interp_ways.shrink_to_fit();
    interp_nodes.shrink_to_fit();
    malloc_trim(0);
    log_mem("after shrink_to_fit");

    // --- Phase 2: Write data files first, free each vector after ---
    std::cerr << "  Writing street_ways.bin (" << ways.size() << " ways)..." << std::endl;
    {
        std::ofstream f(output_dir + "/street_ways.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(ways.data()), ways.size() * sizeof(WayHeader));
    }
    { decltype(ways){}.swap(ways); }

    std::cerr << "  Writing street_nodes.bin (" << street_nodes.size() << " nodes)..." << std::endl;
    {
        std::ofstream f(output_dir + "/street_nodes.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(street_nodes.data()), street_nodes.size() * sizeof(NodeCoord));
    }
    { decltype(street_nodes){}.swap(street_nodes); }

    std::cerr << "  Writing addr_points.bin (" << addr_points.size() << " points)..." << std::endl;
    {
        std::ofstream f(output_dir + "/addr_points.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(addr_points.data()), addr_points.size() * sizeof(AddrPoint));
    }
    { decltype(addr_points){}.swap(addr_points); }

    std::cerr << "  Writing interp_ways.bin (" << interp_ways.size() << " ways)..." << std::endl;
    {
        std::ofstream f(output_dir + "/interp_ways.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(interp_ways.data()), interp_ways.size() * sizeof(InterpWay));
    }
    { decltype(interp_ways){}.swap(interp_ways); }

    std::cerr << "  Writing interp_nodes.bin (" << interp_nodes.size() << " nodes)..." << std::endl;
    {
        std::ofstream f(output_dir + "/interp_nodes.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(interp_nodes.data()), interp_nodes.size() * sizeof(NodeCoord));
    }
    { decltype(interp_nodes){}.swap(interp_nodes); }

    std::cerr << "  Writing admin_polygons.bin (" << admin_polygons.size() << " polygons)..." << std::endl;
    {
        std::ofstream f(output_dir + "/admin_polygons.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(admin_polygons.data()), admin_polygons.size() * sizeof(AdminPolygon));
    }
    { decltype(admin_polygons){}.swap(admin_polygons); }

    std::cerr << "  Writing admin_vertices.bin (" << admin_vertices.size() << " vertices)..." << std::endl;
    {
        std::ofstream f(output_dir + "/admin_vertices.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(admin_vertices.data()), admin_vertices.size() * sizeof(NodeCoord));
    }
    { decltype(admin_vertices){}.swap(admin_vertices); }

    std::cerr << "  Writing strings.bin (" << strings.data().size() / 1024 / 1024 << " MB)..." << std::endl;
    {
        std::ofstream f(output_dir + "/strings.bin", std::ios::binary);
        f.write(strings.data().data(), strings.data().size());
    }
    strings = StringPool{};

    malloc_trim(0);
    log_mem("data files freed");
    double data_secs = phase_timer();
    std::cerr << "  Data files written (" << std::fixed << std::setprecision(1) << data_secs << "s)" << std::endl;

    // --- Phase 3: Extract unique cell IDs from sorted pair vectors ---
    std::cerr << "  Merging geo cell keys..." << std::endl;
    auto extract_unique_cells = [](const std::vector<CellEntry>& pairs) {
        std::vector<uint64_t> cells;
        for (size_t i = 0; i < pairs.size(); ) {
            cells.push_back(pairs[i].cell_id);
            uint64_t cur = pairs[i].cell_id;
            while (i < pairs.size() && pairs[i].cell_id == cur) i++;
        }
        return cells;
    };
    auto way_cells = extract_unique_cells(way_pairs);
    auto addr_cells = extract_unique_cells(addr_pairs);
    auto interp_cells = extract_unique_cells(interp_pairs);

    // Merge 3 sorted cell ID vectors
    std::vector<uint64_t> sorted_geo_cells;
    sorted_geo_cells.reserve(way_cells.size() + addr_cells.size() + interp_cells.size());
    sorted_geo_cells.insert(sorted_geo_cells.end(), way_cells.begin(), way_cells.end());
    sorted_geo_cells.insert(sorted_geo_cells.end(), addr_cells.begin(), addr_cells.end());
    sorted_geo_cells.insert(sorted_geo_cells.end(), interp_cells.begin(), interp_cells.end());
    { decltype(way_cells){}.swap(way_cells); }
    { decltype(addr_cells){}.swap(addr_cells); }
    { decltype(interp_cells){}.swap(interp_cells); }
    std::sort(sorted_geo_cells.begin(), sorted_geo_cells.end());
    sorted_geo_cells.erase(std::unique(sorted_geo_cells.begin(), sorted_geo_cells.end()), sorted_geo_cells.end());
    sorted_geo_cells.shrink_to_fit();
    log_mem("geo cells sorted");
    double merge_secs = phase_timer();
    std::cerr << "  " << sorted_geo_cells.size() << " geo cells merged (" << std::fixed << std::setprecision(1) << merge_secs << "s)" << std::endl;

    // --- Phase 4: Write entry files via merge scan, free each pair vector after ---
    std::vector<uint32_t> street_offsets(sorted_geo_cells.size(), NO_DATA);
    std::vector<uint32_t> addr_offsets(sorted_geo_cells.size(), NO_DATA);
    std::vector<uint32_t> interp_offsets(sorted_geo_cells.size(), NO_DATA);

    std::cerr << "  Writing street_entries.bin..." << std::endl;
    write_entries(output_dir + "/street_entries.bin", sorted_geo_cells, way_pairs, street_offsets);
    { decltype(way_pairs){}.swap(way_pairs); }
    malloc_trim(0);
    log_mem("way_pairs freed");

    std::cerr << "  Writing addr_entries.bin..." << std::endl;
    write_entries(output_dir + "/addr_entries.bin", sorted_geo_cells, addr_pairs, addr_offsets);
    { decltype(addr_pairs){}.swap(addr_pairs); }
    malloc_trim(0);
    log_mem("addr_pairs freed");

    std::cerr << "  Writing interp_entries.bin..." << std::endl;
    write_entries(output_dir + "/interp_entries.bin", sorted_geo_cells, interp_pairs, interp_offsets);
    { decltype(interp_pairs){}.swap(interp_pairs); }
    malloc_trim(0);
    log_mem("interp_pairs freed");

    double entries_secs = phase_timer();
    std::cerr << "  Entry files written (" << std::fixed << std::setprecision(1) << entries_secs << "s)" << std::endl;

    // --- Phase 5: Write geo_cells.bin (fully sequential, no hash lookups) ---
    std::cerr << "  Writing geo_cells.bin (" << sorted_geo_cells.size() << " cells)..." << std::endl;
    {
        std::ofstream f(output_dir + "/geo_cells.bin", std::ios::binary);
        for (size_t i = 0; i < sorted_geo_cells.size(); i++) {
            f.write(reinterpret_cast<const char*>(&sorted_geo_cells[i]), sizeof(uint64_t));
            f.write(reinterpret_cast<const char*>(&street_offsets[i]), sizeof(uint32_t));
            f.write(reinterpret_cast<const char*>(&addr_offsets[i]), sizeof(uint32_t));
            f.write(reinterpret_cast<const char*>(&interp_offsets[i]), sizeof(uint32_t));
        }
    }
    { decltype(street_offsets){}.swap(street_offsets); }
    { decltype(addr_offsets){}.swap(addr_offsets); }
    { decltype(interp_offsets){}.swap(interp_offsets); }
    { decltype(sorted_geo_cells){}.swap(sorted_geo_cells); }
    malloc_trim(0);
    log_mem("geo_cells + offsets freed");
    double geo_secs = phase_timer();
    std::cerr << "  geo_cells.bin written (" << std::fixed << std::setprecision(1) << geo_secs << "s)" << std::endl;

    // --- Phase 6: Write admin cell index ---
    size_t admin_cell_count = count_unique_cells(admin_pairs);
    std::cerr << "  Writing admin_cells.bin + admin_entries.bin (" << admin_cell_count << " cells)..." << std::endl;
    write_cell_index(output_dir + "/admin_cells.bin", output_dir + "/admin_entries.bin", admin_pairs);
    { decltype(admin_pairs){}.swap(admin_pairs); }
    malloc_trim(0);
    log_mem("all freed");
    double admin_secs = phase_timer();
    std::cerr << "  Admin index written (" << std::fixed << std::setprecision(1) << admin_secs << "s)" << std::endl;
}

// --- Capacity estimation from PBF file size ---

struct SizeEstimate {
    size_t ways;
    size_t addr_points;
    size_t street_nodes;
    size_t admin_polygons;
    size_t admin_vertices;
    size_t way_pairs;
    size_t addr_pairs;
    size_t admin_pairs;
};

static SizeEstimate estimate_from_file_size(size_t total_bytes) {
    double gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
    // Per-GB ratios calibrated from Europe (32 GB) and planet (86 GB) builds.
    // Over-reservation is OK — shrink_to_fit() at write time reclaims it.
    // Under-reservation causes expensive reallocations, so use max observed + headroom.
    return {
        static_cast<size_t>(gb * 900000),    // ways (~870K/GB Europe, ~557K/GB planet)
        static_cast<size_t>(gb * 3000000),   // addr_points (~2.7M/GB Europe, ~1.9M/GB planet)
        static_cast<size_t>(gb * 6500000),   // street_nodes (~5.9M/GB planet, ~4.5M/GB Europe)
        static_cast<size_t>(gb * 15000),     // admin_polygons
        static_cast<size_t>(gb * 5000000),   // admin_vertices (~4.2M/GB planet)
        static_cast<size_t>(gb * 9000000),   // way_pairs (~900K ways × 10 cells/way)
        static_cast<size_t>(gb * 3000000),   // addr_pairs (1 cell per addr, same as addr_points)
        static_cast<size_t>(gb * 100000),    // admin_pairs (~15K polys × ~7 cells/poly)
    };
}

// --- Main ---

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: build-index <output-dir> <input.osm.pbf> [input2.osm.pbf ...] [--street-level N] [--admin-level N] [--max-vertices N] [--verbose] [--debug] [--in-memory] [--tmpdir DIR]" << std::endl;
        return 1;
    }

    std::string output_dir = argv[1];
    std::vector<std::string> input_files;
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--street-level" && i + 1 < argc) {
            kStreetCellLevel = std::atoi(argv[++i]);
        } else if (arg == "--admin-level" && i + 1 < argc) {
            kAdminCellLevel = std::atoi(argv[++i]);
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--debug") {
            debug = true;
            verbose = true;
        } else if (arg == "--in-memory") {
            in_memory = true;
        } else if (arg == "--max-vertices" && i + 1 < argc) {
            kMaxVertices = std::min(size_t(65535), size_t(std::atoi(argv[++i])));
        } else if (arg == "--tmpdir" && i + 1 < argc) {
            tmp_dir = argv[++i];
        } else {
            input_files.push_back(arg);
        }
    }

    // Pre-allocate vectors based on total PBF file size
    size_t total_pbf_bytes = 0;
    for (const auto& input_file : input_files) {
        struct stat st;
        if (stat(input_file.c_str(), &st) == 0) {
            total_pbf_bytes += st.st_size;
        }
    }
    if (total_pbf_bytes > 0) {
        auto est = estimate_from_file_size(total_pbf_bytes);
        ways.reserve(est.ways);
        addr_points.reserve(est.addr_points);
        street_nodes.reserve(est.street_nodes);
        admin_polygons.reserve(est.admin_polygons);
        admin_vertices.reserve(est.admin_vertices);
        way_pairs.reserve(est.way_pairs);
        addr_pairs.reserve(est.addr_pairs);
        admin_pairs.reserve(est.admin_pairs);
        if (verbose) {
            double gb = total_pbf_bytes / (1024.0 * 1024.0 * 1024.0);
            std::cerr << "Pre-allocated for " << std::fixed << std::setprecision(1)
                      << gb << " GB of PBF data" << std::endl;
        }
    }

    BuildHandler handler;
    auto total_start = Clock::now();

    for (const auto& input_file : input_files) {
        std::cerr << "Processing " << input_file << "..." << std::endl;
        auto file_start = Clock::now();

        // --- Pass 1: collect relation members for multipolygon assembly ---
        std::cerr << "  Pass 1: scanning relations..." << std::endl;
        auto pass1_start = Clock::now();

        osmium::area::Assembler::config_type assembler_config;
        assembler_config.ignore_invalid_locations = true;
        osmium::area::ProblemReporterStream problem_reporter{std::cerr};
        if (debug) {
            assembler_config.problem_reporter = &problem_reporter;
        }
        osmium::area::MultipolygonManager<osmium::area::Assembler> mp_manager{assembler_config};
        RelationWayCollector way_collector;

        {
            osmium::io::Reader reader1{input_file, osmium::osm_entity_bits::relation, osmium::io::read_meta::no};
            osmium::apply(reader1, mp_manager, way_collector);
            reader1.close();
            mp_manager.prepare_for_lookup();
        }
        auto pass1_secs = Duration(Clock::now() - pass1_start).count();
        std::cerr << "  Pass 1 done (" << format_duration(pass1_secs)
                  << ", " << way_collector.size() << " admin/postal way members)" << std::endl;

        // --- Pass 1.5: scan ways to collect needed node IDs ---
        std::cerr << "  Pass 1.5: scanning ways for needed node IDs..." << std::endl;
        auto pass15_start = Clock::now();
        NodeBitmap needed_nodes;
        {
            WayNodeCollector node_collector{needed_nodes, way_collector.admin_way_ids()};
            osmium::io::Reader reader15{input_file, osmium::osm_entity_bits::way, osmium::io::read_meta::no};
            osmium::apply(reader15, node_collector);
            reader15.close();
            std::cerr << "    " << node_collector.collected_ways() << " ways collected" << std::endl;
        }
        auto pass15_secs = Duration(Clock::now() - pass15_start).count();
        uint64_t needed_count = needed_nodes.count();
        std::cerr << "  Pass 1.5 done (" << format_duration(pass15_secs)
                  << ", " << needed_count << " needed nodes, bitmap "
                  << needed_nodes.memory_mb() << " MB)" << std::endl;
        log_mem("after pass 1.5");

        // --- Pass 2: process all data with filtered node locations ---
        std::cerr << "  Pass 2: processing nodes, ways, and areas..." << std::endl;
        auto pass2_start = Clock::now();

        if (in_memory) {
            using index_type = osmium::index::map::SparseMemArray<
                osmium::unsigned_object_id_type, osmium::Location>;

            index_type index;
            FilteredNodeLocationsForWays<index_type> location_handler{index, needed_nodes};
            location_handler.ignore_errors();

            osmium::io::Reader reader2{input_file, osmium::io::read_meta::no};
            osmium::apply(reader2, location_handler, handler, mp_manager.handler([&handler](osmium::memory::Buffer&& buffer) {
                osmium::apply(buffer, handler);
            }));
            reader2.close();

            std::cerr << "    Node locations: " << location_handler.stored() << " stored, "
                      << location_handler.skipped() << " skipped" << std::endl;
        } else {
            using index_type = osmium::index::map::SparseFileArray<
                osmium::unsigned_object_id_type, osmium::Location>;

            std::string tmp_path = (tmp_dir.empty() ? output_dir : tmp_dir) + "/node_locations.tmp";
            int fd = open(tmp_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
            index_type index{fd};
            FilteredNodeLocationsForWays<index_type> location_handler{index, needed_nodes};
            location_handler.ignore_errors();

            osmium::io::Reader reader2{input_file, osmium::io::read_meta::no};
            osmium::apply(reader2, location_handler, handler, mp_manager.handler([&handler](osmium::memory::Buffer&& buffer) {
                osmium::apply(buffer, handler);
            }));
            reader2.close();
            close(fd);
            std::remove(tmp_path.c_str());

            std::cerr << "    Node locations: " << location_handler.stored() << " stored, "
                      << location_handler.skipped() << " skipped" << std::endl;
        }
        needed_nodes.clear();
        malloc_trim(0);
        auto pass2_secs = Duration(Clock::now() - pass2_start).count();
        auto file_secs = Duration(Clock::now() - file_start).count();
        std::cerr << "  Pass 2 done (" << format_duration(pass2_secs) << ", file total: " << format_duration(file_secs) << ")" << std::endl;
    }

    auto read_secs = Duration(Clock::now() - total_start).count();
    std::cerr << "Done reading (" << format_duration(read_secs) << "):" << std::endl;
    std::cerr << "  " << handler.way_count() << " street ways" << std::endl;
    std::cerr << "  " << addr_count_total << " address points ("
              << handler.building_addr_count() << " from buildings)" << std::endl;
    std::cerr << "  " << handler.interp_count() << " interpolation ways" << std::endl;
    std::cerr << "  " << handler.admin_count() << " admin/postcode boundaries ("
              << admin_polygons.size() << " polygon rings)" << std::endl;

    std::cerr << "Admin polygon pipeline:" << std::endl;
    std::cerr << "  " << admin_stored << " polygon rings stored" << std::endl;
    std::cerr << "  " << admin_s2_direct << " S2 valid (direct)" << std::endl;
    std::cerr << "  " << admin_s2_repaired << " S2 repaired (S2Builder)" << std::endl;
    std::cerr << "  " << admin_bbox_fallback << " bbox fallback (S2 + S2Builder both failed)" << std::endl;
    std::cerr << "  " << admin_simplified_dropped << " dropped by simplification (<3 vertices)" << std::endl;
    std::cerr << "  " << admin_too_few_points << " dropped by cover_polygon (<3 unique S2 points)" << std::endl;
    if (admin_invalid_nodes_skipped > 0) {
        std::cerr << "  " << admin_invalid_nodes_skipped << " total invalid nodes skipped in boundaries" << std::endl;
    }

    auto resolve_start = Clock::now();
    std::cerr << "Resolving interpolation endpoints..." << std::endl;
    resolve_interpolation_endpoints();
    auto resolve_secs = Duration(Clock::now() - resolve_start).count();

    auto dedup_start = Clock::now();
    std::cerr << "Sorting and deduplicating flat pair vectors..." << std::endl;
    log_mem("before sort+dedup");
    std::cerr << "  way_pairs (" << way_pairs.size() << " pairs)..." << std::endl;
    sort_and_dedup(way_pairs);
    std::cerr << "  way_pairs deduped to " << way_pairs.size() << " (" << count_unique_cells(way_pairs) << " cells)" << std::endl;
    std::cerr << "  addr_pairs (" << addr_pairs.size() << " pairs)..." << std::endl;
    sort_and_dedup(addr_pairs);
    std::cerr << "  addr_pairs deduped to " << addr_pairs.size() << " (" << count_unique_cells(addr_pairs) << " cells)" << std::endl;
    std::cerr << "  interp_pairs (" << interp_pairs.size() << " pairs)..." << std::endl;
    sort_and_dedup(interp_pairs);
    std::cerr << "  interp_pairs deduped to " << interp_pairs.size() << " (" << count_unique_cells(interp_pairs) << " cells)" << std::endl;
    std::cerr << "  admin_pairs (" << admin_pairs.size() << " pairs)..." << std::endl;
    sort_and_dedup(admin_pairs);
    std::cerr << "  admin_pairs deduped to " << admin_pairs.size() << " (" << count_unique_cells(admin_pairs) << " cells)" << std::endl;
    log_mem("after sort+dedup");
    auto dedup_secs = Duration(Clock::now() - dedup_start).count();
    std::cerr << "Sort+dedup done (" << format_duration(dedup_secs) << ")" << std::endl;

    size_t admin_polygon_count = admin_polygons.size();

    auto write_start = Clock::now();
    std::cerr << "Writing index files to " << output_dir << "..." << std::endl;
    write_index(output_dir);
    auto write_secs = Duration(Clock::now() - write_start).count();

    auto total_secs = Duration(Clock::now() - total_start).count();
    std::cerr << std::endl;
    std::cerr << "Build complete in " << format_duration(total_secs) << std::endl;
    std::cerr << "  Reading:       " << format_duration(read_secs) << std::endl;
    std::cerr << "  Resolve:       " << format_duration(resolve_secs) << std::endl;
    std::cerr << "  Dedup:         " << format_duration(dedup_secs) << std::endl;
    std::cerr << "  Write:         " << format_duration(write_secs) << std::endl;
    std::cerr << "  Throughput:    " << format_rate(handler.way_count(), read_secs) << " ways, "
              << format_rate(addr_count_total, read_secs) << " addrs, "
              << format_rate(admin_polygon_count, read_secs) << " polygons" << std::endl;
    return 0;
}

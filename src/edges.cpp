#include "edges.h"

// Lines across flat surfaces come three ways from a test of two faces at more than 30 degrees, or one face:
//
// - A surface written twice, once each way up, as Civil 3D and some families write paths and slabs: an edge on
//   it has faces facing both ways, so they seem to fold right back, or it has four.
// - A sliver, a triangle so thin its normal is noise, beside a flat face.
// - A T-junction, a seam or an overlap: a face's edge along the edges of two smaller faces, or along or over a
//   face of another part of the mesh, so it has no face across it in the mesh's own terms.

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace ifcfilter {
namespace {

constexpr double FLAT_SINE = 0.5;        // flat within the 30 degrees an edge is drawn at, as its sine
constexpr double FLAT_M = 0.001;         // or within a millimetre, for slivers
constexpr double PLANE_COSINE = 0.8660254;  // normals in one plane, which way each faces aside

using V = EdgePoint;
V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V& a, const V& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double length(const V& a) { return std::sqrt(dot(a, a)); }
V scaled(const V& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }

// A face along an edge: its normal, its area, and its corner across from the edge
struct Face {
    V normal;
    double area;
    V across;
};

struct Cell {
    int64_t x, y, z;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct CellHash {
    size_t operator()(const Cell& c) const { return std::hash<int64_t>()(c.x * 73856093 ^ c.y * 19349663 ^ c.z * 83492791); }
};

}  // namespace

std::vector<std::pair<uint32_t, uint32_t>> flat_edges_out(const std::vector<EdgePoint>& points, const std::vector<uint32_t>& indices,
                                                           const std::vector<std::pair<uint32_t, uint32_t>>& edges) {
    // Points in the same place are one, to 0.01 mm
    std::vector<uint32_t> canonical(points.size());
    {
        std::map<std::array<int64_t, 3>, uint32_t> at;
        for (uint32_t v = 0; v < points.size(); ++v) {
            const std::array<int64_t, 3> key{std::llround(points[v][0] * 1e5), std::llround(points[v][1] * 1e5), std::llround(points[v][2] * 1e5)};
            canonical[v] = at.emplace(key, v).first->second;
        }
    }
    const auto key = [&](uint32_t a, uint32_t b) {
        a = canonical[a], b = canonical[b];
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    };
    std::unordered_set<uint64_t> wanted;
    for (const auto& [a, b] : edges) {
        if (a >= points.size() || b >= points.size()) return edges;
        wanted.insert(key(a, b));
    }
    std::unordered_map<uint64_t, std::vector<Face>> faces;
    std::vector<std::pair<std::array<V, 3>, V>> triangles;  // each face's corners and normal
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const uint32_t v[3] = {indices[t], indices[t + 1], indices[t + 2]};
        if (v[0] >= points.size() || v[1] >= points.size() || v[2] >= points.size()) return edges;
        const V n = cross(sub(points[v[1]], points[v[0]]), sub(points[v[2]], points[v[0]]));
        const double twice = length(n);
        if (twice < 1e-12) continue;
        const V normal = scaled(n, 1 / twice);
        triangles.push_back({{points[v[0]], points[v[1]], points[v[2]]}, normal});
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = v[k], b = v[(k + 1) % 3], c = v[(k + 2) % 3];
            const uint64_t edge = key(a, b);
            if (canonical[a] != canonical[b] && wanted.count(edge)) faces[edge].push_back({normal, twice / 2, points[c]});
        }
    }

    // Each edge: on a flat surface (dropped), with flat surface on one side only (a seam if there's surface
    // just past it), or neither (kept)
    enum class Kind { flat, one_sided, kept };
    struct Sort {
        Kind kind = Kind::kept;
        V normal{}, side{};  // one-sided: its plane, and across the edge towards its surface
    };
    std::vector<Sort> sorts(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        const V& pa = points[edges[i].first];
        const V along = sub(points[edges[i].second], pa);
        const double span = length(along);
        const auto found = faces.find(key(edges[i].first, edges[i].second));
        if (span < 1e-9 || found == faces.end() || found->second.empty()) continue;
        const V dir = scaled(along, 1 / span);
        const auto& list = found->second;
        const V n = std::max_element(list.begin(), list.end(), [](const Face& x, const Face& y) { return x.area < y.area; })->normal;
        const V across = cross(n, dir);
        bool left = false, right = false, fold = false;
        for (const Face& face : list) {
            const V d = sub(face.across, pa);
            const double height = std::abs(dot(d, n));
            const double off_line = length(sub(d, scaled(dir, dot(d, dir))));
            if (height > std::max(FLAT_M, off_line * FLAT_SINE)) {
                fold = true;
                break;
            }
            const double side = dot(d, across);
            if (side > FLAT_M) left = true;
            else if (side < -FLAT_M) right = true;
        }
        if (fold) continue;
        if (left && right) sorts[i].kind = Kind::flat;
        else if (left || right) sorts[i] = {Kind::one_sided, n, left ? across : scaled(across, -1)};
    }

    // Seams: a one-sided edge with surface in its plane just past it, a quarter, half and three quarters of
    // the way along. A grid of cells by the triangles' boxes finds the faces there.
    std::vector<bool> seam(edges.size(), false);
    if (std::any_of(sorts.begin(), sorts.end(), [](const Sort& s) { return s.kind == Kind::one_sided; })) {
        V lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        for (const V& p : points) {
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
        }
        const double cell = std::max(length(sub(hi, lo)) / 64, 0.05);
        const auto cell_of = [&](const V& p) { return Cell{static_cast<int64_t>(std::floor(p[0] / cell)), static_cast<int64_t>(std::floor(p[1] / cell)), static_cast<int64_t>(std::floor(p[2] / cell))}; };
        std::unordered_map<Cell, std::vector<size_t>, CellHash> grid;
        std::vector<size_t> wide;  // triangles across too many cells, looked at for every probe
        for (size_t t = 0; t < triangles.size(); ++t) {
            const auto& corners = triangles[t].first;
            Cell l = cell_of(corners[0]), h = l;
            for (int c = 1; c < 3; ++c) {
                const Cell at = cell_of(corners[c]);
                l = {std::min(l.x, at.x), std::min(l.y, at.y), std::min(l.z, at.z)};
                h = {std::max(h.x, at.x), std::max(h.y, at.y), std::max(h.z, at.z)};
            }
            if ((h.x - l.x + 1) * (h.y - l.y + 1) * (h.z - l.z + 1) > 4096) {
                wide.push_back(t);
                continue;
            }
            for (int64_t x = l.x; x <= h.x; ++x)
                for (int64_t y = l.y; y <= h.y; ++y)
                    for (int64_t z = l.z; z <= h.z; ++z) grid[{x, y, z}].push_back(t);
        }
        constexpr double REACH = 2 * FLAT_M;  // past it, not across a real gap
        const auto lies_in = [&](size_t t, const V& p, const V& n) {
            const auto& [c, m] = triangles[t];
            if (std::abs(dot(n, m)) < PLANE_COSINE || std::abs(dot(sub(p, c[0]), m)) > std::max(FLAT_M, REACH * FLAT_SINE)) return false;
            for (int k = 0; k < 3; ++k) {
                const V& x = c[k];
                const V& y = c[(k + 1) % 3];
                if (dot(cross(sub(y, x), sub(p, x)), m) < -FLAT_M * length(sub(y, x))) return false;
            }
            return true;
        };
        const auto on_surface = [&](const V& p, const V& n) {
            const auto found = grid.find(cell_of(p));
            if (found != grid.end()) {
                for (const size_t t : found->second)
                    if (lies_in(t, p, n)) return true;
            }
            for (const size_t t : wide)
                if (lies_in(t, p, n)) return true;
            return false;
        };
        for (size_t i = 0; i < edges.size(); ++i) {
            if (sorts[i].kind != Kind::one_sided) continue;
            const V& pa = points[edges[i].first];
            const V& pb = points[edges[i].second];
            bool all = true;
            for (const double t : {0.25, 0.5, 0.75}) {
                V probe;
                for (int k = 0; k < 3; ++k) probe[k] = pa[k] + (pb[k] - pa[k]) * t - sorts[i].side[k] * REACH;
                if (!on_surface(probe, sorts[i].normal)) {
                    all = false;
                    break;
                }
            }
            seam[i] = all;
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> kept;
    kept.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        if (sorts[i].kind != Kind::flat && !seam[i]) kept.push_back(edges[i]);
    }
    return kept;
}

}  // namespace ifcfilter

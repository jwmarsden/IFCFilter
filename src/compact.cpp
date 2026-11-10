#include "compact.h"

#include "extent.h"
#include "spf.h"
#include "system.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifcfilter {
namespace {

constexpr size_t MAX_POLYGON = 4096;  // ear clipping is slow past this, and a set this odd can stay as it is

std::vector<uint32_t> integers_in(std::string_view text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        uint32_t value = 0;
        for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) value = value * 10 + (text[i] - '0');
        out.push_back(value);
    }
    return out;
}

using Point = std::array<double, 3>;

std::vector<Point> points_in(std::string_view list) {
    std::vector<Point> out;
    int axis = 0;
    Point point{};
    for (const char* p = list.data(), *end = list.data() + list.size(); p < end;) {
        if (*p == '-' || *p == '+' || *p == '.' || std::isdigit(static_cast<unsigned char>(*p))) {
            char* after = nullptr;
            const double value = std::strtod(p, &after);
            if (after == p) { ++p; continue; }
            if (axis < 3) point[axis] = value;
            ++axis;
            p = after;
        } else {
            if (*p == ')' && axis > 0) {
                out.push_back(point);
                axis = 0;
            }
            ++p;
        }
    }
    return out;
}

// A coordinate as a whole number of rounding steps, from the same text the file gets, so a test on
// these sees just what's written
using Whole = std::array<int64_t, 3>;

int64_t whole_steps(double value, int places) {
    char buffer[64], digits[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", places, value);
    size_t length = 0;
    for (const char* p = buffer; *p; ++p) {
        if (*p != '.') digits[length++] = *p;
    }
    digits[length] = 0;
    return std::strtoll(digits, nullptr, 10);
}

// True if three points written to these steps make no area: two the same, or all in a line
bool no_area(const Whole& a, const Whole& b, const Whole& c) {
    const int64_t ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const int64_t vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    return uy * vz == uz * vy && uz * vx == ux * vz && ux * vy == uy * vx;
}

// Numbers in a point list at a fixed number of decimal places, as STEP writes reals: 12.5, 3., 0.
void append_rounded(std::string_view text, int places, std::string& out) {
    char buffer[64];
    bool in_string = false;
    for (const char* p = text.data(), *end = text.data() + text.size(); p < end;) {
        if (*p == '\'') in_string = !in_string;
        if (!in_string && (*p == '-' || *p == '+' || *p == '.' || std::isdigit(static_cast<unsigned char>(*p)))) {
            char* after = nullptr;
            const double value = std::strtod(p, &after);
            if (after == p) { out += *p++; continue; }
            int length = std::snprintf(buffer, sizeof buffer, "%.*f", places, value);
            if (places == 0) buffer[length++] = '.';
            else while (buffer[length - 1] == '0') --length;
            if (length == 3 && buffer[0] == '-' && buffer[1] == '0' && buffer[2] == '.') out += "0.";
            else out.append(buffer, length);
            p = after;
        } else {
            out += *p++;
        }
    }
}

// A face flattened along its normal, so that it runs counter-clockwise in (u, v)
struct Flat {
    int u = 0, v = 1;
    double flip = 1;
    bool has_area = false;

    Flat(const std::vector<uint32_t>& face, const std::vector<Point>& points) {
        Point normal{};
        for (size_t i = 0; i < face.size(); ++i) {
            const Point& a = points[face[i] - 1];
            const Point& b = points[face[(i + 1) % face.size()] - 1];
            normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
            normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
            normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
        }
        const double nx = std::abs(normal[0]), ny = std::abs(normal[1]), nz = std::abs(normal[2]);
        const int drop = nx >= ny && nx >= nz ? 0 : ny >= nz ? 1 : 2;
        has_area = normal[drop] != 0;
        u = (drop + 1) % 3;
        v = (drop + 2) % 3;
        flip = normal[drop] > 0 ? 1 : -1;
    }
};

bool in_range(const std::vector<uint32_t>& face, const std::vector<Point>& points) {
    return std::all_of(face.begin(), face.end(), [&](uint32_t i) { return i > 0 && i <= points.size(); });
}

// Joins a face's holes to its outline by a cut running out to it and back, making one ring. Each hole
// joins at its point furthest along u, to the first thing a ray from there hits: the end of that
// edge furthest along, or a vertex in the way that's closer to the ray.
bool bridge(std::vector<uint32_t>& ring, std::vector<std::vector<uint32_t>> holes, const std::vector<Point>& points) {
    if (!in_range(ring, points)) return false;
    const Flat flat(ring, points);
    if (!flat.has_area) return false;
    const auto x = [&](uint32_t i) { return points[i - 1][flat.u]; };
    const auto y = [&](uint32_t i) { return flat.flip * points[i - 1][flat.v]; };
    for (auto& hole : holes) {
        if (hole.size() < 3 || !in_range(hole, points)) return false;
        double area = 0;
        const double ox = x(hole[0]), oy = y(hole[0]);  // from its first point, as in triangulate
        for (size_t i = 0; i < hole.size(); ++i) {
            const uint32_t a = hole[i], b = hole[(i + 1) % hole.size()];
            area += (x(a) - ox) * (y(b) - oy) - (x(b) - ox) * (y(a) - oy);
        }
        if (area > 0) std::reverse(hole.begin(), hole.end());
    }
    const auto furthest = [&](const std::vector<uint32_t>& hole) {
        return static_cast<size_t>(std::max_element(hole.begin(), hole.end(), [&](uint32_t a, uint32_t b) { return x(a) < x(b); }) - hole.begin());
    };
    std::sort(holes.begin(), holes.end(), [&](const auto& a, const auto& b) { return x(a[furthest(a)]) > x(b[furthest(b)]); });

    for (const auto& hole : holes) {
        const size_t start = furthest(hole);
        const double mx = x(hole[start]), my = y(hole[start]);
        double hit = std::numeric_limits<double>::infinity();
        size_t join = ring.size();
        for (size_t i = 0; i < ring.size(); ++i) {
            const size_t next = (i + 1) % ring.size();
            const double ax = x(ring[i]), ay = y(ring[i]), bx = x(ring[next]), by = y(ring[next]);
            if ((ay <= my) == (by <= my)) continue;
            const double at = ax + (my - ay) / (by - ay) * (bx - ax);
            if (at >= mx && at < hit) {
                hit = at;
                join = ax > bx ? i : next;
            }
        }
        if (join == ring.size()) return false;
        // Anything inside the triangle from the hole to the hit to the join is in the way
        const double px = x(ring[join]), py = y(ring[join]);
        const auto side = [](double ax, double ay, double bx, double by, double cx, double cy) { return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax); };
        double best = std::abs(py - my) / std::max(px - mx, 1e-300);
        for (size_t i = 0; i < ring.size(); ++i) {
            const double qx = x(ring[i]), qy = y(ring[i]);
            if (i == join || qx <= mx || (qx == px && qy == py)) continue;
            const double s1 = side(mx, my, hit, my, qx, qy), s2 = side(hit, my, px, py, qx, qy), s3 = side(px, py, mx, my, qx, qy);
            const bool inside = (s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0);
            if (!inside) continue;
            const double slope = std::abs(qy - my) / (qx - mx);
            if (slope < best) {
                best = slope;
                join = i;
            }
        }
        std::vector<uint32_t> joined;
        joined.reserve(ring.size() + hole.size() + 2);
        joined.insert(joined.end(), ring.begin(), ring.begin() + static_cast<std::ptrdiff_t>(join) + 1);
        for (size_t i = 0; i <= hole.size(); ++i) joined.push_back(hole[(start + i) % hole.size()]);
        joined.insert(joined.end(), ring.begin() + static_cast<std::ptrdiff_t>(join), ring.end());
        ring.swap(joined);
    }
    return true;
}

// Ear clipping, projected along the face's normal. Vertices keep their order, so triangles face the
// way the polygon did. False if the polygon can't be cut into triangles covering the same area.
bool triangulate(const std::vector<uint32_t>& face, const std::vector<Point>& points, std::vector<std::array<uint32_t, 3>>& out) {
    const size_t n = face.size();
    if (!in_range(face, points)) return false;
    if (n < 3) return n == 0;
    if (n == 3) {
        out.push_back({face[0], face[1], face[2]});
        return true;
    }
    if (n > MAX_POLYGON) return false;

    const Flat plane(face, points);
    if (!plane.has_area) return true;  // no area at all, so nothing to draw
    const int u = plane.u, v = plane.v;
    const double flip = plane.flip;

    const auto x = [&](uint32_t i) { return points[i - 1][u]; };
    const auto y = [&](uint32_t i) { return points[i - 1][v]; };
    const auto cross = [&](uint32_t a, uint32_t b, uint32_t c) {
        return flip * ((x(b) - x(a)) * (y(c) - y(a)) - (y(b) - y(a)) * (x(c) - x(a)));
    };

    double x0 = x(face[0]), x1 = x0, y0 = y(face[0]), y1 = y0;
    // From the first point, not the origin: in world coordinates, such as E278706 N6124295, products of whole
    // coordinates lose more than a small face's area, and the check below then refused faces that were fine
    const double ox = x(face[0]), oy = y(face[0]);
    double area = 0;
    for (size_t i = 0; i < n; ++i) {
        x0 = std::min(x0, x(face[i]));
        x1 = std::max(x1, x(face[i]));
        y0 = std::min(y0, y(face[i]));
        y1 = std::max(y1, y(face[i]));
        const uint32_t a = face[i], b = face[(i + 1) % n];
        area += (x(a) - ox) * (y(b) - oy) - (x(b) - ox) * (y(a) - oy);
    }
    area = std::abs(area) / 2;
    const double extent = std::max(x1 - x0, y1 - y0);
    const double flat = extent * extent * 1e-13;

    const size_t first = out.size();
    std::vector<uint32_t> ring = face;
    double covered = 0;
    while (ring.size() > 2) {
        const size_t m = ring.size();
        // Strictly: nothing touches the ear, so vertices along an edge aren't skipped. Then loosely,
        // as a face drawn with a bridge to a hole runs along itself. Then a sliver with no area goes.
        bool cut = false;
        for (int pass = 0; pass < 3 && !cut; ++pass) {
            for (size_t i = 0; i < m && !cut; ++i) {
                const uint32_t a = ring[(i + m - 1) % m], b = ring[i], c = ring[(i + 1) % m];
                const double turn = cross(a, b, c);
                if (pass == 2) {
                    if (std::abs(turn) > flat) continue;
                    ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
                    cut = true;
                    break;
                }
                if (turn <= flat) continue;
                bool blocked = false;
                for (size_t j = 0; j < m && !blocked; ++j) {
                    const uint32_t p = ring[j];
                    if (p == a || p == b || p == c) continue;
                    const double ab = cross(a, b, p), bc = cross(b, c, p), ca = cross(c, a, p);
                    blocked = pass == 0 ? ab >= -flat && bc >= -flat && ca >= -flat : ab > flat && bc > flat && ca > flat;
                }
                if (blocked) continue;
                out.push_back({a, b, c});
                covered += turn / 2;
                ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
                cut = true;
            }
        }
        if (!cut) {
            out.resize(first);
            return false;
        }
    }
    if (std::abs(covered - area) > std::max(area * 1e-6, flat * static_cast<double>(n))) {
        out.resize(first);
        return false;
    }
    return true;
}

void append_list(const std::vector<uint32_t>& ids, std::string& out) {
    out += '(';
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ',';
        out += '#';
        out += std::to_string(ids[i]);
    }
    out += ')';
}

}  // namespace

CompactStats compact(const std::string& path, double precision_mm) {
    namespace fs = std::filesystem;
    CompactStats stats;
    stats.bytes_before = fs::file_size(path);
    const std::string temporary = path + ".compact";
    {
        const SpfFile spf(path);
        const uint32_t max_id = spf.max_id();

        std::vector<uint32_t> same_as(max_id, 0);  // written once, as this other instance
        std::vector<bool> face_gone(max_id, false);
        std::vector<uint32_t> values, sets, relationships, face_sets, triangle_sets, colour_maps;
        std::unordered_set<uint32_t> textured;  // their texture coordinates run by face, so the faces have to stay
        for (uint32_t id = 0; id < max_id; ++id) {
            if (spf.line(id).empty()) continue;
            ++stats.instances_before;
            const auto type = spf.type(id);
            if (type == "IFCPROPERTYSINGLEVALUE") values.push_back(id);
            else if (type == "IFCPROPERTYSET") sets.push_back(id);
            else if (type == "IFCRELDEFINESBYPROPERTIES") relationships.push_back(id);
            else if (type == "IFCPOLYGONALFACESET") face_sets.push_back(id);
            else if (type == "IFCTRIANGULATEDFACESET") triangle_sets.push_back(id);
            else if (type == "IFCINDEXEDCOLOURMAP") colour_maps.push_back(id);
            else if (type == "IFCINDEXEDPOLYGONALTEXTUREMAP" || type == "IFCINDEXEDTRIANGLETEXTUREMAP") {
                const auto attributes = attributes_of(spf.line(id));
                if (attributes.size() > 1) {
                    for (const uint32_t set : refs_in(attributes[1])) textured.insert(set);
                }
            }
        }

        // Face sets an opening cuts, or is made of, are left exactly as they are. Taking a solid from a mesh is
        // fragile: rounding or triangles were seen to change what IfcOpenShell cuts, and even to leave nothing.
        const auto cut = sets_openings_touch(spf);
        stats.sets_cut = cut.size();
        std::unordered_set<uint32_t> exact_points;
        for (const uint32_t id : cut) {
            const auto attributes = attributes_of(spf.line(id));
            if (attributes.empty()) continue;
            for (const uint32_t list : refs_in(attributes[0])) exact_points.insert(list);
        }

        std::unordered_map<std::string_view, uint32_t> first_value;
        first_value.reserve(values.size() / 8);
        for (const uint32_t id : values) {
            const auto line = spf.line(id);
            const auto [at, added] = first_value.emplace(line.substr(line.find('=')), id);
            if (!added) same_as[id] = at->second;
        }
        const auto canonical = [&](uint32_t id) { return id < max_id && same_as[id] ? same_as[id] : id; };

        // The same name and values, under the same owner, are the same set. Its GUID is the first one's.
        std::unordered_map<std::string, uint32_t> first_set;
        first_set.reserve(sets.size() / 2);
        std::string key;
        for (const uint32_t id : sets) {
            const auto attributes = attributes_of(spf.line(id));
            if (attributes.size() != 5) continue;
            auto properties = refs_in(attributes[4]);
            for (uint32_t& p : properties) p = canonical(p);
            std::sort(properties.begin(), properties.end());
            key.clear();
            key.append(attributes[1]).append(attributes[2]).append(attributes[3]);
            for (const uint32_t p : properties) key.append("#").append(std::to_string(p));
            const auto [at, added] = first_set.emplace(key, id);
            if (!added) same_as[id] = at->second;
        }

        // One relationship per set, listing everything the set is on
        std::unordered_map<std::string, uint32_t> first_relationship;
        std::unordered_map<uint32_t, std::vector<uint32_t>> related;
        for (const uint32_t id : relationships) {
            const auto attributes = attributes_of(spf.line(id));
            if (attributes.size() != 6 || attributes[5].empty() || attributes[5][0] != '#') continue;
            const auto set = refs_in(attributes[5]);
            if (set.size() != 1) continue;
            key.clear();
            key.append(attributes[1]).append(attributes[2]).append(attributes[3]).append("#").append(std::to_string(canonical(set[0])));
            const auto [at, added] = first_relationship.emplace(key, id);
            if (!added) same_as[id] = at->second;
            auto& objects = related[at->second];
            for (const uint32_t object : refs_in(attributes[4])) objects.push_back(object);
        }

        const double unit = length_unit(spf);
        stats.rounded = unit > 0;
        const int places = unit > 0 ? std::max(0, static_cast<int>(std::ceil(-std::log10(precision_mm * 1e-3 / unit) - 1e-9))) : 0;

        // A set's points as written; none if they aren't rounded or a face doesn't index them directly
        const auto points_as_written = [&](std::string_view coordinates, std::string_view pn_index) {
            std::vector<Whole> out;
            const auto list = refs_in(coordinates);
            if (!stats.rounded || pn_index != "$" || list.size() != 1) return out;
            const auto parts = attributes_of(spf.line(list[0]));
            if (parts.empty()) return out;
            for (const Point& p : points_in(parts[0])) out.push_back({whole_steps(p[0], places), whole_steps(p[1], places), whole_steps(p[2], places)});
            return out;
        };

        // Colour indices run by face, so they multiply with the triangles, and go with a face that's dropped
        std::unordered_map<uint32_t, uint32_t> colour_count;
        for (const uint32_t id : colour_maps) {
            const auto attributes = attributes_of(spf.line(id));
            if (attributes.size() != 4) continue;
            for (const uint32_t set : refs_in(attributes[0])) colour_count[set] = static_cast<uint32_t>(integers_in(attributes[3]).size());
        }

        std::unordered_map<uint32_t, std::string> converted;
        std::unordered_map<uint32_t, std::vector<uint32_t>> triangles_by_face;  // only where colours need it
        const auto append_triangles = [](const std::vector<std::array<uint32_t, 3>>& list, std::string& text) {
            text += '(';
            for (size_t i = 0; i < list.size(); ++i) {
                if (i) text += ',';
                text += '(';
                text += std::to_string(list[i][0]);
                text += ',';
                text += std::to_string(list[i][1]);
                text += ',';
                text += std::to_string(list[i][2]);
                text += ')';
            }
            text += ')';
        };
        // Takes out triangles with no area as written, and counts them off their faces. A set can't be
        // left with none, so if that's all it has they stay, and are counted as left.
        std::vector<std::array<uint32_t, 3>> kept;
        const auto drop_no_area = [&](std::vector<std::array<uint32_t, 3>>& list, std::vector<uint32_t>& by_face, const std::vector<Whole>& points) {
            if (points.empty()) return;
            const auto drawn = [&](const std::array<uint32_t, 3>& t) {
                return t[0] == 0 || t[1] == 0 || t[2] == 0 || t[0] > points.size() || t[1] > points.size() || t[2] > points.size() ||
                       !no_area(points[t[0] - 1], points[t[1] - 1], points[t[2] - 1]);
            };
            const auto counted = by_face;
            kept.clear();
            size_t at = 0;
            for (uint32_t& count : by_face) {
                uint32_t left = 0;
                for (uint32_t i = 0; i < count; ++i, ++at) {
                    if (!drawn(list[at])) continue;
                    kept.push_back(list[at]);
                    ++left;
                }
                count = left;
            }
            if (kept.empty()) {
                stats.no_area_left += list.size();
                by_face = counted;
                return;
            }
            stats.no_area_removed += list.size() - kept.size();
            list.swap(kept);
        };
        std::vector<bool> face_needed(max_id, false);
        std::vector<std::array<uint32_t, 3>> triangles;
        std::vector<uint32_t> counts;
        std::vector<std::vector<uint32_t>> faces;
        std::vector<std::vector<std::vector<uint32_t>>> voids;
        for (const uint32_t id : face_sets) {
            const auto attributes = attributes_of(spf.line(id));
            const auto face_ids = attributes.size() == 4 ? refs_in(attributes[2]) : std::vector<uint32_t>{};
            if (cut.count(id)) {
                for (const uint32_t face : face_ids) {
                    if (face < max_id) face_needed[face] = true;
                }
                continue;
            }
            bool usable = attributes.size() == 4 && !face_ids.empty() && !textured.count(id);
            const auto colours = colour_count.find(id);
            if (usable && colours != colour_count.end() && colours->second != face_ids.size()) usable = false;

            faces.clear();
            voids.clear();
            bool any_large = false;
            for (const uint32_t face : face_ids) {
                if (!usable) break;
                const auto type = spf.type(face);
                const auto parts = attributes_of(spf.line(face));
                if ((type != "IFCINDEXEDPOLYGONALFACE" && type != "IFCINDEXEDPOLYGONALFACEWITHVOIDS") || parts.empty()) {
                    usable = false;
                    break;
                }
                faces.push_back(integers_in(parts[0]));
                voids.emplace_back();
                if (type == "IFCINDEXEDPOLYGONALFACEWITHVOIDS" && parts.size() > 1 && parts[1].size() > 2) {
                    for (const auto loop : attributes_of(parts[1])) voids.back().push_back(integers_in(loop));
                }
                any_large = any_large || faces.back().size() > 3 || !voids.back().empty();
            }
            triangles.clear();
            counts.clear();
            if (usable) {
                const auto coordinates = refs_in(attributes[0]);
                std::vector<Point> points;
                // PnIndex would put another step between a face and its points
                const bool direct = attributes[3] == "$";
                if (any_large && (coordinates.size() != 1 || !direct)) usable = false;
                if (usable && any_large) {
                    const auto list = attributes_of(spf.line(coordinates[0]));
                    if (list.empty()) usable = false;
                    else points = points_in(list[0]);
                }
                for (size_t f = 0; f < faces.size(); ++f) {
                    if (!usable) break;
                    const size_t before = triangles.size();
                    if (!voids[f].empty() && !bridge(faces[f], voids[f], points)) usable = false;
                    else if (faces[f].size() == 3) triangles.push_back({faces[f][0], faces[f][1], faces[f][2]});
                    else if (!triangulate(faces[f], points, triangles)) usable = false;
                    counts.push_back(static_cast<uint32_t>(triangles.size() - before));
                }
            }
            if (triangles.empty()) usable = false;
            if (!usable) {
                ++stats.sets_left;
                stats.faces_left += face_ids.size();
                // It stays as polygons, but without faces that have no area
                const bool can_drop = attributes.size() == 4 && !textured.count(id) && (colours == colour_count.end() || colours->second == face_ids.size());
                const auto points = can_drop ? points_as_written(attributes[0], attributes[3]) : std::vector<Whole>{};
                std::vector<uint32_t> kept_faces;
                counts.clear();
                for (const uint32_t face : face_ids) {
                    bool drawn = points.empty();
                    if (!drawn) {
                        const auto parts = attributes_of(spf.line(face));
                        const auto outline = parts.empty() ? std::vector<uint32_t>{} : integers_in(parts[0]);
                        const bool in_list = !outline.empty() && std::all_of(outline.begin(), outline.end(), [&](uint32_t i) { return i > 0 && i <= points.size(); });
                        drawn = !in_list;
                        for (size_t i = 1; in_list && !drawn && i + 1 < outline.size(); ++i) {
                            drawn = !no_area(points[outline[0] - 1], points[outline[i] - 1], points[outline[i + 1] - 1]);
                        }
                    }
                    if (drawn) kept_faces.push_back(face);
                    counts.push_back(drawn ? 1 : 0);
                }
                if (kept_faces.empty()) {
                    stats.no_area_left += face_ids.size();
                    kept_faces = face_ids;
                } else if (kept_faces.size() < face_ids.size()) {
                    stats.no_area_removed += face_ids.size() - kept_faces.size();
                    for (const uint32_t face : face_ids) {
                        if (face < max_id) face_gone[face] = true;
                    }
                    if (colours != colour_count.end()) triangles_by_face[id] = counts;
                    std::string& text = converted[id];
                    text = "#" + std::to_string(id) + "=IFCPOLYGONALFACESET(";
                    text.append(attributes[0]).append(",").append(attributes[1]).append(",");
                    append_list(kept_faces, text);
                    text.append(",").append(attributes[3]).append(");");
                }
                for (const uint32_t face : kept_faces) {
                    if (face < max_id) face_needed[face] = true;
                }
                continue;
            }
            ++stats.sets_triangulated;
            for (const uint32_t face : face_ids) face_gone[face] = true;
            drop_no_area(triangles, counts, points_as_written(attributes[0], attributes[3]));
            if (colours != colour_count.end() && std::any_of(counts.begin(), counts.end(), [](uint32_t n) { return n != 1; })) triangles_by_face[id] = counts;
            std::string& text = converted[id];
            text = "#" + std::to_string(id) + "=IFCTRIANGULATEDFACESET(";
            text.append(attributes[0]).append(",$,").append(attributes[1]).append(",");
            append_triangles(triangles, text);
            text.append(",").append(attributes[3]).append(");");
        }

        // Sets that were triangles already. A texture map or colours that don't line up leave one as it is.
        for (const uint32_t id : triangle_sets) {
            const auto attributes = attributes_of(spf.line(id));
            if (attributes.size() < 4 || textured.count(id) || cut.count(id)) continue;
            const std::string_view pn_index = attributes.size() > 4 ? attributes[4] : std::string_view("$");
            const auto indices = integers_in(attributes[3]);
            if (indices.empty() || indices.size() % 3) continue;
            triangles.clear();
            for (size_t i = 0; i < indices.size(); i += 3) triangles.push_back({indices[i], indices[i + 1], indices[i + 2]});
            const auto colours = colour_count.find(id);
            if (colours != colour_count.end() && colours->second != triangles.size()) continue;
            counts.assign(triangles.size(), 1);
            const size_t before = triangles.size();
            drop_no_area(triangles, counts, points_as_written(attributes[0], pn_index));
            if (triangles.size() == before) continue;
            if (colours != colour_count.end()) triangles_by_face[id] = counts;
            std::string& text = converted[id];
            text = "#" + std::to_string(id) + "=IFCTRIANGULATEDFACESET(";
            text.append(attributes[0]).append(",").append(attributes[1]).append(",").append(attributes[2]).append(",");
            append_triangles(triangles, text);
            if (attributes.size() > 4) text.append(",").append(attributes[4]);
            text.append(");");
        }

        std::ofstream out(temporary, std::ios::binary);
        out << spf.header();
        std::string text;
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto line = spf.line(id);
            if (line.empty() || same_as[id]) continue;
            if (face_gone[id] && !face_needed[id]) continue;
            ++stats.instances_after;
            const auto done = converted.find(id);
            if (done != converted.end()) {
                out << done->second << '\n';
                continue;
            }
            const auto type = spf.type(id);
            if (type == "IFCCARTESIANPOINTLIST3D" && stats.rounded && !exact_points.count(id)) {
                text.clear();
                const auto open = line.find('(');
                text.append(line.substr(0, open));
                append_rounded(line.substr(open), places, text);
                out << text << '\n';
                continue;
            }
            if (type == "IFCINDEXEDCOLOURMAP") {
                const auto attributes = attributes_of(line);
                const auto sets_mapped = attributes.size() == 4 ? refs_in(attributes[0]) : std::vector<uint32_t>{};
                const auto expansion = sets_mapped.empty() ? triangles_by_face.end() : triangles_by_face.find(sets_mapped[0]);
                if (expansion != triangles_by_face.end()) {
                    const auto colours = integers_in(attributes[3]);
                    text.assign(line.substr(0, line.find('(') + 1));
                    text.append(attributes[0]).append(",").append(attributes[1]).append(",").append(attributes[2]).append(",(");
                    bool first = true;
                    for (size_t i = 0; i < colours.size(); ++i) {
                        for (uint32_t t = 0; t < expansion->second[i]; ++t) {
                            if (!first) text += ',';
                            first = false;
                            text += std::to_string(colours[i]);
                        }
                    }
                    text.append("));");
                    out << text << '\n';
                    continue;
                }
            }
            if (type == "IFCRELDEFINESBYPROPERTIES") {
                const auto group = related.find(id);
                if (group != related.end()) {
                    auto& objects = group->second;
                    std::sort(objects.begin(), objects.end());
                    objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
                    const auto attributes = attributes_of(line);
                    text.assign(line.substr(0, line.find('(') + 1));
                    text.append(attributes[0]).append(",").append(attributes[1]).append(",").append(attributes[2]).append(",").append(attributes[3]).append(",");
                    append_list(objects, text);
                    text.append(",#").append(std::to_string(canonical(refs_in(attributes[5])[0]))).append(");");
                    out << text << '\n';
                    continue;
                }
            }
            // Anything else keeps its text, with references to what's now written once pointed at the one that's left
            text.clear();
            text.reserve(line.size());
            const auto equals = line.find('=');
            text.append(line.substr(0, equals));
            bool in_string = false;
            for (size_t i = equals; i < line.size(); ++i) {
                const char c = line[i];
                if (c == '\'') in_string = !in_string;
                if (c != '#' || in_string) {
                    text += c;
                    continue;
                }
                uint32_t target = 0;
                size_t j = i + 1;
                for (; j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])); ++j) target = target * 10 + (line[j] - '0');
                text += '#';
                text += std::to_string(canonical(target));
                i = j - 1;
            }
            out << text << '\n';
        }
        out << "ENDSEC;\nEND-ISO-10303-21;\n";
    }
    replace_file(temporary, path);
    stats.bytes_after = fs::file_size(path);
    return stats;
}

}  // namespace ifcfilter

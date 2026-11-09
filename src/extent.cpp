#include "extent.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace ifcfilter {
namespace {

bool number_in(std::string_view text, double& out) {
    const std::string copy(text);
    char* end = nullptr;
    out = std::strtod(copy.c_str(), &end);
    return end != copy.c_str();
}

// The representations of a product's shape: its bodies, or all of them if it names none
std::vector<uint32_t> representations_of(const SpfFile& spf, std::string_view shape, bool bodies_only) {
    std::vector<uint32_t> all, bodies;
    const auto shapes = refs_in(shape);
    if (shapes.size() != 1) return all;
    const auto attributes = attributes_of(spf.line(shapes[0]));
    if (attributes.size() != 3) return all;
    for (const uint32_t representation : refs_in(attributes[2])) {
        const auto parts = attributes_of(spf.line(representation));
        if (parts.size() != 4) continue;
        all.push_back(representation);
        if (parts[1] == "'Body'") bodies.push_back(representation);
    }
    return bodies_only && !bodies.empty() ? bodies : all;
}

// Every face set an item is made of, through any mapping
void sets_under(const SpfFile& spf, uint32_t item, std::unordered_set<uint32_t>& out) {
    std::vector<uint32_t> stack{item};
    std::unordered_set<uint32_t> seen;
    while (!stack.empty()) {
        const uint32_t id = stack.back();
        stack.pop_back();
        if (id >= spf.max_id() || !seen.insert(id).second) continue;
        const auto type = spf.type(id);
        if (type == "IFCPOLYGONALFACESET" || type == "IFCTRIANGULATEDFACESET" || type == "IFCFACETEDBREP") out.insert(id);
        else if (type == "IFCPRODUCTDEFINITIONSHAPE" || type == "IFCSHAPEREPRESENTATION" || type == "IFCMAPPEDITEM" || type == "IFCREPRESENTATIONMAP") {
            for_each_ref(spf.line(id), [&](uint32_t next) { stack.push_back(next); });
        }
    }
}

}  // namespace

void Box::add(const Vector& p) {
    for (int i = 0; i < 3; ++i) {
        low[i] = std::min(low[i], p[i]);
        high[i] = std::max(high[i], p[i]);
    }
}

void Box::add(const Box& b) {
    if (b.empty()) return;
    add(b.low);
    add(b.high);
}

double gap_between(const Box& a, const Box& b) {
    double sum = 0;
    for (int i = 0; i < 3; ++i) {
        const double d = std::max({0.0, a.low[i] - b.high[i], b.low[i] - a.high[i]});
        sum += d * d;
    }
    return std::sqrt(sum);
}

std::optional<Rigid> Extents::placement(std::string_view reference_text) {
    const auto ids = refs_in(reference_text);
    if (ids.size() != 1) return std::nullopt;
    const auto seen = placements_.find(ids[0]);
    if (seen != placements_.end()) return seen->second;
    std::optional<Rigid> out;
    if (spf_.type(ids[0]) == "IFCLOCALPLACEMENT") {
        const auto attributes = attributes_of(spf_.line(ids[0]));
        if (attributes.size() == 2) {
            const auto axes = refs_in(attributes[1]);
            const auto local = axes.size() == 1 ? axis_placement(spf_, axes[0]) : std::nullopt;
            if (local) {
                if (attributes[0] == "$") out = local;
                else if (const auto parent = placement(attributes[0])) out = compose(*parent, *local);
            }
        }
    }
    placements_[ids[0]] = out;
    return out;
}

std::optional<Box> Extents::product(uint32_t product) {
    const auto attributes = attributes_of(spf_.line(product));
    if (attributes.size() < 7) return std::nullopt;
    const auto placed = placement(attributes[5]);
    if (!placed) return std::nullopt;
    Box box;
    for (const uint32_t representation : representations_of(spf_, attributes[6], true)) {
        for (const uint32_t next : refs_in(attributes_of(spf_.line(representation))[3])) {
            if (!item(next, *placed, box)) return std::nullopt;
        }
    }
    if (box.empty()) return std::nullopt;
    return box;
}

bool Extents::reach(uint32_t item, const Rigid& to_world, Box& box, int depth) {
    if (depth > 8 || item >= spf_.max_id() || spf_.line(item).empty()) return false;
    const auto type = spf_.type(item);
    const auto attributes = attributes_of(spf_.line(item));
    if (type == "IFCMAPPEDITEM") {
        const auto map = attributes.size() == 2 ? refs_in(attributes[0]) : std::vector<uint32_t>{};
        const auto target = attributes.size() == 2 ? refs_in(attributes[1]) : std::vector<uint32_t>{};
        if (map.size() != 1 || target.size() != 1) return false;
        const auto placed = operator_of(spf_, target[0]);
        const auto parts = attributes_of(spf_.line(map[0]));
        const auto origin_ids = parts.size() == 2 ? refs_in(parts[0]) : std::vector<uint32_t>{};
        const auto origin = origin_ids.size() == 1 ? axis_placement(spf_, origin_ids[0]) : std::nullopt;
        const auto representation = parts.size() == 2 ? refs_in(parts[1]) : std::vector<uint32_t>{};
        if (!placed || !origin || representation.size() != 1) return false;
        const Rigid inner = compose(to_world, compose(*placed, inverse(*origin)));
        const auto held = attributes_of(spf_.line(representation[0]));
        if (held.size() != 4) return false;
        for (const uint32_t next : refs_in(held[3])) {
            if (!reach(next, inner, box, depth + 1)) return false;
        }
        return true;
    }
    if (type == "IFCBOOLEANRESULT" || type == "IFCBOOLEANCLIPPINGRESULT") {
        // What's taken away or kept can't reach past the first operand
        const auto first = attributes.size() == 3 ? refs_in(attributes[1]) : std::vector<uint32_t>{};
        return first.size() == 1 && reach(first[0], to_world, box, depth + 1);
    }
    if (type == "IFCEXTRUDEDAREASOLID" || type == "IFCEXTRUDEDAREASOLIDTAPERED") {
        if (attributes.size() < 4) return false;
        const auto profile = refs_in(attributes[0]);
        double low_x, high_x, low_y, high_y, length;
        if (profile.size() != 1 || !profile_box(profile[0], low_x, high_x, low_y, high_y) || !number_in(attributes[3], length)) return false;
        if (type == "IFCEXTRUDEDAREASOLIDTAPERED") {
            double end_low_x, end_high_x, end_low_y, end_high_y;
            const auto end = attributes.size() > 4 ? refs_in(attributes[4]) : std::vector<uint32_t>{};
            if (end.size() != 1 || !profile_box(end[0], end_low_x, end_high_x, end_low_y, end_high_y)) return false;
            low_x = std::min(low_x, end_low_x), high_x = std::max(high_x, end_high_x);
            low_y = std::min(low_y, end_low_y), high_y = std::max(high_y, end_high_y);
        }
        Rigid position;
        const auto at = refs_in(attributes[1]);
        if (at.size() == 1) {
            const auto read = axis_placement(spf_, at[0]);
            if (!read) return false;
            position = *read;
        }
        const auto direction_id = refs_in(attributes[2]);
        if (direction_id.size() != 1) return false;
        const auto direction_parts = attributes_of(spf_.line(direction_id[0]));
        const auto direction = direction_parts.empty() ? std::nullopt : normalised(numbers_in(direction_parts[0]));
        if (!direction) return false;
        const Rigid placed = compose(to_world, position);
        for (const double x : {low_x, high_x}) {
            for (const double y : {low_y, high_y}) {
                box.add(applied(placed, {x, y, 0}));
                box.add(applied(placed, {x + (*direction)[0] * length, y + (*direction)[1] * length, (*direction)[2] * length}));
            }
        }
        return true;
    }
    if (type == "IFCSWEPTDISKSOLID" || type == "IFCSWEPTDISKSOLIDPOLYGONAL") {
        double radius;
        Box path;
        if (attributes.size() < 2 || !number_in(attributes[1], radius) || !points_under(item, to_world, path) || path.empty()) return false;
        box.add(Vector{path.low[0] - radius, path.low[1] - radius, path.low[2] - radius});
        box.add(Vector{path.high[0] + radius, path.high[1] + radius, path.high[2] + radius});
        return true;
    }
    // Shapes made of points, with no placements of their own inside them
    static const std::unordered_set<std::string_view> MESHES = {
        "IFCPOLYGONALFACESET", "IFCTRIANGULATEDFACESET", "IFCTRIANGULATEDIRREGULARNETWORK", "IFCFACETEDBREP", "IFCFACETEDBREPWITHVOIDS",
        "IFCFACEBASEDSURFACEMODEL", "IFCSHELLBASEDSURFACEMODEL", "IFCPOLYLINE", "IFCINDEXEDPOLYCURVE", "IFCCARTESIANPOINT"};
    if (MESHES.count(type)) {
        Box points;
        if (!points_under(item, to_world, points) || points.empty()) return false;
        box.add(points);
        return true;
    }
    return false;
}

// Every point an item is drawn through. Placements inside it, as a curve's, would make these wrong.
bool Extents::points_under(uint32_t item, const Rigid& to_world, Box& box) {
    std::vector<uint32_t> stack{item};
    std::unordered_set<uint32_t> seen;
    while (!stack.empty()) {
        const uint32_t id = stack.back();
        stack.pop_back();
        if (id >= spf_.max_id() || !seen.insert(id).second) continue;
        const auto type = spf_.type(id);
        const auto line = spf_.line(id);
        if (type == "IFCAXIS2PLACEMENT3D" || type == "IFCAXIS2PLACEMENT2D" || type == "IFCCIRCLE" || type == "IFCELLIPSE") return false;
        if (type == "IFCCARTESIANPOINT") {
            const auto parts = attributes_of(line);
            if (!parts.empty()) box.add(applied(to_world, numbers_in(parts[0])));
        } else if (type == "IFCCARTESIANPOINTLIST3D" || type == "IFCCARTESIANPOINTLIST2D") {
            const auto parts = attributes_of(line);
            if (parts.empty()) continue;
            for (const auto point : attributes_of(parts[0])) box.add(applied(to_world, numbers_in(point)));
        } else {
            for_each_ref(line, [&](uint32_t next) { stack.push_back(next); });
        }
    }
    return true;
}

// Where a profile reaches in its own plane: exactly for an outline of points, and a little over for one
// made of sizes, as a square as wide as its widest side
bool Extents::profile_box(uint32_t profile, double& low_x, double& high_x, double& low_y, double& high_y) {
    const auto type = spf_.type(profile);
    const auto attributes = attributes_of(spf_.line(profile));
    if (type == "IFCARBITRARYCLOSEDPROFILEDEF" || type == "IFCARBITRARYPROFILEDEFWITHVOIDS" || type == "IFCARBITRARYOPENPROFILEDEF") {
        const auto curve = attributes.size() >= 3 ? refs_in(attributes[2]) : std::vector<uint32_t>{};
        Box points;
        if (curve.size() != 1 || !points_under(curve[0], Rigid{}, points) || points.empty()) return false;
        low_x = points.low[0], high_x = points.high[0], low_y = points.low[1], high_y = points.high[1];
        return true;
    }
    if (attributes.size() < 4 || type.find("PROFILEDEF") == std::string_view::npos || type == "IFCDERIVEDPROFILEDEF" || type == "IFCCOMPOSITEPROFILEDEF") return false;
    Vector centre{0, 0, 0};
    const auto at = refs_in(attributes[2]);
    if (at.size() == 1) {
        const auto placement = attributes_of(spf_.line(at[0]));
        const auto location = placement.empty() ? std::vector<uint32_t>{} : refs_in(placement[0]);
        if (location.size() != 1) return false;
        const auto point = attributes_of(spf_.line(location[0]));
        if (point.empty()) return false;
        centre = numbers_in(point[0]);
    } else if (attributes[2] != "$") {
        return false;
    }
    double first = 0, second = 0, half = 0;
    if (!number_in(attributes[3], first)) return false;
    if (type == "IFCCIRCLEPROFILEDEF" || type == "IFCCIRCLEHOLLOWPROFILEDEF") half = first;
    else if (attributes.size() > 4 && number_in(attributes[4], second)) half = type == "IFCELLIPSEPROFILEDEF" ? std::max(first, second) : std::max(first, second) / 2;
    else return false;
    low_x = centre[0] - half, high_x = centre[0] + half, low_y = centre[1] - half, high_y = centre[1] + half;
    return true;
}

std::unordered_set<uint32_t> sets_openings_touch(const SpfFile& spf) {
    std::unordered_set<uint32_t> sets;
    // Every shape of an element an opening cuts, not only those the opening reaches: a viewer cuts an element's
    // shapes together, and on a merged gantry, whose parts are each a few shapes, rounding the shapes an opening
    // doesn't reach was seen to change how much it takes from those it does, by up to 55 litres
    std::unordered_set<uint32_t> cut;
    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        if (spf.line(id).empty() || spf.type(id) != "IFCRELVOIDSELEMENT") continue;
        const auto relationship = attributes_of(spf.line(id));
        if (relationship.size() < 6) continue;
        for (const uint32_t opening : refs_in(relationship[5])) {
            const auto attributes = attributes_of(spf.line(opening));
            if (attributes.size() >= 7) {
                for (const uint32_t representation : representations_of(spf, attributes[6], false)) sets_under(spf, representation, sets);
            }
        }
        for (const uint32_t element : refs_in(relationship[4])) cut.insert(element);
    }
    for (const uint32_t element : cut) {
        const auto attributes = attributes_of(spf.line(element));
        if (attributes.size() < 7) continue;
        for (const uint32_t representation : representations_of(spf, attributes[6], false)) sets_under(spf, representation, sets);
    }
    return sets;
}

}  // namespace ifcfilter

#include "signs.h"

#include "extent.h"
#include "rigid.h"
#include "spf.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace ifcfilter {
namespace {

std::string quoted_in(std::string_view value) {
    const auto open = value.find('\'');
    const auto close = value.rfind('\'');
    return open == std::string_view::npos || close <= open ? std::string() : std::string(value.substr(open + 1, close - open - 1));
}

// The points of a face set, as given, in its own coordinates
std::vector<Vector> points_of(const SpfFile& spf, uint32_t item) {
    std::vector<Vector> out;
    const auto type = spf.type(item);
    if (type != "IFCTRIANGULATEDFACESET" && type != "IFCPOLYGONALFACESET") return out;
    const auto attributes = attributes_of(spf.line(item));
    const auto list = attributes.empty() ? std::vector<uint32_t>{} : refs_in(attributes[0]);
    if (list.size() != 1 || spf.type(list[0]) != "IFCCARTESIANPOINTLIST3D") return out;
    const auto coordinates = attributes_of(spf.line(list[0]));
    if (coordinates.empty()) return out;
    const std::string text(coordinates[0]);
    std::vector<double> numbers;
    const char* at = text.c_str();
    while (*at) {
        if ((*at >= '0' && *at <= '9') || *at == '-' || *at == '.') {
            char* end = nullptr;
            numbers.push_back(std::strtod(at, &end));
            at = end > at ? end : at + 1;
        } else {
            ++at;
        }
    }
    for (size_t i = 0; i + 2 < numbers.size(); i += 3) out.push_back({numbers[i], numbers[i + 1], numbers[i + 2]});
    return out;
}

}  // namespace

Forward move_forward(const SpfFile& spf, const std::vector<ForwardRule>& rules, const std::string& path) {
    Forward forward;
    if (rules.empty()) return forward;
    const double metres = length_unit(spf);
    if (metres <= 0) return forward;

    // The assemblies picked, and how far each goes
    std::unordered_map<uint32_t, double> picked;
    std::unordered_map<uint32_t, std::vector<uint32_t>> parts_of;
    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        const auto type = spf.type(id);
        if (type == "IFCRELAGGREGATES") {
            const auto attributes = attributes_of(spf.line(id));
            if (attributes.size() != 6) continue;
            const auto whole = refs_in(attributes[4]);
            if (whole.size() == 1) parts_of[whole[0]] = refs_in(attributes[5]);
            continue;
        }
        if (type != "IFCRELDEFINESBYPROPERTIES") continue;
        const auto attributes = attributes_of(spf.line(id));
        if (attributes.size() != 6) continue;
        const auto set = refs_in(attributes[5]);
        if (set.size() != 1 || spf.type(set[0]) != "IFCPROPERTYSET") continue;
        const auto set_attributes = attributes_of(spf.line(set[0]));
        if (set_attributes.size() != 5) continue;
        const std::string name = quoted_in(set_attributes[2]);
        for (const ForwardRule& rule : rules) {
            if (rule.set != name) continue;
            for (const uint32_t property : refs_in(set_attributes[4])) {
                const auto parts = attributes_of(spf.line(property));
                if (parts.size() < 3 || quoted_in(parts[0]) != rule.property || quoted_in(parts[2]) != rule.value) continue;
                for (const uint32_t object : refs_in(attributes[4])) picked[object] = rule.millimetres / 1000 / metres;
            }
        }
    }

    Extents extents(spf);
    // What has a placement and a shape: products, found once, when a face needs them
    std::vector<uint32_t> products_found;
    bool products_read = false;
    const auto products = [&]() -> const std::vector<uint32_t>& {
        if (products_read) return products_found;
        products_read = true;
        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            if (spf.line(id).empty() || spf.type(id).rfind("IFCREL", 0) == 0) continue;
            const auto attributes = attributes_of(spf.line(id));
            if (attributes.size() < 7) continue;
            const auto placement = refs_in(attributes[5]);
            const auto shape = refs_in(attributes[6]);
            if (placement.size() == 1 && shape.size() == 1 && spf.type(placement[0]) == "IFCLOCALPLACEMENT" && spf.type(shape[0]) == "IFCPRODUCTDEFINITIONSHAPE") {
                products_found.push_back(id);
            }
        }
        return products_found;
    };
    uint32_t next = spf.max_id();
    std::vector<std::string> added;
    std::unordered_map<uint32_t, std::string> replaced;
    for (const auto& [assembly, distance] : picked) {
        const auto parts = parts_of.find(assembly);
        if (parts == parts_of.end() || parts->second.empty()) continue;

        // Each part's points in the world, and its placement
        struct Part {
            uint32_t id;
            std::string placement;
            std::optional<Rigid> place;
            std::vector<Vector> points;
        };
        std::vector<Part> all;
        for (const uint32_t id : parts->second) {
            const auto attributes = attributes_of(spf.line(id));
            if (attributes.size() < 7) continue;
            Part part{id, std::string(attributes[5]), extents.placement(attributes[5]), {}};
            if (!part.place) continue;
            for (const uint32_t shape : refs_in(attributes[6])) {
                const auto representations = attributes_of(spf.line(shape));
                if (representations.size() != 3) continue;
                for (const uint32_t representation : refs_in(representations[2])) {
                    const auto items = attributes_of(spf.line(representation));
                    if (items.size() != 4) continue;
                    for (const uint32_t item : refs_in(items[3])) {
                        for (const Vector& p : points_of(spf, item)) part.points.push_back(applied(*part.place, p));
                    }
                }
            }
            all.push_back(std::move(part));
        }

        // The plate is the biggest part; its plane, as a sign stands upright, is found from where its points are across
        const auto spread = [](const std::vector<Vector>& points) {
            double low[3] = {1e300, 1e300, 1e300}, high[3] = {-1e300, -1e300, -1e300};
            for (const Vector& p : points) {
                for (int a = 0; a < 3; ++a) low[a] = std::min(low[a], p[a]), high[a] = std::max(high[a], p[a]);
            }
            return points.empty() ? 0.0 : std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
        };
        const auto plate = std::max_element(all.begin(), all.end(), [&](const Part& a, const Part& b) { return spread(a.points) < spread(b.points); });
        if (plate == all.end() || plate->points.size() < 3) {
            ++forward.unsure;
            continue;
        }
        double cx = 0, cy = 0;
        for (const Vector& p : plate->points) cx += p[0], cy += p[1];
        cx /= plate->points.size(), cy /= plate->points.size();
        double xx = 0, xy = 0, yy = 0;
        for (const Vector& p : plate->points) {
            xx += (p[0] - cx) * (p[0] - cx), xy += (p[0] - cx) * (p[1] - cy), yy += (p[1] - cy) * (p[1] - cy);
        }
        const double angle = 0.5 * std::atan2(2 * xy, xx - yy);  // along the plate
        Vector normal{-std::sin(angle), std::cos(angle), 0};

        // Forward is where the legend is: how far the other parts' points are in front of the plate's middle
        double plate_middle = 0;
        for (const Vector& p : plate->points) plate_middle += dot(p, normal);
        plate_middle /= plate->points.size();
        double legend = 0;
        size_t count = 0;
        for (const Part& part : all) {
            if (&part == &*plate) continue;
            for (const Vector& p : part.points) legend += dot(p, normal) - plate_middle, ++count;
        }
        const double clear = 0.0005 / metres;  // half a millimetre: a legend in the plate's plane doesn't say
        if (count && std::abs(legend / count) >= clear) {
            if (legend < 0) normal = {-normal[0], -normal[1], 0};
        } else {
            // Forward is away from what it's on, such as its post: what touches it, whatever its site ID
            Box face;
            for (const Part& part : all) {
                for (const Vector& p : part.points) face.add(p);
            }
            const double grow = 0.05 / metres;
            for (int a = 0; a < 3; ++a) face.low[a] -= grow, face.high[a] += grow;
            const std::unordered_set<uint32_t> own(parts->second.begin(), parts->second.end());
            double behind = 0;
            for (const uint32_t product : products()) {
                if (own.count(product) || product == assembly) continue;
                const auto box = extents.product(product);
                if (!box || box->empty() || gap_between(*box, face) > 0) continue;
                Vector middle;
                for (int a = 0; a < 3; ++a) middle[a] = (box->low[a] + box->high[a]) / 2;
                behind += dot(middle, normal) - plate_middle;
            }
            if (std::abs(behind) < clear) {
                ++forward.unsure;
                continue;
            }
            if (behind > 0) normal = {-normal[0], -normal[1], 0};
        }

        // One placement for all its parts, from where they were, moved forward: they share the model's origin's
        const std::string& from = all.front().placement;
        const auto relative = refs_in(from);
        if (relative.size() != 1 || std::any_of(all.begin(), all.end(), [&](const Part& p) { return p.placement != from; })) {
            ++forward.unsure;
            continue;
        }
        const Rigid& place = *all.front().place;
        const Vector move{normal[0] * distance, normal[1] * distance, 0};
        Vector local;
        for (int a = 0; a < 3; ++a) local[a] = dot(place.axes[a], move);
        char text[160];
        std::snprintf(text, sizeof text, "IFCCARTESIANPOINT((%.9g,%.9g,%.9g));", local[0], local[1], local[2]);
        const uint32_t point = next++;
        added.push_back("#" + std::to_string(point) + "=" + text);
        const uint32_t axes = next++;
        added.push_back("#" + std::to_string(axes) + "=IFCAXIS2PLACEMENT3D(#" + std::to_string(point) + ",$,$);");
        const uint32_t placement = next++;
        added.push_back("#" + std::to_string(placement) + "=IFCLOCALPLACEMENT(#" + std::to_string(relative[0]) + ",#" + std::to_string(axes) + ");");
        for (const Part& part : all) {
            auto attributes = attributes_of(spf.line(part.id));
            const auto line = spf.line(part.id);
            std::string rewritten(line.substr(0, line.find('(') + 1));
            for (size_t i = 0; i < attributes.size(); ++i) rewritten.append(i ? "," : "").append(i == 5 ? "#" + std::to_string(placement) : std::string(attributes[i]));
            rewritten += ");";
            replaced[part.id] = rewritten;
        }
        ++forward.faces;
    }
    if (!forward.faces) return forward;

    std::ofstream out(path, std::ios::binary);
    out << spf.header();
    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        const auto line = spf.line(id);
        if (line.empty()) continue;
        const auto at = replaced.find(id);
        if (at != replaced.end()) out << at->second << '\n';  // as the line began, with its number
        else out << line << '\n';
    }
    for (const std::string& line : added) out << line << '\n';
    out << "ENDSEC;\nEND-ISO-10303-21;\n";
    return forward;
}

}  // namespace ifcfilter

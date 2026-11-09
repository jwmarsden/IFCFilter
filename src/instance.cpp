#include "instance.h"

#include "rigid.h"
#include "extent.h"
#include "spf.h"
#include "system.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifcfilter {
namespace {

// A smaller shape takes about as much to place as it does to write out
constexpr size_t MIN_BYTES = 512;

enum class Kind : uint8_t { Other, Set, Representation, Map, Mapped, Styled, ColourMap, Layer, Part };

Kind kind_of(std::string_view type) {
    if (type == "IFCTRIANGULATEDFACESET" || type == "IFCPOLYGONALFACESET") return Kind::Set;
    if (type == "IFCSHAPEREPRESENTATION") return Kind::Representation;
    if (type == "IFCREPRESENTATIONMAP") return Kind::Map;
    if (type == "IFCMAPPEDITEM") return Kind::Mapped;
    if (type == "IFCSTYLEDITEM") return Kind::Styled;
    if (type == "IFCINDEXEDCOLOURMAP") return Kind::ColourMap;
    if (type == "IFCPRESENTATIONLAYERASSIGNMENT" || type == "IFCPRESENTATIONLAYERWITHSTYLE") return Kind::Layer;
    // These only exist for what points at them
    if (type == "IFCCARTESIANTRANSFORMATIONOPERATOR3D" || type == "IFCAXIS2PLACEMENT3D" || type == "IFCDIRECTION" || type == "IFCCARTESIANPOINT" ||
        type == "IFCCARTESIANPOINTLIST3D" || type == "IFCINDEXEDPOLYGONALFACE" || type == "IFCINDEXEDPOLYGONALFACEWITHVOIDS" || type == "IFCCOLOURRGBLIST") {
        return Kind::Part;
    }
    return Kind::Other;
}

std::vector<Vector> points_in(std::string_view list) {
    std::vector<Vector> out;
    Vector point{};
    int axis = 0;
    for (const char* p = list.data(), *end = p + list.size(); p < end;) {
        if (*p == '-' || *p == '+' || *p == '.' || (*p >= '0' && *p <= '9')) {
            char* after = nullptr;
            const double value = std::strtod(p, &after);
            if (after == p) {
                ++p;
                continue;
            }
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

// A shape's axes, from three of its points that are far apart. The same points of a copy give the copy's.
struct Shape {
    uint32_t set = 0;
    std::vector<Vector> points;
    size_t a = 0, b = 0, c = 0;
    Rigid to_local;
};

std::optional<Rigid> frame(const std::vector<Vector>& points, size_t a, size_t b, size_t c) {
    const auto x = normalised(minus(points[b], points[a]));
    if (!x) return std::nullopt;
    const auto z = normalised(cross(*x, minus(points[c], points[a])));
    if (!z) return std::nullopt;
    Rigid out;
    out.axes = {*x, cross(*z, *x), *z};
    out.origin = points[a];
    return out;
}

bool set_up(Shape& shape) {
    const auto& points = shape.points;
    if (points.size() < 3) return false;
    double furthest = -1;
    for (size_t i = 0; i < points.size(); ++i) {
        const Vector along = minus(points[i], points[0]);
        const double distance = dot(along, along);
        if (distance > furthest) {
            furthest = distance;
            shape.b = i;
        }
    }
    const Vector line = minus(points[shape.b], points[0]);
    furthest = -1;
    for (size_t i = 0; i < points.size(); ++i) {
        const Vector off = cross(line, minus(points[i], points[0]));
        const double distance = dot(off, off);
        if (distance > furthest) {
            furthest = distance;
            shape.c = i;
        }
    }
    const auto axes = frame(points, shape.a, shape.b, shape.c);
    if (!axes) return false;
    shape.to_local = inverse(*axes);
    return true;
}

class Instancer {
  public:
    Instancer(const SpfFile& spf, double tolerance)
        : spf_(spf), tolerance_(tolerance), max_id_(spf.max_id()), emitter_(spf.max_id()), kinds_(max_id_, Kind::Other), excluded_(max_id_, false), top_(max_id_, false),
          map_stays_(max_id_, false), removed_(max_id_, false), references_(max_id_, 0) {}

    // False if there's nothing to do, and the file should stay as it is
    bool run(const std::string& out_path, InstanceStats& stats) {
        scan();
        stats.sets = sets_.size();
        exclude_what_cannot_be_flattened();
        find_copies(stats);
        if (copy_of_.empty()) return false;
        rewrite_representations();
        if (!remove_what_is_left_over()) return false;
        write(out_path);
        return true;
    }

  private:
    void scan() {
        for (uint32_t id = 0; id < max_id_; ++id) {
            if (spf_.line(id).empty()) continue;
            kinds_[id] = kind_of(spf_.type(id));
            if (kinds_[id] == Kind::Set) sets_.push_back(id);
        }
        for (uint32_t id = 0; id < max_id_; ++id) {
            const auto line = spf_.line(id);
            if (line.empty()) continue;
            const Kind kind = kinds_[id];
            if (kind == Kind::Styled || kind == Kind::ColourMap) read_follower(id, kind);
            for_each_ref(line, [&](uint32_t next) {
                if (next >= max_id_) return;
                if (kind != Kind::Layer) ++references_[next];
                const Kind target = kinds_[next];
                // Anything else that uses a set, such as a boolean or a texture, needs it as it is
                if (target == Kind::Set && kind != Kind::Representation && kind != Kind::Styled && kind != Kind::ColourMap && kind != Kind::Layer) excluded_[next] = true;
                if (target == Kind::Representation && kind != Kind::Map && kind != Kind::Layer) top_[next] = true;
                if (target == Kind::Map && kind != Kind::Mapped) map_stays_[next] = true;
            });
        }
        for (uint32_t id = 0; id < max_id_; ++id) {
            if (kinds_[id] == Kind::Map && !references_[id]) map_stays_[id] = true;
            if (kinds_[id] == Kind::Representation && !references_[id]) top_[id] = true;
        }
    }

    void read_follower(uint32_t id, Kind kind) {
        const auto attributes = attributes_of(spf_.line(id));
        if (attributes.size() < 2) return;
        const auto targets = refs_in(attributes[0]);
        if (targets.size() != 1) return;
        const uint32_t target = targets[0];
        if (kind == Kind::Styled && attributes.size() > 1) {
            if (target < max_id_ && spf_.type(target) == "IFCMAPPEDITEM") styled_mapped_.insert(target);
            looks_[target].push_back(expanded(attributes[1]));
        } else if (kind == Kind::ColourMap && attributes.size() == 4) {
            looks_[target].push_back(std::string(attributes[1]) + expanded(attributes[2]) + std::string(attributes[3]));
        }
        followers_[target].push_back(id);
    }

    // Text with references replaced by what they point to, so that the same style in two instances compares the same
    std::string expanded(std::string_view text) {
        std::string out;
        bool in_string = false;
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '\'') in_string = !in_string;
            if (c != '#' || in_string) {
                out += c;
                continue;
            }
            uint32_t id = 0;
            size_t j = i + 1;
            for (; j < text.size() && text[j] >= '0' && text[j] <= '9'; ++j) id = id * 10 + static_cast<uint32_t>(text[j] - '0');
            out += content_of(id);
            i = j - 1;
        }
        return out;
    }

    const std::string& content_of(uint32_t id) {
        const auto seen = contents_.find(id);
        if (seen != contents_.end()) return seen->second;
        const auto line = spf_.line(id);
        const auto equals = line.find('=');
        std::string text = equals == std::string_view::npos ? std::string() : expanded(line.substr(equals));
        return contents_[id] = std::move(text);
    }

    uint32_t representation_of(uint32_t map) const {
        const auto attributes = attributes_of(spf_.line(map));
        const auto ids = attributes.size() == 2 ? refs_in(attributes[1]) : std::vector<uint32_t>{};
        return ids.size() == 1 ? ids[0] : 0;
    }

    std::vector<uint32_t> items_of(uint32_t representation) const {
        const auto attributes = attributes_of(spf_.line(representation));
        return attributes.size() == 4 ? refs_in(attributes[3]) : std::vector<uint32_t>{};
    }

    // Where a mapped item puts its map's points. None if it can't be repeated from outside.
    std::optional<std::pair<uint32_t, Rigid>> placement_of(uint32_t mapped) const {
        const auto parts = attributes_of(spf_.line(mapped));
        if (parts.size() != 2) return std::nullopt;
        const auto maps = refs_in(parts[0]);
        const auto targets = refs_in(parts[1]);
        if (maps.size() != 1 || targets.size() != 1) return std::nullopt;
        const auto target = operator_of(spf_, targets[0]);
        if (!target) return std::nullopt;
        const auto map = attributes_of(spf_.line(maps[0]));
        const auto origins = map.size() == 2 ? refs_in(map[0]) : std::vector<uint32_t>{};
        if (origins.size() != 1) return std::nullopt;
        const auto origin = axis_placement(spf_, origins[0]);
        if (!origin) return std::nullopt;
        return std::make_pair(maps[0], compose(*target, inverse(*origin)));
    }

    void exclude_under(uint32_t representation, std::unordered_set<uint32_t>& visited) {
        if (!representation || !visited.insert(representation).second) return;
        for (const uint32_t item : items_of(representation)) {
            if (item >= max_id_) continue;
            if (kinds_[item] == Kind::Set) excluded_[item] = true;
            else if (kinds_[item] == Kind::Mapped) {
                const auto parts = attributes_of(spf_.line(item));
                const auto maps = parts.size() == 2 ? refs_in(parts[0]) : std::vector<uint32_t>{};
                if (maps.size() == 1) exclude_under(representation_of(maps[0]), visited);
            }
        }
    }

    // Shapes in a representation, counting what's in the maps it places
    size_t shapes_in(uint32_t representation, int depth = 0) {
        size_t count = 0;
        for (const uint32_t item : items_of(representation)) {
            if (item < max_id_ && kinds_[item] == Kind::Mapped && depth < 8) {
                const auto parts = attributes_of(spf_.line(item));
                const auto maps = parts.size() == 2 ? refs_in(parts[0]) : std::vector<uint32_t>{};
                count += maps.size() == 1 ? shapes_in(representation_of(maps[0]), depth + 1) : 1;
            } else {
                ++count;
            }
        }
        return count;
    }

    // Mapped items get flattened, so what's under one that scales or has its own style has to stay
    void exclude_what_cannot_be_flattened() {
        std::unordered_set<uint32_t> visited;
        // What an opening cuts stays as it is, as taking a solid from a mesh is fragile
        for (const uint32_t id : sets_openings_touch(spf_)) excluded_[id] = true;
        // A body of several shapes stays as it is: flattened, it would be one mapped item for each, and Open IFC
        // Viewer won't select an element whose body is many mapped items by clicking it until it's been opened
        // from the tree. A body of one shape becomes one mapped item, which it selects straight away.
        std::unordered_set<uint32_t> several;
        for (uint32_t id = 0; id < max_id_; ++id) {
            if (kinds_[id] == Kind::Representation && top_[id] && shapes_in(id) > 1) exclude_under(id, several);
        }
        for (uint32_t id = 0; id < max_id_; ++id) {
            if (kinds_[id] != Kind::Mapped) continue;
            if (placement_of(id) && !styled_mapped_.count(id)) continue;
            const auto parts = attributes_of(spf_.line(id));
            const auto maps = parts.size() == 2 ? refs_in(parts[0]) : std::vector<uint32_t>{};
            if (maps.size() == 1) exclude_under(representation_of(maps[0]), visited);
        }
    }

    void find_copies(InstanceStats& stats) {
        std::map<std::string, std::vector<uint32_t>> buckets;
        std::string key;
        for (const uint32_t id : sets_) {
            if (excluded_[id]) continue;
            const auto attributes = attributes_of(spf_.line(id));
            const auto lists = attributes.empty() ? std::vector<uint32_t>{} : refs_in(attributes[0]);
            if (lists.size() != 1 || spf_.type(lists[0]) != "IFCCARTESIANPOINTLIST3D") continue;
            size_t bytes = spf_.line(id).size() + spf_.line(lists[0]).size();
            key.clear();
            if (spf_.type(id) == "IFCTRIANGULATEDFACESET") {
                if (attributes.size() < 4 || attributes[1] != "$") continue;  // its own normals would have to turn too
                key.append(attributes[2]).append(attributes[3]);
                if (attributes.size() > 4) key.append(attributes[4]);
            } else {
                if (attributes.size() != 4) continue;
                key.append(attributes[1]).append(attributes[3]);
                for (const uint32_t face : refs_in(attributes[2])) {
                    const auto line = spf_.line(face);
                    bytes += line.size();
                    key.append(line.substr(line.find('=')));
                }
            }
            if (bytes < MIN_BYTES) continue;
            const auto look = looks_.find(id);
            if (look != looks_.end()) {
                auto sorted = look->second;
                std::sort(sorted.begin(), sorted.end());
                for (const std::string& part : sorted) key.append("|").append(part);
            }
            buckets[derived_guid(std::string(spf_.type(id)) + key)].push_back(id);
        }

        const double limit = tolerance_ * tolerance_;
        for (const auto& [bucket, members] : buckets) {
            if (members.size() < 2) continue;
            std::vector<Shape> shapes;
            std::vector<bool> has_copies;
            for (const uint32_t id : members) {
                const auto attributes = attributes_of(spf_.line(id));
                const auto list = attributes_of(spf_.line(refs_in(attributes[0])[0]));
                if (list.empty()) continue;
                std::vector<Vector> points = points_in(list[0]);
                bool placed = false;
                for (size_t s = 0; s < shapes.size() && !placed; ++s) {
                    const Shape& shape = shapes[s];
                    if (shape.points.size() != points.size()) continue;
                    const auto axes = frame(points, shape.a, shape.b, shape.c);
                    if (!axes) continue;
                    const Rigid move = compose(*axes, shape.to_local);
                    bool same = true;
                    for (size_t i = 0; i < points.size() && same; ++i) {
                        const Vector off = minus(applied(move, shape.points[i]), points[i]);
                        same = dot(off, off) <= limit;
                    }
                    if (!same) continue;
                    copy_of_[id] = {shape.set, move};
                    has_copies[s] = true;
                    placed = true;
                }
                if (placed) continue;
                Shape shape;
                shape.set = id;
                shape.points = std::move(points);
                if (!set_up(shape)) continue;
                shapes.push_back(std::move(shape));
                has_copies.push_back(false);
            }
            stats.shapes += static_cast<uint64_t>(std::count(has_copies.begin(), has_copies.end(), true));
        }
        stats.copies = copy_of_.size();
    }

    bool changes(uint32_t representation) {
        const auto seen = changes_.find(representation);
        if (seen != changes_.end()) return seen->second;
        bool any = false;
        for (const uint32_t item : items_of(representation)) {
            if (item >= max_id_) continue;
            if (kinds_[item] == Kind::Set) any = any || copy_of_.count(item) > 0;
            else if (kinds_[item] == Kind::Mapped) {
                const auto placed = placement_of(item);
                if (placed) any = changes(representation_of(placed->first)) || any;
            }
        }
        changes_[representation] = any;
        return any;
    }

    // A map of just this item, for what's placed from it
    uint32_t map_of(uint32_t item, const std::vector<std::string_view>& representation) {
        const auto seen = item_maps_.find(item);
        if (seen != item_maps_.end()) return seen->second;
        const std::string type = kinds_[item] == Kind::Set ? "'Tessellation'" : std::string(representation[2]);
        const uint32_t held = emitter_.emit("IFCSHAPEREPRESENTATION(" + std::string(representation[0]) + "," + std::string(representation[1]) + "," + type + ",(" + reference(item) + "));");
        const uint32_t identity = emitter_.emit_identity();
        const uint32_t map = emitter_.emit("IFCREPRESENTATIONMAP(" + reference(identity) + "," + reference(held) + ");");
        item_maps_[item] = map;
        return map;
    }

    // What's in a representation, as maps and where to put them, with no map inside a map
    std::vector<std::pair<uint32_t, Rigid>> flattened(uint32_t representation) {
        std::vector<std::pair<uint32_t, Rigid>> out;
        const auto attributes = attributes_of(spf_.line(representation));
        for (const uint32_t item : items_of(representation)) {
            if (item >= max_id_) continue;
            const auto copy = copy_of_.find(item);
            if (copy != copy_of_.end()) {
                out.emplace_back(map_of(copy->second.first, attributes), copy->second.second);
            } else if (kinds_[item] == Kind::Mapped) {
                const auto placed = placement_of(item);
                if (!placed) continue;
                const uint32_t inner = representation_of(placed->first);
                if (!changes(inner)) {
                    out.emplace_back(placed->first, *operator_of(spf_, refs_in(attributes_of(spf_.line(item))[1])[0]));
                    continue;
                }
                for (const auto& [map, place] : flattened(inner)) out.emplace_back(map, compose(placed->second, place));
            } else {
                out.emplace_back(map_of(item, attributes), Rigid{});
            }
        }
        return out;
    }

    void rewrite_representations() {
        std::unordered_map<uint32_t, bool> in_kept_map;
        for (uint32_t id = 0; id < max_id_; ++id) {
            if (kinds_[id] == Kind::Map && map_stays_[id]) in_kept_map[representation_of(id)] = true;
        }
        for (uint32_t id = 0; id < max_id_; ++id) {
            if (kinds_[id] != Kind::Representation || !changes(id)) continue;
            if (!top_[id] && !in_kept_map.count(id)) continue;
            std::vector<uint32_t> items;
            for (const auto& [map, place] : flattened(id)) {
                const uint32_t target = emitter_.emit_operator(place);
                const uint32_t item = emitter_.emit_once("IFCMAPPEDITEM(" + reference(map) + "," + reference(target) + ");");
                // Items are a set, and a shape in the same place twice only needs to be there once
                if (std::find(items.begin(), items.end(), item) == items.end()) items.push_back(item);
            }
            const auto attributes = attributes_of(spf_.line(id));
            rewritten_[id] = reference(id) + "=IFCSHAPEREPRESENTATION(" + std::string(attributes[0]) + "," + std::string(attributes[1]) + ",'MappedRepresentation'," + list_of(items) + ");";
        }
    }

    void release(uint32_t id, std::vector<uint32_t>& to_remove) {
        if (id >= max_id_) return;
        if (references_[id] > 0) --references_[id];
        if (references_[id] || removed_[id] || rewritten_.count(id)) return;
        const Kind kind = kinds_[id];
        if (kind == Kind::Set || kind == Kind::Representation || kind == Kind::Map || kind == Kind::Mapped || kind == Kind::Part) to_remove.push_back(id);
    }

    // Copies go, and with them whatever nothing points to any more. False if that would leave something
    // pointing at what's gone, which would mean the plan above is wrong: the file is then left as it was.
    bool remove_what_is_left_over() {
        const auto count_new = [&](std::string_view line) {
            for_each_ref(line, [&](uint32_t next) {
                if (next < max_id_) ++references_[next];
            });
        };
        for (const std::string& line : emitter_.lines()) count_new(line);
        for (const auto& [id, line] : rewritten_) count_new(line);

        std::vector<uint32_t> to_remove;
        for (const auto& [id, line] : rewritten_) {
            for_each_ref(spf_.line(id), [&](uint32_t next) { release(next, to_remove); });
        }
        for (const auto& [set, copy] : copy_of_) to_remove.push_back(set);
        while (!to_remove.empty()) {
            const uint32_t id = to_remove.back();
            to_remove.pop_back();
            if (removed_[id]) continue;
            removed_[id] = true;
            const auto followers = followers_.find(id);
            if (followers != followers_.end()) {
                to_remove.insert(to_remove.end(), followers->second.begin(), followers->second.end());
            }
            for_each_ref(spf_.line(id), [&](uint32_t next) { release(next, to_remove); });
        }

        for (uint32_t id = 0; id < max_id_; ++id) {
            if (removed_[id] || kinds_[id] != Kind::Representation || rewritten_.count(id)) continue;
            if (changes(id)) return false;
        }
        return true;
    }

    void write(const std::string& path) {
        std::ofstream out(path, std::ios::binary);
        out << spf_.header();
        std::string text;
        const auto is_removed = [&](uint32_t id) { return id < max_id_ && removed_[id]; };
        for (uint32_t id = 0; id < max_id_; ++id) {
            const auto line = spf_.line(id);
            if (line.empty() || removed_[id]) continue;
            const auto rewritten = rewritten_.find(id);
            if (rewritten != rewritten_.end()) {
                out << rewritten->second << '\n';
            } else if (kinds_[id] == Kind::Layer) {
                if (rewrite(line, is_removed, WhenEmpty::Drop, text)) out << text << '\n';
            } else {
                out << line << '\n';
            }
        }
        for (const std::string& line : emitter_.lines()) out << line << '\n';
        out << "ENDSEC;\nEND-ISO-10303-21;\n";
    }

    const SpfFile& spf_;
    const double tolerance_;
    const uint32_t max_id_;
    Emitter emitter_;
    std::vector<Kind> kinds_;
    std::vector<uint32_t> sets_;
    std::vector<bool> excluded_, top_, map_stays_, removed_;
    std::vector<uint32_t> references_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> followers_;
    std::unordered_map<uint32_t, std::vector<std::string>> looks_;
    std::unordered_map<uint32_t, std::string> contents_;
    std::unordered_set<uint32_t> styled_mapped_;
    std::unordered_map<uint32_t, std::pair<uint32_t, Rigid>> copy_of_;  // a set, and what it's placed from, and how
    std::unordered_map<uint32_t, bool> changes_;
    std::unordered_map<uint32_t, uint32_t> item_maps_;
    std::map<uint32_t, std::string> rewritten_;
};

}  // namespace

InstanceStats instance(const std::string& path, double precision_mm) {
    namespace fs = std::filesystem;
    InstanceStats stats;
    stats.bytes_before = stats.bytes_after = fs::file_size(path);
    const std::string temporary = path + ".instanced";
    bool changed = false;
    {
        const SpfFile spf(path);
        const double unit = length_unit(spf);
        if (unit <= 0) return stats;
        stats.checked = true;
        Instancer instancer(spf, precision_mm * 1e-3 / unit);
        changed = instancer.run(temporary, stats);
        if (!changed) stats.copies = stats.shapes = 0;
    }
    if (changed) {
        replace_file(temporary, path);
        stats.bytes_after = fs::file_size(path);
    }
    return stats;
}

}  // namespace ifcfilter

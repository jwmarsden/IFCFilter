#include "merge.h"

#include "extent.h"
#include "rigid.h"
#include "site.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace bimpass {
namespace {

// What's between a string's quotes, wherever it is in a value such as IFCTEXT('X')
std::string quoted_in(std::string_view value) {
    const auto open = value.find('\'');
    const auto close = value.rfind('\'');
    return open == std::string_view::npos || close <= open ? std::string() : std::string(value.substr(open + 1, close - open - 1));
}

struct PropertySet {
    std::string_view name;  // with its quotes, as it's written
    std::vector<std::pair<std::string_view, uint32_t>> properties;  // each one's text, and its id
    std::string site, design;
};

struct Member {
    uint32_t id = 0;
    Rigid placement;
    std::vector<uint32_t> bodies;
    bool can_merge = false;  // placed in a way a mapped item can repeat
    bool placed = false;
};

class Planner {
  public:
    Planner(const SpfFile& spf, const std::function<bool(uint32_t)>& gone, const std::string& stem, double gap_m, MergePlan& plan)
        : spf_(spf), gone_(gone), stem_(stem), plan_(plan), emitter_(spf.max_id()), extents_(spf) {
        const double unit = length_unit(spf);
        unit_ = unit;
        plan_.gap_checked = gap_m <= 0 || unit > 0;
        gap_ = unit > 0 ? gap_m / unit : 0;
    }

    void run(const std::vector<uint32_t>& candidates) {
        const std::unordered_set<uint32_t> candidate(candidates.begin(), candidates.end());
        read_relationships(candidate);

        // A design tag counts as an ID if it looks like one and isn't on most of what has one. One that's only
        // on one site's parts says which site an element with none belongs to.
        std::unordered_map<std::string, size_t> tagged;
        std::unordered_map<std::string, std::set<std::string>> sites_of_tag;
        size_t with_tags = 0;
        for (const uint32_t id : candidates) {
            const std::string tag = design_of(id);
            if (tag.empty()) continue;
            ++with_tags;
            ++tagged[tag];
            const std::string site = site_of(id);
            if (is_one_site(site)) sites_of_tag[tag].insert(site);
        }
        for (const auto& [tag, count] : tagged) {
            if (looks_like_an_id(tag) && count * 2 > with_tags) ++plan_.tags_not_ids;
        }
        const auto is_id = [&](const std::string& tag) { return looks_like_an_id(tag) && tagged[tag] * 2 <= with_tags; };

        // Ordered, so that a run writes the same file every time. Parts that can't merge stay in, as what
        // can join the rest.
        struct Asset {
            std::string name, label;
            std::vector<Member> members;
        };
        std::map<std::string, Asset> sites;
        for (const uint32_t id : candidates) {
            std::string key = site_of(id);
            Asset asset{key, "site ID " + key, {}};
            bool by_design = false;
            if (!is_one_site(key)) {
                const std::string tag = design_of(id);
                if (tag.empty() || !is_id(tag)) continue;
                const auto owners = sites_of_tag.find(tag);
                if (owners != sites_of_tag.end() && owners->second.size() == 1) {
                    key = *owners->second.begin();
                    asset = {key, "site ID " + key, {}};
                    ++plan_.joined_by_design;
                } else {
                    key = "design " + tag;
                    asset = {tag, "design tag " + tag, {}};
                }
                by_design = true;
            }
            const auto attributes = attributes_of(spf_.line(id));
            if (attributes.size() < 7) continue;
            Member member;
            member.id = id;
            member.bodies = bodies_of(attributes[6]);
            if (member.bodies.empty()) continue;  // an assembly goes with its parts, later
            const auto placed = placement_of(attributes[5]);
            if (placed) member.placement = *placed;
            member.placed = placed.has_value();
            member.can_merge = placed.has_value();
            if (!placed) ++plan_.left_out_other;
            if (by_design) by_design_.insert(id);
            auto& at = sites.emplace(key, std::move(asset)).first->second;
            at.members.push_back(std::move(member));
        }
        for (auto& [key, asset] : sites) {
            const auto& members = asset.members;
            if (std::count_if(members.begin(), members.end(), [](const Member& m) { return m.can_merge; }) < 2) continue;
            by_site_ = asset.label.rfind("site ID ", 0) == 0;
            const auto groups = groups_of(asset.label, members);
            if (groups.size() > 1) ++plan_.sites_apart;
            for (size_t i = 0; i < groups.size(); ++i) {
                const std::string note = groups.size() > 1 ? ", group " + std::to_string(i + 1) + " of " + std::to_string(groups.size()) + ", far apart" : "";
                merge(asset.name, asset.label, i == 0 ? key : key + "/" + std::to_string(i + 1), note, groups[i]);
            }
        }
        // What has a site and wasn't merged goes on its site's level too
        for (const auto& [key, asset] : sites) {
            if (asset.label.rfind("site ID ", 0) != 0) continue;
            for (const Member& member : asset.members) {
                const auto structure = container_.find(member.id);
                if (!plan_.merged_into.count(member.id) && structure != container_.end()) levels_[asset.name].emplace_back(member.id, structure->second);
            }
        }
        make_levels();

        // Finish what's only known once every asset has been made
        for (const auto& [structure, elements] : contained_) {
            emitter_.emit("IFCRELCONTAINEDINSPATIALSTRUCTURE('" + derived_guid("contains" + reference(structure) + "@" + stem_) + "'," + owner_ + ",$,$," + list_of(elements) + "," +
                          reference(structure) + ");");
            plan_.keep.push_back(structure);
        }
        // An assembly has no shape of its own, so it's nothing once all its parts have gone
        for (const auto& [assembly, parts] : parts_of_) {
            if (!candidate.count(assembly) || parts.empty()) continue;
            const auto first = plan_.merged_into.find(parts[0]);
            if (first == plan_.merged_into.end()) continue;
            const size_t asset = first->second;
            const bool together = std::all_of(parts.begin(), parts.end(), [&](uint32_t part) {
                const auto at = plan_.merged_into.find(part);
                return at != plan_.merged_into.end() && at->second == asset;
            });
            const auto attributes = attributes_of(spf_.line(assembly));
            if (together && attributes.size() >= 7 && bodies_of(attributes[6]).empty()) plan_.merged_into[assembly] = asset;
        }
        plan_.lines = std::move(emitter_.lines());
    }

  private:
    // Each site is a level of its own, named after it, beside the levels its elements were on, which keep
    // what has no site. Levels are listed in alphabetical order, and a level left with nothing goes.
    void make_levels() {
        std::map<uint32_t, std::vector<uint32_t>> new_levels_of;  // building, and its site levels
        std::map<uint32_t, std::string> names;
        std::unordered_set<uint32_t> moved;
        for (const auto& [site, entries] : levels_) {
            const uint32_t building = building_of(entries[0].second);
            if (!building) {
                // Nowhere to put a level: assets go where their first part was, and the rest stay
                for (const auto& [element, structure] : entries) {
                    if (element >= spf_.max_id()) contained_[structure].push_back(element);
                }
                continue;
            }
            const auto first = attributes_of(spf_.line(building));
            const std::string owner = owner_ != "$" ? owner_ : std::string(first[1]);
            std::vector<uint32_t> elements;
            for (const auto& [element, structure] : entries) {
                elements.push_back(element);
                if (element < spf_.max_id()) moved.insert(element);
            }
            const uint32_t level = emitter_.emit("IFCBUILDINGSTOREY('" + derived_guid("level/" + site + "@" + stem_) + "'," + owner + ",'" + site + "',$,$," + std::string(first[5]) +
                                                 ",$,$,.ELEMENT.,$);");
            emitter_.emit("IFCRELCONTAINEDINSPATIALSTRUCTURE('" + derived_guid("level/" + site + "@" + stem_ + "/contains") + "'," + owner + ",$,$," + list_of(elements) + "," +
                          reference(level) + ");");
            new_levels_of[building].push_back(level);
            names[level] = site;
            ++plan_.levels;
            plan_.on_levels += elements.size();
        }

        // What moved leaves the relationships that held it
        std::map<uint32_t, size_t> left_on;  // structure, and what's still on it
        for (const uint32_t relationship : containments_) {
            const auto attributes = attributes_of(spf_.line(relationship));
            std::vector<uint32_t> kept;
            const auto elements = refs_in(attributes[4]);
            for (const uint32_t element : elements) {
                if (moved.count(element)) continue;
                kept.push_back(element);
            }
            const auto structure = refs_in(attributes[5]);
            for (const uint32_t element : kept) {
                if (!gone_(element) && !plan_.merged_into.count(element) && structure.size() == 1) ++left_on[structure[0]];
            }
            if (kept.size() == elements.size()) continue;
            plan_.replace[relationship] = kept.empty() ? std::string()
                : reference(relationship) + "=IFCRELCONTAINEDINSPATIALSTRUCTURE(" + std::string(attributes[0]) + "," + std::string(attributes[1]) + "," + std::string(attributes[2]) + "," +
                      std::string(attributes[3]) + "," + list_of(kept) + "," + std::string(attributes[5]) + ");";
        }
        for (const auto& [structure, assets] : contained_) left_on[structure] += assets.size();

        for (const auto& [building, levels] : new_levels_of) {
            std::vector<uint32_t> children = levels;
            const auto existing = parts_of_.find(building);
            if (existing != parts_of_.end()) {
                for (const uint32_t child : existing->second) {
                    // A level that only held what moved goes
                    const bool held = held_.count(child) > 0;
                    if (spf_.type(child) == "IFCBUILDINGSTOREY" && held && !left_on[child] && !parts_of_.count(child)) {
                        plan_.dropped.push_back(child);
                        continue;
                    }
                    children.push_back(child);
                    names[child] = quoted_in(attributes_of(spf_.line(child))[2]);
                }
            }
            std::stable_sort(children.begin(), children.end(), [&](uint32_t a, uint32_t b) { return names[a] < names[b]; });
            const auto aggregation = aggregation_of_.find(building);
            if (aggregation != aggregation_of_.end()) {
                const auto attributes = attributes_of(spf_.line(aggregation->second));
                plan_.replace[aggregation->second] = reference(aggregation->second) + "=IFCRELAGGREGATES(" + std::string(attributes[0]) + "," + std::string(attributes[1]) + "," +
                                                     std::string(attributes[2]) + "," + std::string(attributes[3]) + "," + reference(building) + "," + list_of(children) + ");";
            } else {
                const std::string owner(attributes_of(spf_.line(building))[1]);
                emitter_.emit("IFCRELAGGREGATES('" + derived_guid("levels" + reference(building) + "@" + stem_) + "'," + owner + ",$,$," + reference(building) + "," + list_of(children) + ");");
            }
        }
    }

    // The building a structure is in, or is; 0 if none
    uint32_t building_of(uint32_t structure) {
        for (int depth = 0; structure && depth < 16; ++depth) {
            if (spf_.type(structure) == "IFCBUILDING") return structure;
            const auto whole = whole_of_.find(structure);
            structure = whole == whole_of_.end() ? 0 : whole->second;
        }
        return 0;
    }

    // A site's parts that can merge, in groups that are each one thing: every part is within the gap of
    // another part of its group, merging or not. A part on its own stays as it is.
    std::vector<std::vector<Member>> groups_of(const std::string& label, const std::vector<Member>& members) {
        std::vector<std::vector<Member>> out;
        if (gap_ <= 0) {
            out.emplace_back();
            for (const Member& m : members) {
                if (m.can_merge) out.back().push_back(m);
            }
            return out;
        }
        std::vector<Box> boxes(members.size());
        std::vector<bool> known(members.size(), false);
        for (size_t i = 0; i < members.size(); ++i) {
            if (!members[i].placed) continue;
            known[i] = true;
            for (const uint32_t body : members[i].bodies) {
                for (const uint32_t item : refs_in(attributes_of(spf_.line(body))[3])) {
                    if (!extents_.item(item, members[i].placement, boxes[i])) known[i] = false;
                }
            }
            if (boxes[i].empty()) known[i] = false;
        }
        std::vector<size_t> parent(members.size());
        for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
        const auto find = [&](size_t i) {
            while (parent[i] != i) i = parent[i] = parent[parent[i]];
            return i;
        };
        for (size_t i = 0; i < members.size(); ++i) {
            if (!known[i]) continue;
            for (size_t j = i + 1; j < members.size(); ++j) {
                if (known[j] && find(i) != find(j) && gap_between(boxes[i], boxes[j]) <= gap_) parent[find(i)] = find(j);
            }
        }
        // Groups in the order of their first part
        std::map<size_t, size_t> group_of_root;
        std::vector<std::vector<Member>> all;
        for (size_t i = 0; i < members.size(); ++i) {
            if (!members[i].can_merge) continue;
            if (!known[i]) {
                ++plan_.left_out_extent;
                continue;
            }
            const auto [at, added] = group_of_root.emplace(find(i), all.size());
            if (added) all.emplace_back();
            all[at->second].push_back(members[i]);
        }
        for (auto& group : all) {
            if (group.size() > 1) out.push_back(std::move(group));
            else if (all.size() > 1) plan_.apart[group[0].id] = label;
        }
        return out;
    }

    void read_relationships(const std::unordered_set<uint32_t>& candidate) {
        for (uint32_t id = 0; id < spf_.max_id(); ++id) {
            const auto type = spf_.type(id);
            if (type == "IFCRELDEFINESBYPROPERTIES") {
                const auto attributes = attributes_of(spf_.line(id));
                if (attributes.size() != 6) continue;
                const auto sets = refs_in(attributes[5]);
                if (sets.size() != 1 || spf_.type(sets[0]) != "IFCPROPERTYSET") continue;
                for (const uint32_t object : refs_in(attributes[4])) {
                    if (candidate.count(object)) sets_of_[object].push_back(sets[0]);
                }
            } else if (type == "IFCRELVOIDSELEMENT") {
                const auto attributes = attributes_of(spf_.line(id));
                if (attributes.size() == 6) {
                    for (const uint32_t element : refs_in(attributes[4])) voids_of_[element].push_back(id);
                }
            } else if (type == "IFCRELCONTAINEDINSPATIALSTRUCTURE") {
                const auto attributes = attributes_of(spf_.line(id));
                if (attributes.size() != 6) continue;
                const auto structure = refs_in(attributes[5]);
                if (structure.size() != 1) continue;
                containments_.push_back(id);
                held_.insert(structure[0]);
                for (const uint32_t element : refs_in(attributes[4])) container_[element] = structure[0];
            } else if (type == "IFCRELAGGREGATES") {
                const auto attributes = attributes_of(spf_.line(id));
                if (attributes.size() != 6) continue;
                const auto whole = refs_in(attributes[4]);
                if (whole.size() != 1) continue;
                aggregation_of_[whole[0]] = id;
                auto& parts = parts_of_[whole[0]];
                parts = refs_in(attributes[5]);
                for (const uint32_t part : parts) whole_of_[part] = whole[0];
            }
        }
    }

    const PropertySet& property_set(uint32_t id) {
        const auto seen = property_sets_.find(id);
        if (seen != property_sets_.end()) return seen->second;
        PropertySet& set = property_sets_[id];
        const auto attributes = attributes_of(spf_.line(id));
        if (attributes.size() != 5) return set;
        set.name = attributes[2];
        const std::string name = quoted_in(attributes[2]);
        for (const uint32_t property : refs_in(attributes[4])) {
            const auto line = spf_.line(property);
            if (line.empty()) continue;
            set.properties.emplace_back(line.substr(line.find('=')), property);
            if (!SITE_PSETS.count(name) && !DESIGN_PSETS.count(name)) continue;
            const auto parts = attributes_of(line);
            if (parts.size() <= 2) continue;
            const std::string property_name = quoted_in(parts[0]);
            if (SITE_PSETS.count(name) && property_name == SITE_PROPERTY) set.site = quoted_in(parts[2]);
            if (DESIGN_PSETS.count(name) && property_name == DESIGN_PROPERTY) set.design = quoted_in(parts[2]);
        }
        return set;
    }

    // Blank if it has none. Asset is preferred to Data, though they've not been seen to differ.
    std::string site_of(uint32_t element) {
        std::string site;
        const auto sets = sets_of_.find(element);
        if (sets == sets_of_.end()) return site;
        for (const uint32_t id : sets->second) {
            const PropertySet& set = property_set(id);
            if (set.site.empty()) continue;
            if (set.name == "'Asset'") return set.site;
            site = set.site;
        }
        return site;
    }

    // Blank if it has none. Quantities is preferred, though they've not been seen to differ.
    std::string design_of(uint32_t element) {
        std::string design;
        const auto sets = sets_of_.find(element);
        if (sets == sets_of_.end()) return design;
        for (const uint32_t id : sets->second) {
            const PropertySet& set = property_set(id);
            if (set.design.empty()) continue;
            if (set.name == "'Quantities'") return set.design;
            design = set.design;
        }
        return design;
    }

    std::vector<uint32_t> bodies_of(std::string_view shape) {
        std::vector<uint32_t> out;
        const auto shapes = refs_in(shape);
        if (shapes.size() != 1) return out;
        const auto attributes = attributes_of(spf_.line(shapes[0]));
        if (attributes.size() != 3) return out;
        for (const uint32_t representation : refs_in(attributes[2])) {
            const auto parts = attributes_of(spf_.line(representation));
            if (parts.size() == 4 && parts[1] == "'Body'" && !gone_(representation)) out.push_back(representation);
        }
        return out;
    }

    // Where an element's placement puts it in the world; none for a grid placement
    std::optional<Rigid> placement_of(std::string_view reference_text) { return extents_.placement(reference_text); }

    // A map that shows a whole representation as it is, so that it can be placed by a mapped item
    uint32_t map_of(uint32_t representation) {
        const auto seen = maps_.find(representation);
        if (seen != maps_.end()) return seen->second;
        const uint32_t identity = emitter_.emit_identity();
        const uint32_t map = emitter_.emit("IFCREPRESENTATIONMAP(" + reference(identity) + "," + reference(representation) + ");");
        maps_[representation] = map;
        plan_.keep.push_back(representation);
        return map;
    }

    // The mapped items that put a member's body where it was. False if they can't.
    bool place_body(const Member& member, const Rigid& relative, std::vector<std::pair<uint32_t, Rigid>>& out) {
        for (const uint32_t body : member.bodies) {
            const auto attributes = attributes_of(spf_.line(body));
            bool mapped = attributes[2] == "'MappedRepresentation'";
            std::vector<std::pair<uint32_t, Rigid>> items;
            // Items that are already mapped are placed directly, as maps inside maps aren't reliably read
            if (mapped) {
                for (const uint32_t item : refs_in(attributes[3])) {
                    if (gone_(item)) continue;
                    if (spf_.type(item) != "IFCMAPPEDITEM") {
                        mapped = false;
                        break;
                    }
                    const auto parts = attributes_of(spf_.line(item));
                    const auto map = parts.size() == 2 ? refs_in(parts[0]) : std::vector<uint32_t>{};
                    const auto target = parts.size() == 2 ? refs_in(parts[1]) : std::vector<uint32_t>{};
                    const auto placed = map.size() == 1 && target.size() == 1 ? operator_of(spf_, target[0]) : std::nullopt;
                    if (!placed) return false;
                    items.emplace_back(map[0], compose(relative, *placed));
                }
            }
            if (!mapped) {
                items.clear();
                items.emplace_back(map_of(body), relative);
            }
            out.insert(out.end(), items.begin(), items.end());
        }
        return !out.empty();
    }

    // Quantities for the asset as a whole: its height, width and length are those of all of it, in metres, as
    // for a gantry, the structure's, and other numbers that differ part to part are added up. Length is the
    // longer way across in plan, width the shorter, height up. What isn't a number, and differs, is left out.
    void combine_quantities(const std::vector<const Member*>& members, std::map<std::string_view, std::vector<uint32_t>>& kept) {
        std::string_view set_name;
        std::map<std::string, std::vector<std::string>> values;  // property, and its value on each part that has it
        std::map<std::string, std::string> wrapper;              // property, and the type its value is written as
        for (const Member* member : members) {
            const auto sets = sets_of_.find(member->id);
            if (sets == sets_of_.end()) continue;
            for (const uint32_t id : sets->second) {
                const PropertySet& set = property_set(id);
                if (set.name != "'Quantities'") continue;
                set_name = set.name;
                for (const auto& [text, property] : set.properties) {
                    const auto parts = attributes_of(spf_.line(property));
                    if (parts.size() < 3) continue;
                    const std::string name = quoted_in(parts[0]);
                    const auto value = parts[2];
                    const auto open = value.find('(');
                    if (open == std::string_view::npos) continue;
                    wrapper.emplace(name, std::string(value.substr(0, open)));
                    values[name].push_back(value.find('\'') == std::string_view::npos ? std::string(value.substr(open + 1, value.size() - open - 2)) : quoted_in(value));
                }
            }
        }
        if (set_name.empty()) return;

        Box box;
        bool known = unit_ > 0;
        for (const Member* member : members) {
            for (const uint32_t body : member->bodies) {
                for (const uint32_t item : refs_in(attributes_of(spf_.line(body))[3])) {
                    if (!extents_.item(item, member->placement, box)) known = false;
                }
            }
        }
        const double across_x = (box.high[0] - box.low[0]) * unit_, across_y = (box.high[1] - box.low[1]) * unit_;
        const std::map<std::string, double> sizes = {
            {"Length", std::max(across_x, across_y)}, {"Width", std::min(across_x, across_y)}, {"Height", (box.high[2] - box.low[2]) * unit_}};

        auto& list = kept[set_name];
        std::map<std::string, uint32_t> kept_by_name;
        for (const uint32_t id : list) kept_by_name[quoted_in(attributes_of(spf_.line(id))[0])] = id;
        for (const auto& [name, seen] : values) {
            std::string combined;
            const auto size = sizes.find(name);
            if (size != sizes.end() && known && !box.empty()) {
                std::ostringstream out;
                out << std::fixed << std::setprecision(3) << size->second;
                combined = out.str();
            } else if (kept_by_name.count(name)) {
                continue;
            } else {
                double sum = 0;
                size_t numbers = 0, places = 0;
                bool all_numbers = true;
                for (const std::string& v : seen) {
                    if (v.empty() || v == "N/A") continue;
                    char* end = nullptr;
                    const double x = std::strtod(v.c_str(), &end);
                    if (end == v.c_str() || *end != '\0') {
                        all_numbers = false;
                        break;
                    }
                    sum += x;
                    ++numbers;
                    const auto dot = v.find('.');
                    if (dot != std::string::npos) places = std::max(places, v.size() - dot - 1);
                }
                if (!all_numbers || !numbers) continue;
                std::ostringstream out;
                out << std::fixed << std::setprecision(static_cast<int>(places)) << sum;
                combined = out.str();
            }
            const auto replaced = kept_by_name.find(name);
            if (replaced != kept_by_name.end()) list.erase(std::remove(list.begin(), list.end(), replaced->second), list.end());
            const std::string& type = wrapper[name];
            const bool text = type == "IFCTEXT" || type == "IFCLABEL" || type == "IFCIDENTIFIER";
            list.push_back(emitter_.emit("IFCPROPERTYSINGLEVALUE('" + name + "',$," + type + "(" + (text ? "'" + combined + "'" : combined) + "),$);"));
            ++plan_.quantities_combined;
        }
    }

    // asset_key is the site, or the site and its group if the site is in groups, and makes the asset's GUIDs
    void merge(const std::string& site, const std::string& label, const std::string& asset_key, const std::string& note, const std::vector<Member>& candidates) {
        const Rigid world_to_asset = inverse(candidates[0].placement);
        std::vector<const Member*> members;
        std::vector<std::pair<uint32_t, Rigid>> items;
        for (const Member& member : candidates) {
            std::vector<std::pair<uint32_t, Rigid>> placed;
            if (!place_body(member, compose(world_to_asset, member.placement), placed)) {
                ++plan_.left_out_other;
                continue;
            }
            members.push_back(&member);
            items.insert(items.end(), placed.begin(), placed.end());
        }
        if (members.size() < 2) return;

        std::vector<uint32_t> mapped_items;
        for (const auto& [map, target] : items) {
            const uint32_t place = emitter_.emit_operator(target);
            mapped_items.push_back(emitter_.emit("IFCMAPPEDITEM(" + reference(map) + "," + reference(place) + ");"));
            if (map < spf_.max_id()) plan_.keep.push_back(map);
        }

        const auto first = attributes_of(spf_.line(members[0]->id));
        const std::string owner(first[1]);
        if (owner_ == "$") owner_ = owner;
        const auto context = attributes_of(spf_.line(members[0]->bodies[0]))[0];
        for (const uint32_t id : refs_in(context)) plan_.keep.push_back(id);
        for (const uint32_t id : refs_in(first[1])) plan_.keep.push_back(id);
        for (const uint32_t id : refs_in(first[5])) plan_.keep.push_back(id);

        const uint32_t representation = emitter_.emit("IFCSHAPEREPRESENTATION(" + std::string(context) + ",'Body','MappedRepresentation'," + list_of(mapped_items) + ");");
        const uint32_t shape = emitter_.emit("IFCPRODUCTDEFINITIONSHAPE($,$,(" + reference(representation) + "));");
        const std::string guid = derived_guid(asset_key + "@" + stem_);
        const uint32_t element = emitter_.emit("IFCBUILDINGELEMENTPROXY('" + guid + "'," + owner + ",'" + site + "',$,'" + site + "'," + std::string(first[5]) + "," + reference(shape) + ",$,$);");

        // An opening cuts the asset now, as what it cut is part of it. It reaches into other parts only where
        // they meet what it cut, such as a bolt in its hole, which was measured to be all it takes on gantries.
        for (const Member* member : members) {
            const auto voids = voids_of_.find(member->id);
            if (voids == voids_of_.end()) continue;
            for (const uint32_t relationship : voids->second) {
                const auto parts = attributes_of(spf_.line(relationship));
                const auto opening = refs_in(parts[5]);
                if (opening.size() != 1 || gone_(opening[0])) continue;
                emitter_.emit("IFCRELVOIDSELEMENT('" + derived_guid(quoted_in(parts[0]) + "@" + stem_ + "/" + asset_key) + "'," + std::string(parts[1]) + "," + std::string(parts[2]) + "," +
                              std::string(parts[3]) + "," + reference(element) + "," + reference(opening[0]) + ");");
                for (const uint32_t id : refs_in(parts[1])) plan_.keep.push_back(id);
                ++plan_.openings_moved;
            }
        }

        // The asset keeps what's true of all of it
        std::map<std::string, size_t> counted;
        std::vector<std::pair<std::string_view, uint32_t>> first_seen;
        for (const Member* member : members) {
            std::unordered_set<std::string> here;
            const auto sets = sets_of_.find(member->id);
            if (sets == sets_of_.end()) continue;
            for (const uint32_t id : sets->second) {
                const PropertySet& set = property_set(id);
                for (const auto& [text, property] : set.properties) {
                    std::string key(set.name);
                    key.append(text);
                    if (!here.insert(key).second) continue;
                    if (++counted[key] == 1 && member == members[0]) first_seen.emplace_back(set.name, property);
                }
            }
        }
        std::map<std::string_view, std::vector<uint32_t>> kept;
        for (const auto& [name, property] : first_seen) {
            const auto line = spf_.line(property);
            std::string key(name);
            key.append(line.substr(line.find('=')));
            if (counted[key] == members.size()) kept[name].push_back(property);
        }
        combine_quantities(members, kept);
        for (const auto& [name, properties] : kept) {
            const std::string base = asset_key + "@" + stem_ + "/" + quoted_in(name);
            const uint32_t set = emitter_.emit("IFCPROPERTYSET('" + derived_guid(base) + "'," + owner + "," + std::string(name) + ",$," + list_of(properties) + ");");
            emitter_.emit("IFCRELDEFINESBYPROPERTIES('" + derived_guid(base + "/defines") + "'," + owner + ",$,$,(" + reference(element) + ")," + reference(set) + ");");
            plan_.keep.insert(plan_.keep.end(), properties.begin(), properties.end());
        }

        for (const Member* member : members) {
            uint32_t at = member->id;
            for (auto whole = whole_of_.find(at); whole != whole_of_.end() && !container_.count(at); whole = whole_of_.find(at)) at = whole->second;
            const auto structure = container_.find(at);
            if (structure != container_.end()) {
                if (by_site_) levels_[site].emplace_back(element, structure->second);
                else contained_[structure->second].push_back(element);
                break;
            }
        }

        const size_t asset = plan_.assets.size();
        plan_.assets.push_back({site, guid, label, note});
        for (const Member* member : members) {
            plan_.merged_into[member->id] = asset;
            if (by_design_.count(member->id)) ++plan_.by_design;
        }
    }

    const SpfFile& spf_;
    const std::function<bool(uint32_t)>& gone_;
    const std::string& stem_;
    MergePlan& plan_;
    Emitter emitter_;
    Extents extents_;
    double unit_ = 0;  // metres in the model's length unit; 0 if not known
    double gap_ = 0;  // in the model's length unit; 0 doesn't check
    std::string owner_ = "$";
    std::unordered_map<uint32_t, std::vector<uint32_t>> sets_of_, parts_of_;
    std::unordered_map<uint32_t, uint32_t> container_, whole_of_, maps_;
    std::unordered_set<uint32_t> by_design_;  // elements that are in their asset by their design tag
    bool by_site_ = false;                    // the asset being made is a site's
    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> levels_;  // site, and what goes on its level, with where it was
    std::vector<uint32_t> containments_;      // relationships that hold elements on levels
    std::unordered_set<uint32_t> held_;       // structures they hold elements on
    std::unordered_map<uint32_t, uint32_t> aggregation_of_;  // whole, and the relationship of its parts
    std::unordered_map<uint32_t, std::vector<uint32_t>> voids_of_;  // element, and the relationships of the openings that cut it
    std::unordered_map<uint32_t, PropertySet> property_sets_;
    std::map<uint32_t, std::vector<uint32_t>> contained_;
};

}  // namespace

MergePlan plan_merge(const SpfFile& spf, const std::vector<uint32_t>& candidates, const std::function<bool(uint32_t)>& gone, const std::string& stem,
                     double gap_m) {
    MergePlan plan;
    Planner(spf, gone, stem, gap_m, plan).run(candidates);
    std::sort(plan.keep.begin(), plan.keep.end());
    plan.keep.erase(std::unique(plan.keep.begin(), plan.keep.end()), plan.keep.end());
    return plan;
}

}  // namespace bimpass

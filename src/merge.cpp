#include "merge.h"

#include "extent.h"
#include "rigid.h"
#include "site.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
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
    std::string site;
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
        plan_.gap_checked = gap_m <= 0 || unit > 0;
        gap_ = unit > 0 ? gap_m / unit : 0;
    }

    void run(const std::vector<uint32_t>& candidates) {
        const std::unordered_set<uint32_t> candidate(candidates.begin(), candidates.end());
        read_relationships(candidate);

        // Ordered, so that a run writes the same file every time. Parts that can't merge stay in, as what
        // can join the rest.
        std::map<std::string, std::vector<Member>> sites;
        for (const uint32_t id : candidates) {
            const std::string site = site_of(id);
            if (!is_one_site(site)) continue;
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
            sites[site].push_back(std::move(member));
        }
        for (auto& [site, members] : sites) {
            if (std::count_if(members.begin(), members.end(), [](const Member& m) { return m.can_merge; }) < 2) continue;
            const auto groups = groups_of(site, members);
            if (groups.size() > 1) ++plan_.sites_apart;
            for (size_t i = 0; i < groups.size(); ++i) {
                const std::string note = groups.size() > 1 ? ", group " + std::to_string(i + 1) + " of " + std::to_string(groups.size()) + ", far apart" : "";
                merge(site, i == 0 ? site : site + "/" + std::to_string(i + 1), note, groups[i]);
            }
        }
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
    // A site's parts that can merge, in groups that are each one thing: every part is within the gap of
    // another part of its group, merging or not. A part on its own stays as it is.
    std::vector<std::vector<Member>> groups_of(const std::string& site, const std::vector<Member>& members) {
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
            else if (all.size() > 1) plan_.apart[group[0].id] = site;
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
                for (const uint32_t element : refs_in(attributes[4])) container_[element] = structure[0];
            } else if (type == "IFCRELAGGREGATES") {
                const auto attributes = attributes_of(spf_.line(id));
                if (attributes.size() != 6) continue;
                const auto whole = refs_in(attributes[4]);
                if (whole.size() != 1) continue;
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
            if (!SITE_PSETS.count(name)) continue;
            const auto parts = attributes_of(line);
            if (parts.size() > 2 && quoted_in(parts[0]) == SITE_PROPERTY) set.site = quoted_in(parts[2]);
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

    // asset_key is the site, or the site and its group if the site is in groups, and makes the asset's GUIDs
    void merge(const std::string& site, const std::string& asset_key, const std::string& note, const std::vector<Member>& candidates) {
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
                contained_[structure->second].push_back(element);
                break;
            }
        }

        const size_t asset = plan_.assets.size();
        plan_.assets.push_back({site, guid, note});
        for (const Member* member : members) plan_.merged_into[member->id] = asset;
    }

    const SpfFile& spf_;
    const std::function<bool(uint32_t)>& gone_;
    const std::string& stem_;
    MergePlan& plan_;
    Emitter emitter_;
    Extents extents_;
    double gap_ = 0;  // in the model's length unit; 0 doesn't check
    std::string owner_ = "$";
    std::unordered_map<uint32_t, std::vector<uint32_t>> sets_of_, parts_of_;
    std::unordered_map<uint32_t, uint32_t> container_, whole_of_, maps_;
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

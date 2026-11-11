#include "merge.h"

#include "bake.h"
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

namespace ifcfilter {
namespace {
constexpr double RUN_GAP_M = 0.05;  // parts of a run closer than this touch

// What's between a string's quotes, wherever it is in a value such as IFCTEXT('X')
std::string quoted_in(std::string_view value) {
    const auto open = value.find('\'');
    const auto close = value.rfind('\'');
    return open == std::string_view::npos || close <= open ? std::string() : std::string(value.substr(open + 1, close - open - 1));
}

// A string as STEP writes it, quotes and all
std::string quoted(const std::string& text) {
    std::string out = "'";
    for (const char c : text) {
        out += c;
        if (c == '\'') out += '\'';
    }
    return out + "'";
}

struct PropertySet {
    std::string_view name;  // with its quotes, as it's written
    std::vector<std::pair<std::string_view, uint32_t>> properties;  // each one's text, and its id
    std::string site, design, mark;
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
            bool run = false;
            if (!is_one_site(key) && !NOT_A_SITE.count(key) && schema().merge_runs.count(asset_type_of(key))) {
                // A placeholder that stands for a kind of thing, such as a pipe run's: what touches goes together
                asset = {key, "run of " + key, {}};
                key = "run " + key;
                run = true;
                ++plan_.in_runs;
            }
            if (!run && !is_one_site(key)) {
                const std::string tag = design_of(id);
                if (tag.empty() || !is_id(tag)) continue;
                const auto owners = sites_of_tag.find(tag);
                if (owners != sites_of_tag.end() && owners->second.size() == 1) {
                    key = *owners->second.begin();
                    asset = {key, "site ID " + key, {}};
                    ++plan_.joined_by_design;
                } else {
                    // A tag can name a type of piece, as a deck unit's does: pieces with marks of their own
                    // are each one thing, so only those with the same mark, or none, go together
                    const std::string mark = mark_of(id);
                    key = "design " + tag + (mark.empty() ? "" : "\n" + mark);
                    asset = {tag, "design tag " + tag + (mark.empty() ? "" : ", mark " + mark), {}};
                    if (!mark.empty()) ++plan_.by_mark;
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
            // A run is what touches, so it's only a hair apart, not a site's few metres
            const bool run = asset.label.rfind("run of ", 0) == 0;
            const double site_gap = gap_;
            if (run && gap_ > 0) gap_ = RUN_GAP_M / unit_;
            const auto groups = groups_of(asset.label, members);
            gap_ = site_gap;
            if (run) plan_.runs += groups.size();
            if (groups.size() > 1 && !run) ++plan_.sites_apart;
            for (size_t i = 0; i < groups.size(); ++i) {
                const std::string note = groups.size() > 1 ? ", group " + std::to_string(i + 1) + " of " + std::to_string(groups.size()) + ", far apart" : "";
                merge(asset.name, asset.label, i == 0 ? key : key + "/" + std::to_string(i + 1), note, groups[i]);
            }
        }
        // An assembly has no shape of its own, so it's nothing once all its parts have gone. This goes before
        // the levels, so none holds it.
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
        // What has a site and wasn't merged goes on its site's level too
        for (const auto& [key, asset] : sites) {
            if (asset.label.rfind("site ID ", 0) != 0) continue;
            for (const Member& member : asset.members) {
                const auto structure = container_.find(member.id);
                if (!plan_.merged_into.count(member.id) && structure != container_.end()) levels_[asset.name].emplace_back(member.id, structure->second);
            }
        }
        // What has a site but couldn't be a member, such as an assembly, goes on its site too
        std::unordered_set<uint32_t> placed;
        for (const auto& [site, entries] : levels_) {
            for (const auto& [element, structure] : entries) placed.insert(element);
        }
        std::vector<std::pair<uint32_t, uint32_t>> contained(container_.begin(), container_.end());
        std::sort(contained.begin(), contained.end());
        for (const auto& [element, structure] : contained) {
            if (placed.count(element) || plan_.merged_into.count(element) || !candidate.count(element)) continue;
            const std::string site = site_of(element);
            if (!is_one_site(site)) continue;
            levels_[site].emplace_back(element, structure);
        }
        make_levels();

        // Finish what's only known once every asset has been made
        for (const auto& [structure, elements] : contained_) {
            emitter_.emit("IFCRELCONTAINEDINSPATIALSTRUCTURE('" + derived_guid("contains" + reference(structure) + "@" + stem_) + "'," + owner_ + ",$,$," + list_of(elements) + "," +
                          reference(structure) + ");");
            plan_.keep.push_back(structure);
        }
        plan_.lines = std::move(emitter_.lines());
    }

  private:
    // Under each building, a level for each asset type holding its sites, and one last level for what has
    // no site ID. A site that's one element or asset is that element, renamed after its site; one still in
    // several pieces is a level of its own on its type's level. The building's own levels go once they're
    // empty, as everything on them has moved.
    void make_levels() {
        struct Levels {
            std::map<std::string, std::vector<std::pair<std::string, uint32_t>>> elements_of_type;  // type, and its sites' elements, by site
            std::map<std::string, std::vector<std::pair<std::string, uint32_t>>> sites_of_type;     // type, and the levels of its sites in pieces
            std::vector<uint32_t> unmanaged;
        };
        std::map<uint32_t, Levels> of_building;
        std::unordered_set<uint32_t> moved;
        const auto owner_of = [&](uint32_t building) { return owner_ != "$" ? owner_ : std::string(attributes_of(spf_.line(building))[1]); };
        // Each level is placed where its building is, by a placement of its own: a viewer goes from what's clicked
        // to what it's on by placements, and one shared by a building and its levels can't say which
        const auto placement_of_building = [&](uint32_t building) {
            return reference(emitter_.emit("IFCLOCALPLACEMENT(" + std::string(attributes_of(spf_.line(building))[5]) + "," + reference(emitter_.emit_identity()) + ");"));
        };
        for (const auto& [site, entries] : levels_) {
            const uint32_t building = building_of(entries[0].second);
            if (!building) {
                // Nowhere to put a level: assets go where their first part was, and the rest stay
                for (const auto& [element, structure] : entries) {
                    if (element >= spf_.max_id()) contained_[structure].push_back(element);
                }
                continue;
            }
            Levels& levels = of_building[building];
            const std::string type = asset_type_of(site);
            std::vector<uint32_t> elements;
            for (const auto& [element, structure] : entries) {
                elements.push_back(element);
                if (element < spf_.max_id()) moved.insert(element);
            }
            if (elements.size() == 1) {
                const uint32_t element = elements[0];
                if (element < spf_.max_id()) {
                    const auto line = spf_.line(element);
                    auto attributes = attributes_of(line);
                    if (attributes.size() > 2) {
                        const std::string name = "'" + site + "'";
                        attributes[2] = name;
                        std::string text(line.substr(0, line.find('(') + 1));
                        for (size_t i = 0; i < attributes.size(); ++i) text.append(i ? "," : "").append(attributes[i]);
                        plan_.replace[element] = text + ");";
                    }
                }
                levels.elements_of_type[type].emplace_back(site, element);
                ++plan_.on_building;
                continue;
            }
            const std::string owner = owner_of(building);
            const std::string site_placement = placement_of_building(building);
            const uint32_t level = emitter_.emit("IFCBUILDINGSTOREY('" + derived_guid("level/" + site + "@" + stem_) + "'," + owner + ",'" + site + "',$,$," +
                                                 site_placement + ",$,$,.PARTIAL.,$);");
            for (const uint32_t element : elements) place_on(element, site_placement, building);
            emitter_.emit("IFCRELCONTAINEDINSPATIALSTRUCTURE('" + derived_guid("level/" + site + "@" + stem_ + "/contains") + "'," + owner + ",$,$," + list_of(elements) + "," +
                          reference(level) + ");");
            levels.sites_of_type[type].emplace_back(site, level);
            ++plan_.levels;
            plan_.on_levels += elements.size();
        }

        // Every building that holds anything has levels, even with no site in it, so a model whose elements have
        // no site IDs comes out like the rest, with them on Non-Managed Types, not on Revit's DATUM
        for (const uint32_t relationship : containments_) {
            const auto attributes = attributes_of(spf_.line(relationship));
            const auto structure = attributes.size() == 6 ? refs_in(attributes[5]) : std::vector<uint32_t>{};
            if (structure.size() != 1) continue;
            const auto type = spf_.type(structure[0]);
            if (type != "IFCBUILDINGSTOREY" && type != "IFCBUILDING") continue;
            const uint32_t building = building_of(structure[0]);
            if (building) of_building[building];
        }
        // On a building that's been given levels, what's on its own levels, or on it, and has no site, goes on
        // the level for that. What moved, or went with a merge, just leaves the relationship that held it.
        const auto planned_building = [&](uint32_t structure) -> Levels* {
            const auto type = spf_.type(structure);
            if (type != "IFCBUILDINGSTOREY" && type != "IFCBUILDING") return nullptr;
            const auto found = of_building.find(building_of(structure));
            return found == of_building.end() ? nullptr : &found->second;
        };
        std::map<uint32_t, size_t> left_on;  // structure, and what's still on it
        for (const uint32_t relationship : containments_) {
            const auto attributes = attributes_of(spf_.line(relationship));
            const auto elements = refs_in(attributes[4]);
            const auto structure = refs_in(attributes[5]);
            Levels* levels = structure.size() == 1 ? planned_building(structure[0]) : nullptr;
            std::vector<uint32_t> kept;
            for (const uint32_t element : elements) {
                if (moved.count(element)) continue;
                if (levels) {
                    if (!gone_(element) && !plan_.merged_into.count(element)) levels->unmanaged.push_back(element);
                    continue;
                }
                kept.push_back(element);
            }
            for (const uint32_t element : kept) {
                if (!gone_(element) && !plan_.merged_into.count(element) && structure.size() == 1) ++left_on[structure[0]];
            }
            if (kept.size() == elements.size()) continue;
            plan_.replace[relationship] = kept.empty() ? std::string()
                : reference(relationship) + "=IFCRELCONTAINEDINSPATIALSTRUCTURE(" + std::string(attributes[0]) + "," + std::string(attributes[1]) + "," + std::string(attributes[2]) + "," +
                      std::string(attributes[3]) + "," + list_of(kept) + "," + std::string(attributes[5]) + ");";
        }
        // Assets with no site, merged by design tag, go with them
        for (auto at = contained_.begin(); at != contained_.end();) {
            Levels* levels = planned_building(at->first);
            if (!levels) {
                left_on[at->first] += at->second.size();
                ++at;
                continue;
            }
            levels->unmanaged.insert(levels->unmanaged.end(), at->second.begin(), at->second.end());
            at = contained_.erase(at);
        }

        for (auto& [building, levels] : of_building) {
            const std::string owner = owner_of(building);
            // Each building has a level of each type, so where there are several, a level's GUID is made from its
            // building's too. A model of one building keeps the GUIDs it had.
            const std::string in_building = of_building.size() > 1 ? "/" + quoted_in(attributes_of(spf_.line(building))[0]) : std::string();
            std::vector<uint32_t> children;
            std::set<std::string> types;
            for (const auto& [type, elements] : levels.elements_of_type) types.insert(type);
            for (const auto& [type, sites] : levels.sites_of_type) types.insert(type);
            for (const std::string& type : types) {
                const std::string base = "type/" + type + "@" + stem_ + in_building;
                const std::string placement = placement_of_building(building);
                const uint32_t level = emitter_.emit("IFCBUILDINGSTOREY('" + derived_guid(base) + "'," + owner + "," + quoted(type) + ",$,$," + placement + ",$,$,.ELEMENT.,$);");
                auto elements = levels.elements_of_type[type];
                std::sort(elements.begin(), elements.end());
                if (!elements.empty()) {
                    std::vector<uint32_t> ids;
                    for (const auto& [site, element] : elements) {
                        ids.push_back(element);
                        place_on(element, placement, building);
                    }
                    emitter_.emit("IFCRELCONTAINEDINSPATIALSTRUCTURE('" + derived_guid(base + "/contains") + "'," + owner + ",$,$," + list_of(ids) + "," + reference(level) + ");");
                }
                auto sites = levels.sites_of_type[type];
                std::sort(sites.begin(), sites.end());
                if (!sites.empty()) {
                    std::vector<uint32_t> ids;
                    for (const auto& [site, site_level] : sites) ids.push_back(site_level);
                    emitter_.emit("IFCRELAGGREGATES('" + derived_guid(base + "/sites") + "'," + owner + ",$,$," + reference(level) + "," + list_of(ids) + ");");
                }
                children.push_back(level);
                ++plan_.type_levels;
            }
            if (!levels.unmanaged.empty()) {
                const std::string base = "unmanaged@" + stem_ + in_building;
                const std::string placement = placement_of_building(building);
                for (const uint32_t element : levels.unmanaged) place_on(element, placement, building);
                const uint32_t level = emitter_.emit("IFCBUILDINGSTOREY('" + derived_guid(base) + "'," + owner + "," + quoted(UNMANAGED_LEVEL) + ",$,$," + placement +
                                                     ",$,$,.ELEMENT.,$);");
                emitter_.emit("IFCRELCONTAINEDINSPATIALSTRUCTURE('" + derived_guid(base + "/contains") + "'," + owner + ",$,$," + list_of(levels.unmanaged) + "," + reference(level) + ");");
                children.push_back(level);
                plan_.unmanaged += levels.unmanaged.size();
            }
            // The building's own levels stay only if something's still on them, or they're made of something, such as spaces
            std::vector<std::pair<std::string, uint32_t>> others;
            const auto existing = parts_of_.find(building);
            if (existing != parts_of_.end()) {
                for (const uint32_t child : existing->second) {
                    if (spf_.type(child) == "IFCBUILDINGSTOREY" && !left_on[child] && !parts_of_.count(child)) {
                        plan_.dropped.push_back(child);
                        continue;
                    }
                    others.emplace_back(quoted_in(attributes_of(spf_.line(child))[2]), child);
                }
            }
            std::stable_sort(others.begin(), others.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [name, child] : others) children.push_back(child);
            const auto aggregation = aggregation_of_.find(building);
            if (aggregation != aggregation_of_.end()) {
                const auto attributes = attributes_of(spf_.line(aggregation->second));
                plan_.replace[aggregation->second] = reference(aggregation->second) + "=IFCRELAGGREGATES(" + std::string(attributes[0]) + "," + std::string(attributes[1]) + "," +
                                                     std::string(attributes[2]) + "," + std::string(attributes[3]) + "," + reference(building) + "," + list_of(children) + ");";
                // Its levels may all be new now, and a relationship is otherwise only kept for what it held
                plan_.keep.push_back(aggregation->second);
            } else {
                emitter_.emit("IFCRELAGGREGATES('" + derived_guid("levels" + reference(building) + "@" + stem_) + "'," + owner + ",$,$," + reference(building) + "," + list_of(children) + ");");
            }
        }
    }

    // Places an element relative to the level it's now on, where it was: IFC places what's on a level from the
    // level, and the levels it was placed from are gone. Its own placement is changed, so that what's placed from
    // it, such as its ports and openings, still is; one that other products share is left, and it gets a new
    // one. One placed in a way that can't be read stays as it was.
    void place_on(uint32_t element, const std::string& level_placement, uint32_t building) {
        std::string* emitted = emitter_.line_of(element);
        const auto replaced = plan_.replace.find(element);
        const std::string line = emitted ? *emitted : replaced != plan_.replace.end() ? replaced->second : std::string(spf_.line(element));
        if (line.empty()) return;
        auto attributes = attributes_of(line);
        if (attributes.size() < 6) return;
        const auto world = placement_of(attributes[5]);
        const auto origin = placement_of(attributes_of(spf_.line(building))[5]);
        const auto own = refs_in(attributes[5]);
        if (!world || !origin || own.size() != 1) return;
        const uint32_t axes = emitter_.emit_placement(compose(inverse(*origin), *world));
        const std::string text = "IFCLOCALPLACEMENT(" + level_placement + "," + reference(axes) + ");";
        if (products_placed_by(own[0]) <= 1 && !plan_.replace.count(own[0])) {
            plan_.replace[own[0]] = reference(own[0]) + "=" + text;
        } else {
            attributes[5] = reference(emitter_.emit(text));
            std::string rewritten(line.substr(0, line.find('(') + 1));
            for (size_t i = 0; i < attributes.size(); ++i) rewritten.append(i ? "," : "").append(attributes[i]);
            rewritten += ");";
            if (emitted) *emitted = rewritten;
            else plan_.replace[element] = rewritten;
        }
        ++plan_.placed_on_levels;
    }

    // How many lines that aren't placements refer to a placement: products, mostly
    size_t products_placed_by(uint32_t placement) {
        if (placed_by_.empty()) {
            placed_by_.assign(spf_.max_id(), 0);
            for (uint32_t id = 0; id < spf_.max_id(); ++id) {
                const auto line = spf_.line(id);
                if (line.empty() || spf_.type(id) == "IFCLOCALPLACEMENT") continue;
                for_each_ref(line, [&](uint32_t next) {
                    if (next < placed_by_.size() && spf_.type(next) == "IFCLOCALPLACEMENT") ++placed_by_[next];
                });
            }
        }
        return placement < placed_by_.size() ? placed_by_[placement] : 0;
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
            } else if (type == "IFCRELNESTS" || type == "IFCRELCONNECTSPORTTOELEMENT") {
                // A port is nested in what it's on, or in IFC2X3 connected to it
                const auto attributes = attributes_of(spf_.line(id));
                if (attributes.size() != 6) continue;
                const bool nests = type == "IFCRELNESTS";
                const auto host = refs_in(attributes[nests ? 4 : 5]);
                if (host.size() != 1 || !candidate.count(host[0])) continue;
                for (const uint32_t port : refs_in(attributes[nests ? 5 : 4])) {
                    if (spf_.type(port) == "IFCDISTRIBUTIONPORT") ports_of_[host[0]].push_back(port);
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
            const Schema& s = schema();
            if (!s.site_sets.count(name) && !s.design_sets.count(name) && !s.mark_sets.count(name)) continue;
            const auto parts = attributes_of(line);
            if (parts.size() <= 2) continue;
            const std::string property_name = quoted_in(parts[0]);
            if (s.site_sets.count(name) && property_name == s.site_property) set.site = quoted_in(parts[2]);
            if (s.design_sets.count(name) && property_name == s.design_property) set.design = quoted_in(parts[2]);
            if (s.mark_sets.count(name) && property_name == s.mark_property) set.mark = quoted_in(parts[2]);
        }
        return set;
    }

    // Blank if it has none. The first site-set is preferred, though they've not been seen to differ.
    std::string site_of(uint32_t element) {
        std::string site;
        const auto sets = sets_of_.find(element);
        if (sets == sets_of_.end()) return site;
        for (const uint32_t id : sets->second) {
            const PropertySet& set = property_set(id);
            if (set.site.empty()) continue;
            if (quoted_in(set.name) == schema().site_set_preferred) return set.site;
            site = set.site;
        }
        return site;
    }

    // Blank if it has none. The first design-set is preferred, though they've not been seen to differ.
    std::string design_of(uint32_t element) {
        std::string design;
        const auto sets = sets_of_.find(element);
        if (sets == sets_of_.end()) return design;
        for (const uint32_t id : sets->second) {
            const PropertySet& set = property_set(id);
            if (set.design.empty()) continue;
            if (quoted_in(set.name) == schema().design_set_preferred) return set.design;
            design = set.design;
        }
        return design;
    }

    // Blank if it has none, or one that says nothing, such as N/A
    std::string mark_of(uint32_t element) {
        std::string mark;
        const auto sets = sets_of_.find(element);
        if (sets == sets_of_.end()) return mark;
        for (const uint32_t id : sets->second) {
            const PropertySet& set = property_set(id);
            if (set.mark.empty() || NOT_A_SITE.count(set.mark)) continue;
            if (quoted_in(set.name) == schema().mark_set_preferred) return set.mark;
            mark = set.mark;
        }
        return mark;
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

    // A representation of a member's, and where it goes in the asset
    struct Placed {
        uint32_t map = 0;             // the map that placed it, if the member's body was mapped items; 0 for the body itself
        uint32_t representation = 0;  // what's placed
        Rigid target;                 // for a mapped item of the map, or of the body
        std::optional<Rigid> to;      // what moves the representation's own coordinates there; none if its map's origin can't be read
    };

    // What puts a member's body where it was. False if it can't be.
    bool place_body(const Member& member, const Rigid& relative, std::vector<Placed>& out) {
        for (const uint32_t body : member.bodies) {
            const auto attributes = attributes_of(spf_.line(body));
            bool mapped = attributes[2] == "'MappedRepresentation'";
            std::vector<Placed> items;
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
                    const auto held = attributes_of(spf_.line(map[0]));
                    const auto origin_ids = held.size() == 2 ? refs_in(held[0]) : std::vector<uint32_t>{};
                    const auto origin = origin_ids.size() == 1 ? axis_placement(spf_, origin_ids[0]) : std::nullopt;
                    const auto representation = held.size() == 2 ? refs_in(held[1]) : std::vector<uint32_t>{};
                    if (representation.size() != 1) return false;
                    const Rigid target_here = compose(relative, *placed);
                    items.push_back({map[0], representation[0], target_here, origin ? std::optional<Rigid>(compose(target_here, inverse(*origin))) : std::nullopt});
                }
            }
            if (!mapped) {
                items.clear();
                items.push_back({0, body, relative, relative});
            }
            out.insert(out.end(), items.begin(), items.end());
        }
        return !out.empty();
    }

    // The quantity set for the asset as a whole: its height, width and length are those of all of it, in metres, as
    // for a gantry, the structure's, and other numbers that differ part to part are added up. Length is the
    // longer way across in plan, width the shorter, height up. What isn't a number, and differs, is left out.
    void combine_quantities(const std::vector<const Member*>& members, std::map<std::string_view, std::vector<uint32_t>>& kept) {
        const Schema& s = schema();
        if (s.quantity_set.empty()) return;
        std::string_view set_name;
        std::map<std::string, std::vector<std::string>> values;  // property, and its value on each part that has it
        std::map<std::string, std::string> wrapper;              // property, and the type its value is written as
        for (const Member* member : members) {
            const auto sets = sets_of_.find(member->id);
            if (sets == sets_of_.end()) continue;
            for (const uint32_t id : sets->second) {
                const PropertySet& set = property_set(id);
                if (quoted_in(set.name) != s.quantity_set) continue;
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
            {s.length_property, std::max(across_x, across_y)}, {s.width_property, std::min(across_x, across_y)}, {s.height_property, (box.high[2] - box.low[2]) * unit_}};

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
        std::vector<Placed> items;
        for (const Member& member : candidates) {
            std::vector<Placed> placed;
            if (!place_body(member, compose(world_to_asset, member.placement), placed)) {
                ++plan_.left_out_other;
                continue;
            }
            members.push_back(&member);
            items.insert(items.end(), placed.begin(), placed.end());
        }
        if (members.size() < 2) return;

        // The body is the parts' shapes, moved to where they were, so that clicking any of it selects the asset.
        // If anything can't be moved, it's a mapped item of each part's body instead, as it was.
        std::string why_not;
        for (const Placed& placed : items) {
            if (!placed.to) why_not = "a map placed from an origin that isn't a 3D axis placement";
            else if (!baker_.can_move(placed.representation)) why_not = baker_.why_not();
            if (!why_not.empty()) break;
        }
        std::vector<uint32_t> body_items;
        std::string representation_type = "MappedRepresentation";
        if (why_not.empty()) {
            for (const Placed& placed : items) baker_.move(placed.representation, *placed.to, body_items, plan_.keep);
            representation_type = baker_.representation_type(body_items);
            ++plan_.assets_plain;
        } else {
            for (const Placed& placed : items) {
                const uint32_t map = placed.map ? placed.map : map_of(placed.representation);
                const uint32_t place = emitter_.emit_operator(placed.target);
                body_items.push_back(emitter_.emit("IFCMAPPEDITEM(" + reference(map) + "," + reference(place) + ");"));
                if (map < spf_.max_id()) plan_.keep.push_back(map);
            }
            ++plan_.assets_mapped[why_not];
        }

        const auto first = attributes_of(spf_.line(members[0]->id));
        const std::string owner(first[1]);
        if (owner_ == "$") owner_ = owner;
        const auto context = attributes_of(spf_.line(members[0]->bodies[0]))[0];
        for (const uint32_t id : refs_in(context)) plan_.keep.push_back(id);
        for (const uint32_t id : refs_in(first[1])) plan_.keep.push_back(id);
        for (const uint32_t id : refs_in(first[5])) plan_.keep.push_back(id);

        const uint32_t representation =
            emitter_.emit("IFCSHAPEREPRESENTATION(" + std::string(context) + ",'Body','" + representation_type + "'," + list_of(body_items) + ");");
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

        // Its parts' ports are on the asset now, where they were, rather than left on nothing
        std::vector<uint32_t> ports;
        for (const Member* member : members) {
            const auto on = ports_of_.find(member->id);
            if (on == ports_of_.end()) continue;
            for (const uint32_t port : on->second) {
                if (!gone_(port)) ports.push_back(port);
            }
        }
        if (!ports.empty()) {
            emitter_.emit("IFCRELNESTS('" + derived_guid(asset_key + "@" + stem_ + "/ports") + "'," + owner + ",'NestedPorts',$," + reference(element) + "," + list_of(ports) + ");");
            plan_.ports_moved += ports.size();
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
    Baker baker_{spf_, emitter_, gone_};
    Extents extents_;
    double unit_ = 0;  // metres in the model's length unit; 0 if not known
    double gap_ = 0;  // in the model's length unit; 0 doesn't check
    std::string owner_ = "$";
    std::unordered_map<uint32_t, std::vector<uint32_t>> sets_of_, parts_of_;
    std::unordered_map<uint32_t, uint32_t> container_, whole_of_, maps_;
    std::unordered_set<uint32_t> by_design_;  // elements that are in their asset by their design tag
    bool by_site_ = false;                    // the asset being made is a site's
    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> levels_;  // site, and what goes on its level, with where it was
    std::vector<uint32_t> placed_by_;  // placement, and how many products and the like it places
    std::vector<uint32_t> containments_;      // relationships that hold elements on levels
    std::unordered_set<uint32_t> held_;       // structures they hold elements on
    std::unordered_map<uint32_t, uint32_t> aggregation_of_;  // whole, and the relationship of its parts
    std::unordered_map<uint32_t, std::vector<uint32_t>> ports_of_;  // element, and the ports on it
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

}  // namespace ifcfilter

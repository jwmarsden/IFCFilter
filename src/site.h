#pragma once

// Where a model says which site, and so which asset, an element belongs to.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

namespace ifcfilter {

// What a project calls the sets and properties IFCFilter reads and writes. None are named here: they come from
// the rules file (see rules.txt.example), and what isn't given is left alone.
struct Schema {
    std::set<std::string> site_sets;    // site-set: the sets that hold an element's site ID
    std::string site_set_preferred;     // the first site-set, if they differ
    std::string site_property;          // site-id
    std::string type_property;          // asset-type: corrected to what the site ID starts with
    std::string component_property;     // asset-component: an asset's parts are described as it
    std::set<std::string> design_sets;  // design-set: the sets that hold the design tag
    std::string design_set_preferred;   // the first design-set, if they differ
    std::string design_property;        // design-tag
    std::set<std::string> mark_sets;    // mark-set: the sets that hold an element's mark
    std::string mark_set_preferred;     // the first mark-set, if they differ
    std::string mark_property;          // mark: says which piece it is, so parts with a design tag but different marks are apart
    std::string zone_property;          // zone-property: an element's zone, in any set, for the IFCFilter set
    std::set<std::string> zones;        // zone: one of the zones, as N5T; anything else, such as a group of them, isn't one
    std::string location_property;      // location-property: an element's location, in any set, for the IFCFilter set
    std::string quantity_set;           // quantity-set: sizes worked out for an asset as a whole
    std::string length_property, width_property, height_property;  // quantity-length, -width, -height
    std::set<std::string> culled_sets;  // cull-set: used to work out what merges, and not wanted after
    std::string duplicate_set;          // duplicate-set: what it repeats from other sets is left out
    std::string project_set;            // project-set: where the next two are
    std::string long_name_property;     // project-long-name: the project's long name
    std::string building_property;      // building-name: the building's name
    std::string site_name;              // site-name: the site's name, rather than Revit's "Default"
};

inline Schema& schema() {
    static Schema s;
    return s;
}

// Takes one line of the rules file into the schema; false if it's another kind of rule
inline bool schema_rule(const std::string& action, const std::string& text) {
    Schema& s = schema();
    const auto add = [&](std::set<std::string>& sets, std::string& preferred) {
        if (sets.empty()) preferred = text;
        sets.insert(text);
    };
    if (action == "site-set") add(s.site_sets, s.site_set_preferred);
    else if (action == "site-id") s.site_property = text;
    else if (action == "asset-type") s.type_property = text;
    else if (action == "asset-component") s.component_property = text;
    else if (action == "design-set") add(s.design_sets, s.design_set_preferred);
    else if (action == "design-tag") s.design_property = text;
    else if (action == "mark-set") add(s.mark_sets, s.mark_set_preferred);
    else if (action == "mark") s.mark_property = text;
    else if (action == "zone-property") s.zone_property = text;
    else if (action == "zone") s.zones.insert(text);
    else if (action == "location-property") s.location_property = text;
    else if (action == "quantity-set") s.quantity_set = text;
    else if (action == "quantity-length") s.length_property = text;
    else if (action == "quantity-width") s.width_property = text;
    else if (action == "quantity-height") s.height_property = text;
    else if (action == "cull-set") s.culled_sets.insert(text);
    else if (action == "duplicate-set") s.duplicate_set = text;
    else if (action == "project-set") s.project_set = text;
    else if (action == "project-long-name") s.long_name_property = text;
    else if (action == "building-name") s.building_property = text;
    else if (action == "site-name") s.site_name = text;
    else return false;
    return true;
}

// The schema lines of a rules file, for commands that don't split
inline void load_schema(const std::string& path) {
    if (path.empty()) return;
    std::ifstream in(path);
    if (!in) throw std::runtime_error(path + ": can't be read");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto space = line.find(' ');
        if (space == std::string::npos) continue;
        const auto start = line.find_first_not_of(' ', space);
        if (start != std::string::npos) schema_rule(line.substr(0, space), line.substr(start));
    }
}

inline const std::string UNMANAGED_LEVEL = "Non-Managed Types";  // the level for what has no site ID
inline const std::set<std::string> NOT_A_SITE = {"", "N/A", "PLACEHOLDER", "Placeholder", "placeholder", "No Linear Assets Req", "-", "TBA", "TBC"};

// An ID ending in xx, such as WSU398xx, stands for a range of sites, not one asset. One ending in six zeros,
// such as ECL000000, is a class of asset with no site given yet: in LV, one covers a whole model. One with
// no digits, such as HSP, PRF or WPNXXXXXX, is a class of asset too, as is a design tag such as LO or BOLLARD.
inline bool is_one_site(const std::string& site) {
    const auto ends_with = [&](const char* end) {
        const std::string e(end);
        return site.size() > e.size() && site.compare(site.size() - e.size(), e.size(), e) == 0;
    };
    const bool numbered = std::any_of(site.begin(), site.end(), [](unsigned char c) { return std::isdigit(c); });
    return numbered && !NOT_A_SITE.count(site) && !ends_with("xx") && !ends_with("000000");
}

// A site's asset type is how its ID starts: its letters, as in PU290101, LAR2909100 or GLT290001, and RC1, RC2
// and RC3, whose number is part of the type. A placeholder's run of Xs, as in CPTXXXXXX, isn't.
inline std::string asset_type_of(const std::string& site) {
    size_t end = 0;
    while (end < site.size() && std::isalpha(static_cast<unsigned char>(site[end]))) ++end;
    std::string type = site.substr(0, end);
    if (type == "RC" && end < site.size() && site[end] >= '1' && site[end] <= '3') return type + site[end];
    size_t xs = type.size();
    while (xs > 0 && (type[xs - 1] == 'X' || type[xs - 1] == 'x')) --xs;
    if (type.size() - xs >= 3 && xs > 0) type.resize(xs);
    return type.empty() ? site : type;
}

// Where there's no site ID, the design tag can say what goes together, if it's an ID: some models give
// every element the same one, or a description, and those aren't
inline bool looks_like_an_id(const std::string& tag) {
    return is_one_site(tag) && std::none_of(tag.begin(), tag.end(), [](unsigned char c) { return std::isspace(c); });
}

}  // namespace ifcfilter

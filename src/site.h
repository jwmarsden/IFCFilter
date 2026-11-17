#pragma once

// Where a model says which site, and so which asset, an element belongs to.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
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
    std::set<std::string> zones;        // zone: one of the zones, as Z01; anything else, such as a group of them, isn't one
    std::string location_property;      // location-property: an element's location, in any set, for the IFCFilter set
    std::string discipline_property;    // discipline-property: an element's discipline, in the IFC_Filter or project set, for caches
    std::string hierarchy_set;          // hierarchy-set: the set the Hierarchy set's parent fields are read from
    std::map<std::string, std::string> hierarchy_sources;  // hierarchy-source: a Hierarchy field read from a differently named property
    std::map<std::string, double> transparent_sites;  // transparent-site: a site ID, and how transparent it's drawn, from 0 to 1
    std::map<std::string, std::string> join_across;  // join-across-models: asset types (or *) joined across combined models, and the discipline whose element keeps its own
    std::set<std::string> merge_runs;   // merge-runs: asset types whose placeholder site IDs, such as ABC000000, merge where they touch
    std::set<std::string> site_from_assembly;  // site-from-assembly: IFC classes, upper case, or *, whose parts with no site ID take theirs
    std::string quantity_set;           // quantity-set: sizes worked out for an asset as a whole
    std::string length_property, width_property, height_property;  // quantity-length, -width, -height
    std::set<std::string> culled_sets;  // cull-set: used to work out what merges, and not wanted after
    std::string duplicate_set;          // duplicate-set: what it repeats from other sets is left out
    std::string project_set;            // project-set: where the next two are
    std::string long_name_property;     // project-long-name: the project's long name
    std::string code_property;          // project-code: the project package's code, for the manifest
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
    else if (action == "discipline-property") s.discipline_property = text;
    else if (action == "hierarchy-set") s.hierarchy_set = text;
    else if (action == "hierarchy-source") {
        // FIELD PROPERTY, as hierarchy-source SafetyParent SupplyIsolationID
        const auto space = text.find(' ');
        if (space == std::string::npos) throw std::runtime_error("hierarchy-source needs a field and a property: " + text);
        std::string property = text.substr(space + 1);
        property.erase(0, std::min(property.size(), property.find_first_not_of(' ')));
        while (!property.empty() && (property.back() == ' ' || property.back() == '\r')) property.pop_back();
        s.hierarchy_sources[text.substr(0, space)] = property;
    }
    else if (action == "merge-runs") s.merge_runs.insert(text);
    else if (action == "site-from-assembly") {
        // CLASS, as site-from-assembly IfcStair, or * for any assembly
        std::string type = text;
        while (!type.empty() && (type.back() == ' ' || type.back() == '\r')) type.pop_back();
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        s.site_from_assembly.insert(type);
    }
    else if (action == "join-across-models") {
        // TYPE [DISCIPLINE], as join-across-models ABC XY: each site's element in the XY model keeps its own
        const auto space = text.find(' ');
        std::string keeper = space == std::string::npos ? std::string() : text.substr(space);
        keeper.erase(0, std::min(keeper.size(), keeper.find_first_not_of(' ')));
        while (!keeper.empty() && (keeper.back() == ' ' || keeper.back() == '\r')) keeper.pop_back();
        s.join_across[text.substr(0, space)] = keeper;
    }
    else if (action == "transparent-site") {
        // SITEID TRANSPARENCY, as transparent-site ABC123456 0.5
        const auto space = text.rfind(' ');
        if (space == std::string::npos) throw std::runtime_error("transparent-site needs a site ID and a transparency: " + text);
        const double transparency = std::stod(text.substr(space + 1));
        if (transparency < 0 || transparency > 1) throw std::runtime_error("transparent-site's transparency is from 0 to 1: " + text);
        std::string site = text.substr(0, space);
        while (!site.empty() && site.back() == ' ') site.pop_back();
        s.transparent_sites[site] = transparency;
    }
    else if (action == "quantity-set") s.quantity_set = text;
    else if (action == "quantity-length") s.length_property = text;
    else if (action == "quantity-width") s.width_property = text;
    else if (action == "quantity-height") s.height_property = text;
    else if (action == "cull-set") s.culled_sets.insert(text);
    else if (action == "duplicate-set") s.duplicate_set = text;
    else if (action == "project-set") s.project_set = text;
    else if (action == "project-long-name") s.long_name_property = text;
    else if (action == "project-code") s.code_property = text;
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
// such as ABC000000, is a class of asset with no site given yet: in LV, one covers a whole model. One with
// no digits, such as ABC or ABCXXXXXX, is a class of asset too, as is a design tag such as LO or BOLLARD.
inline bool is_one_site(const std::string& site) {
    // A site is an alphabetic asset type (RC1, RC2 and RC3 are also types), followed by exactly six digits.
    size_t at = 0;
    while (at < site.size() && std::isalpha(static_cast<unsigned char>(site[at]))) ++at;
    if (!at) return false;
    if (site.substr(0, at) == "RC" && at < site.size() && site[at] >= '1' && site[at] <= '3') ++at;
    if (site.size() - at != 6) return false;
    const auto first = site.begin() + static_cast<std::ptrdiff_t>(at);
    if (!std::all_of(first, site.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
    // A repeated digit or simple sequential number is a common placeholder, not an individual site.
    if (std::all_of(first, site.end(), [&](char c) { return c == *first; })) return false;
    const std::string digits(first, site.end());
    return digits != "123456" && digits != "654321";
}

// A site's asset type is how its ID starts: its letters, as in AB120101, ABC1209100 or XYZ120001, and RC1, RC2
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


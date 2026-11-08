#pragma once

// Where a model says which site, and so which asset, an element belongs to.

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace bimpass {

inline const std::set<std::string> SITE_PSETS = {"Asset", "Data"};  // the FD models keep everything in Data
inline const std::string SITE_PROPERTY = "SiteID";
inline const std::set<std::string> NOT_A_SITE = {"", "N/A", "PLACEHOLDER", "Placeholder", "placeholder", "No Linear Assets Req", "-", "TBA", "TBC"};

// An ID ending in xx, such as WSU398xx, stands for a range of sites, not one asset. One ending in six zeros,
// such as ECL000000, is a class of asset with no site given yet: in LV, one covers a whole model.
inline bool is_one_site(const std::string& site) {
    const auto ends_with = [&](const char* end) {
        const std::string e(end);
        return site.size() > e.size() && site.compare(site.size() - e.size(), e.size(), e) == 0;
    };
    return !NOT_A_SITE.count(site) && !ends_with("xx") && !ends_with("000000");
}

// Where there's no site ID, the design tag can say what goes together, if it's an ID: some models give
// every element the same one, or a description, and those aren't
inline const std::set<std::string> DESIGN_PSETS = {"Quantities", "Data", "General", "Model Properties"};
inline const std::string DESIGN_PROPERTY = "DesignTag";

inline bool looks_like_an_id(const std::string& tag) {
    return is_one_site(tag) && std::none_of(tag.begin(), tag.end(), [](unsigned char c) { return std::isspace(c); });
}

}  // namespace bimpass

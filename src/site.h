#pragma once

// Where a model says which site, and so which asset, an element belongs to.

#include <set>
#include <string>

namespace bimpass {

inline const std::set<std::string> SITE_PSETS = {"Asset", "Data"};  // the FD models keep everything in Data
inline const std::string SITE_PROPERTY = "SiteID";
inline const std::set<std::string> NOT_A_SITE = {"", "N/A", "PLACEHOLDER", "Placeholder", "placeholder", "No Linear Assets Req", "-"};

// An ID ending in xx, such as WSU398xx, stands for a range of sites, not one asset
inline bool is_one_site(const std::string& site) {
    const bool range = site.size() > 2 && site.compare(site.size() - 2, 2, "xx") == 0;
    return !NOT_A_SITE.count(site) && !range;
}

}  // namespace bimpass

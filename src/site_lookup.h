#pragma once
// Corrections by site ID, from data/site-id-lookup.xlsx (or a CSV of the same columns): a non-empty cell
// overrides what the models say for that site; a blank one leaves it.

#include <array>
#include <map>
#include <string>
namespace ifcfilter {
// One site's row. codes are its Uniclass Co, En, SL, EF, Ss and Pr codes, in that order; extra is every
// non-empty cell by column name, for columns the property schema lists under the same name.
struct SiteCorrection {
    std::string design, type, location, zone, identifier, description;
    std::array<std::string, 6> codes;
    std::map<std::string, std::string> extra;
};
using SiteLookup = std::map<std::string, SiteCorrection>;
// Stops with an error on a duplicate or invalid site ID, a missing column, or a Uniclass code that isn't an
// active one of the right table. An empty path gives an empty lookup.
SiteLookup load_site_lookup(const std::string& path);
}

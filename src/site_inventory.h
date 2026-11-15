#pragma once
// ifcfilter site-uniclass-inventory: which Uniclass codes the source models give each site ID, as a CSV of
// site, then one column per table, and a .review.csv beside it of conflicting or unknown codes. It reads only;
// the lookup isn't changed.

#include <string>
#include <vector>
namespace ifcfilter {
int inventory_site_uniclass(const std::vector<std::string>& inputs, const std::string& rules, const std::string& output);
}

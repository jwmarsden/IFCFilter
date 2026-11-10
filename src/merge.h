#pragma once

// Elements with the same site ID are one asset, so they can be written as one element. Its body is the
// elements' shapes, moved to where they were (see bake.h), or if any can't be, a mapped item of each
// element's body, placed where the element was.

#include "spf.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ifcfilter {

struct MergedAsset {
    std::string site, guid;  // site: its site ID, or its design tag
    std::string label;       // what it was merged by, as "site ID X" or "design tag X"
    std::string note;  // which group it is, if its site's parts are far apart
};

struct MergePlan {
    std::vector<std::string> lines;                    // new instances, numbered up from the input's last
    std::vector<uint32_t> keep;                        // instances of the input that they refer to
    std::vector<MergedAsset> assets;
    std::unordered_map<uint32_t, size_t> merged_into;  // element, and the asset it's now part of
    uint64_t openings_moved = 0;                       // openings that cut a merged part, and now cut its asset
    uint64_t left_out_other = 0;                       // placed in a way a mapped item can't repeat
    uint64_t left_out_extent = 0;                      // where its shape reaches couldn't be worked out
    uint64_t sites_apart = 0;                          // sites whose parts are in groups too far apart to be one thing
    std::unordered_map<uint32_t, std::string> apart;   // element, and its label, left as it is as nothing of its site is near it
    uint64_t by_design = 0;                            // elements with no site ID that merged by their design tag
    uint64_t joined_by_design = 0;                     // of those, ones whose tag is only on one site, so they joined it
    uint64_t by_mark = 0;                              // elements going by design tag that only go with others of the same mark
    uint64_t tags_not_ids = 0;                        // design tags on most of the model, so not taken as IDs
    bool gap_checked = true;                           // false if the model's length unit isn't known
    std::unordered_map<uint32_t, std::string> replace; // lines of the input written as this instead; empty to leave one out
    std::vector<uint32_t> dropped;                     // levels left with nothing on them, left out
    uint64_t levels = 0, on_levels = 0;                // site levels made, and the elements and assets on them
    uint64_t on_building = 0;                          // sites of one element or asset, on their type's level as themselves
    uint64_t type_levels = 0, unmanaged = 0;
    uint64_t placed_on_levels = 0;                     // elements and assets placed from the level they're now on           // levels for asset types, and what's on the level for no site ID
    uint64_t quantities_combined = 0;                  // quantity-set values worked out for assets as a whole
    uint64_t assets_plain = 0;                         // assets whose body is their parts' shapes, moved
    std::map<std::string, uint64_t> assets_mapped;     // assets left as mapped items of their parts, by the kind of shape that couldn't be moved
};

// candidates are the elements that could merge; gone says what isn't in the file being written. Parts of a site
// are only merged if they're within gap_m of each other, through any chain of its parts; 0 doesn't check.
MergePlan plan_merge(const SpfFile& spf, const std::vector<uint32_t>& candidates, const std::function<bool(uint32_t)>& gone, const std::string& stem,
                     double gap_m);

}  // namespace ifcfilter

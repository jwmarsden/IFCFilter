#pragma once

// Elements with the same site ID are one asset, so they can be written as one element. Geometry is
// left as it is: each element's body becomes a mapped item of the merged element, placed where the
// element was.

#include "spf.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bimpass {

struct MergedAsset {
    std::string site, guid;
};

struct MergePlan {
    std::vector<std::string> lines;                    // new instances, numbered up from the input's last
    std::vector<uint32_t> keep;                        // instances of the input that they refer to
    std::vector<MergedAsset> assets;
    std::unordered_map<uint32_t, size_t> merged_into;  // element, and the asset it's now part of
    uint64_t left_out_openings = 0;                    // a void on a merged element would cut all of it
    uint64_t left_out_other = 0;                       // placed in a way a mapped item can't repeat
};

// candidates are the elements that could merge; gone says what isn't in the file being written
MergePlan plan_merge(const SpfFile& spf, const std::vector<uint32_t>& candidates, const std::function<bool(uint32_t)>& gone, const std::string& stem);

}  // namespace bimpass

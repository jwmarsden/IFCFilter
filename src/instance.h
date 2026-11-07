#pragma once

// Face sets that are the same shape, moved and turned, are written once and placed with mapped items.
// This rewrites the file in place, and goes before compact(): only the shape that's kept gets rounded
// and triangulated, so copies can't drift apart by being rounded differently.

#include <cstdint>
#include <string>

namespace bimpass {

struct InstanceStats {
    uint64_t sets = 0;       // face sets in the file
    uint64_t copies = 0;     // of those, the ones now placed from another
    uint64_t shapes = 0;     // shapes that now have copies
    uint64_t bytes_before = 0, bytes_after = 0;
    bool checked = false;    // false if the length unit isn't plain SI, as there's then no tolerance to go by
};

// A copy has the same faces and colours as what it's placed from, and all its points are within precision_mm
InstanceStats instance(const std::string& path, double precision_mm);

}  // namespace bimpass

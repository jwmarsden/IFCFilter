#pragma once

#include <cstdint>
#include <string>

namespace bimpass {

struct CompactStats {
    uint64_t bytes_before = 0, bytes_after = 0;
    uint64_t instances_before = 0, instances_after = 0;
    uint64_t sets_triangulated = 0, sets_left = 0;  // face sets; left: a texture map, or a face that couldn't be triangulated
    uint64_t faces_left = 0;
    uint64_t no_area_removed = 0, no_area_left = 0;  // triangles and faces with no area once rounded; left: all a set had
    bool rounded = false;                           // false if the model's length unit isn't known
};

// Rewrites an IFC file in place, keeping every element and property: coordinates rounded to
// precision_mm, polygon face sets as triangle lists, faces with no area left out, and identical property values, sets and their
// relationships written once.
CompactStats compact(const std::string& path, double precision_mm);

}  // namespace bimpass

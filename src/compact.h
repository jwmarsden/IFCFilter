#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ifcfilter {

struct CompactStats {
    uint64_t bytes_before = 0, bytes_after = 0;
    uint64_t instances_before = 0, instances_after = 0;
    uint64_t sets_triangulated = 0, sets_left = 0;  // face sets; left: a texture map, or a face that couldn't be triangulated
    uint64_t sets_cut = 0;                          // left exactly as they were, as openings cut them or are made of them
    uint64_t faces_left = 0;
    uint64_t no_area_removed = 0, no_area_left = 0;  // triangles and faces with no area once rounded; left: all a set had
    bool rounded = false;                           // false if the model's length unit isn't known
};

// Rewrites an IFC file in place, keeping every element and property: coordinates rounded to
// precision_mm, polygon face sets as triangle lists, faces with no area left out, apart from face sets that openings touch, and identical property values, sets and their
// relationships written once.
CompactStats compact(const std::string& path, double precision_mm);

// The numbers in a list, as written: a face's indices
std::vector<uint32_t> indices_in(std::string_view text);

// The points in a list of them, such as a point list's
std::vector<std::array<double, 3>> point_list(std::string_view list);

// Cuts a polygon, and any holes, into triangles of the same area. Indices count from 1, into points, and
// the triangles face the way the polygon does. False if it can't be done.
bool triangulate_polygon(std::vector<uint32_t> ring, const std::vector<std::vector<uint32_t>>& holes, const std::vector<std::array<double, 3>>& points,
                         std::vector<std::array<uint32_t, 3>>& out);

}  // namespace ifcfilter

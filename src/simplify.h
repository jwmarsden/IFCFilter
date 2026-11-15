#pragma once

// Triangle meshes with more points than their shape needs, such as pipes and fittings drawn with 24-sided circles,
// are decimated, so that what's left is within a tolerance of the planes of the faces it stands for. Points are only
// taken away, never moved, so what's left is rounded as compact() rounded it. This rewrites the file in place,
// and goes after compact(), which makes the triangles.

#include <cstdint>
#include <string>

namespace ifcfilter {

struct SimplifyStats {
    uint64_t sets = 0;                 // triangle sets in the file
    uint64_t simplified = 0;           // of those, the ones with fewer triangles now
    uint64_t triangles_before = 0, triangles_after = 0;  // of those
    uint64_t left_cut = 0;             // left as they are, as openings cut them
    uint64_t left_other = 0;           // left as they are: colours or texture by triangle, their own normals, or not a manifold
    uint64_t bytes_before = 0, bytes_after = 0;
    bool checked = false;              // false if the length unit isn't plain SI, as there's then no tolerance to go by
};

// What's kept is within tolerance_mm of the faces it replaces. Sets that openings touch are left exactly as they are.
SimplifyStats simplify(const std::string& path, double tolerance_mm);

}  // namespace ifcfilter

#pragma once

// Faceted breps, as Revit writes most families, are a shell of faces of loops of points, four
// instances to a face. Instancing and compact writing only work on face sets, so this writes each one
// as a polygonal face set of the same faces first.

#include <cstdint>
#include <string>

namespace bimpass {

struct BrepStats {
    uint64_t bytes_before = 0, bytes_after = 0;
    uint64_t converted = 0, left = 0;  // breps; left: one with voids, that an opening reaches, or not made the usual way
    uint64_t left_cut = 0;             // of those left, the ones an opening reaches
    uint64_t faces = 0;
    uint64_t faces_dropped = 0;        // an outer loop with fewer than three different points, so no area
    bool schema = true;                // false if the file isn't IFC4 or later, which has no face sets
};

// Rewrites an IFC file in place. Each face set takes its brep's id, so the styles and representations
// that point at the brep point at it, and shells, faces, loops and points nothing else uses are left out.
// Goes before instance() and compact().
BrepStats breps_to_face_sets(const std::string& path);

}  // namespace bimpass

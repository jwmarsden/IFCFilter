#pragma once

#include <cstddef>
#include <string>

namespace bimpass {

struct ReportOptions {
    std::string ifc_path;
    std::string csv_path;     // per-family table, skipped if empty
    std::string suspects_path;  // per-element list of suspiciously large geometry, to look at; skipped if empty
    size_t top_families = 15;
    size_t heavy_faces = 5000;    // faces on an element that count as a tessellation explosion
    double transparent = 0.7;     // as AMISChatter's ifc_geometry: at least this see-through is an envelope
};

// Read-only: where a model's bytes go, what looks like an explosion or an envelope, and how
// site IDs are used. Writes nothing but the optional CSV. Returns a process exit code.
int report(const ReportOptions& options);

}  // namespace bimpass

#pragma once

#include <string>

namespace bimpass {

struct SplitOptions {
    std::string ifc_path;
    std::string out_dir;
    std::string rules_path;      // see rules.txt; none if empty
    double transparent = 0.7;    // as AMISChatter's ifc_geometry: at least this see-through is an envelope
    bool compact = true;         // see compact.h
    double precision_mm = 0.1;
    bool zip = true;             // outputs as .ifczip, not .ifc
};

// Writes OUT/STEM.federate.ifc, STEM.hidden.ifc, STEM.envelopes.ifc and STEM.manifest.csv,
// and links STEM.original.ifc to the input.
// Geometry is copied as it is. Returns a process exit code.
int split(const SplitOptions& options);

}  // namespace bimpass

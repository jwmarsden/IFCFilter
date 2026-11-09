#pragma once

#include <string>
#include <vector>

namespace bimpass {

struct SplitOptions {
    std::vector<std::string> inputs;  // models, and folders of them
    std::string out_dir;
    std::string rules_path;      // see rules.txt; none if empty
    std::string mapping_path;    // see mapping.h: what to do with classes, attributes, sets and properties in the federation file; none if empty
    double transparent = 0.7;    // as AMISChatter's ifc_geometry: at least this see-through is an envelope
    bool merge = true;           // see merge.h
    bool instance = true;        // see instance.h
    bool compact = true;         // see compact.h
    double precision_mm = 0.1;
    double merge_gap_m = 5;      // parts of a site further apart than this aren't one thing; 0 doesn't check
    bool zip = true;             // the hidden and envelope files as .ifczip, not .ifc; the federation file is always .ifc
    bool all = false;            // do every model, even those that would come out the same
    int jobs = 0;                // models at once, as memory allows; 0 for as many as there are processors
    std::string worker_shared;   // for one model of a parallel run: the file of GUIDs in several models
};

// Writes federate/DISCIPLINE/STEM.federate.ifc, where DISCIPLINE is the name of the input's folder,
// non-federate/STEM.hidden.ifc and STEM.envelopes.ifc, and manifest/STEM.manifest.csv under OUT
// for each model, and links STEM.original.ifc to its input. register.csv says what they were built from. No GUID is written twice in a run, so models to be
// federated together should go through in one run.
// Geometry is copied as it is. Returns a process exit code.
int split(const SplitOptions& options);

// Compares models, and folders of them, with OUT/register.csv, without changing anything. Returns 0 if
// the outputs are up to date with them, and 1 if something is new, changed or missing.
int check(const std::vector<std::string>& inputs, const std::string& out_dir);

}  // namespace bimpass

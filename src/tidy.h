#pragma once

// What a model says about itself, made consistent once it's been split: property sets nobody needs once
// the split has used them are left out, the header and owner history say IFCFilter changed the file, and
// names and descriptions follow one rule across the model.

#include <cstdint>
#include <string>

namespace ifcfilter {

struct TidyStats {
    uint64_t sets_dropped = 0;    // property sets left out: CULLED_PSETS, and Data sets with nothing of their own
    uint64_t repeats_dropped = 0; // properties left out of Data, as another set on the same elements says the same
    uint64_t named = 0;           // elements renamed: after their site, or with the Revit element ID taken off
    uint64_t described = 0;       // elements given their asset component as their description
    uint64_t histories = 0;       // owner histories marked as modified by IFCFilter
    uint64_t types_corrected = 0; // elements whose asset-type property now says what their site ID starts with
    uint64_t types_mixed = 0;     // sets of asset types left as they were, as they're on elements of different types
};

// Rewrites an IFC file in place. source is the model it was split from, for the header.
TidyStats tidy(const std::string& path, const std::string& source);

}  // namespace ifcfilter

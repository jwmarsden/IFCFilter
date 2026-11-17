#pragma once

// An IFCFilter property set on every element of the federation file, saying where it came from: the model's
// file name, with their zone, site ID, asset type
// (how the site ID starts, as its level is named) and location.

#include "site_lookup.h"
#include "element_corrections.h"
#include "location_reference.h"
#include "property_schema.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ifcfilter {

class SpfFile;

struct Traced {
    std::string guid;                // in the federation file
    std::vector<uint32_t> sources;   // the input's elements it's made from, by id
};

struct TraceStats {
    uint64_t elements = 0;  // given a set
    uint64_t missing = 0;   // whose GUID wasn't in the file
    bool project_missing = false;  // no project set was available on contributing elements
};

inline const char* TRACE_SET = "IFCFilter";

// input is the model as it was read, for its elements' GUIDs and properties; file_name is the model's.
// The sets are added to the file at path.
TraceStats trace(const std::string& path, const SpfFile& input, const std::string& file_name, const std::vector<Traced>& elements, const SiteLookup& corrections, const PropertySchema& property_schema = {}, const LocationReference& locations = {}, const ElementCorrections& element_corrections = {});

}  // namespace ifcfilter


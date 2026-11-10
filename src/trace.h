#pragma once

// An IFCFilter property set on every element of the federation file, saying where it came from: the model's
// file name and the GUIDs it had there, of every element merged into it, with their zone, site ID, asset type
// (how the site ID starts, as its level is named) and location.

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
};

inline const char* TRACE_SET = "IFCFilter";

// input is the model as it was read, for its elements' GUIDs and properties; file_name is the model's.
// The sets are added to the file at path.
TraceStats trace(const std::string& path, const SpfFile& input, const std::string& file_name, const std::vector<Traced>& elements);

}  // namespace ifcfilter

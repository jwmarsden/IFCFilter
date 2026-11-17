#pragma once

// Every model in metres: a model whose length, area or volume unit is another SI unit, such as millimetres, is
// written again with its numbers scaled and its units made metres, square metres and cubic metres, so that
// models can be combined and compared as they are.

#include "spf.h"

#include <cstdint>
#include <string>

namespace ifcfilter {

struct Metres {
    double length = 1, area = 1, volume = 1;  // what each number of each was multiplied by
    uint64_t lines = 0;                       // lines changed
    std::string refused;                      // why it wasn't converted, such as a unit that isn't SI
    bool converted() const { return lines > 0; }
};

// Writes path, a copy of spf in metres, if spf isn't in metres already; converted() says whether it did.
// schema_name is the file's schema, as its header names it.
Metres to_metres(const SpfFile& spf, const std::string& schema_name, const std::string& path);

}  // namespace ifcfilter

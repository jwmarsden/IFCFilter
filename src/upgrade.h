#pragma once

#include "spf.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bimpass {

// The schema a model's header names, without reading the rest of it
std::string schema_in_header(const std::filesystem::path& path);

struct Upgrade {
    std::vector<std::string> refused;  // what doesn't fit IFC4; if there's any of this, nothing was written
    uint64_t lengthened = 0;           // instances given the attributes IFC4 added
    uint64_t ports = 0;                // port relationships now written as nesting
    uint64_t retyped = 0;              // predefined types IFC4 doesn't have, now user defined
};

// Writes an IFC2X3 model as IFC4, at path. Every instance is checked against the IFC4 schema first: its
// type, what its attributes are called, and that each value fits, down to the type of what it refers to.
Upgrade upgrade_to_ifc4(const SpfFile& spf, const std::filesystem::path& path);

}  // namespace bimpass

#pragma once

#include <string>

namespace bimpass {

// A zip holding one file, under the name entry. An ifczip is this, holding the IFC file.
void zip_file(const std::string& path, const std::string& zip_path, const std::string& entry);

}  // namespace bimpass

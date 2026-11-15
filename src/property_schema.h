#pragma once
#include <filesystem>
#include <map>
#include <set>
#include <string>
namespace ifcfilter {
using PropertySchema = std::map<std::string, std::set<std::string>>;
// Read property-set/property pairs; Element rows describe IFC attributes, not property sets.
PropertySchema load_property_schema(const std::string& path);
// Give every product the explicitly requested schema, or the federation union when none is supplied.
void normalize_properties(const std::filesystem::path& directory, const std::string& schema_path = {});
}

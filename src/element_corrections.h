#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace ifcfilter {
class SpfFile;
struct ElementCorrection { std::string site, type; double transparency = -1; };  // -1: drawn as it is
using ElementCorrections = std::unordered_map<uint32_t, ElementCorrection>;

// A per-model workbook in site-id-corrections/DISCIPLINE/MODEL.xlsx. An optional Transparency column, from 0
// to 1, draws the corrected site see-through in that model's federate file, as transparent-site does.
std::filesystem::path element_correction_path(const std::filesystem::path& input, const std::string& schema_path);
ElementCorrections load_element_corrections(const std::filesystem::path& path, const std::filesystem::path& input, const SpfFile& spf);
}

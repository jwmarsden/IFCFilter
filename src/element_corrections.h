#pragma once
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>

namespace ifcfilter {
class SpfFile;
// site and type from the site ID corrections; location from the location corrections
struct ElementCorrection { std::string site, type; double transparency = -1; std::string location; };  // -1: drawn as it is
using ElementCorrections = std::unordered_map<uint32_t, ElementCorrection>;

// A per-model workbook in site-id-corrections/DISCIPLINE/MODEL.xlsx. An optional Transparency column, from 0
// to 1, draws the corrected site see-through in that model's federate file, as transparent-site does.
std::filesystem::path element_correction_path(const std::filesystem::path& input, const std::string& schema_path);
ElementCorrections load_element_corrections(const std::filesystem::path& path, const std::filesystem::path& input, const SpfFile& spf);

// A per-model file in location-corrections/DISCIPLINE/MODEL.csv, which IFCViewer's Edit dialog adds rows to:
// GlobalId, Location, Description, Model, Set by, Set at, Status. The last Approved row for an element wins, and
// one with no Location takes its correction away. The location goes in the IFCFilter set's Location, ahead of
// the site lookup's and the model's own.
std::filesystem::path location_correction_path(const std::filesystem::path& input, const std::string& schema_path);
void load_location_corrections(const std::filesystem::path& path, const std::filesystem::path& input, const SpfFile& spf, ElementCorrections& corrections);

// A per-model file in site-id-corrections/DISCIPLINE/MODEL.csv, beside the workbook, which IFCViewer's Edit
// dialog adds rows to: GlobalId, Site ID, Model, Set by, Set at, Status. Read after the workbook, so the last
// Approved row for an element wins over it; the asset type is the site's. One with no Site ID takes the
// element's site correction away.
std::filesystem::path site_correction_csv_path(const std::filesystem::path& input, const std::string& schema_path);
void load_site_corrections_csv(const std::filesystem::path& path, const std::filesystem::path& input, const SpfFile& spf, ElementCorrections& corrections);

// The site IDs a model's corrections give its elements, from the workbook's approved rows and the CSV's: putting
// an element in a site another model of the same combined file has joins it to that model's element
std::set<std::string> corrected_sites(const std::filesystem::path& input, const std::string& schema_path);
}

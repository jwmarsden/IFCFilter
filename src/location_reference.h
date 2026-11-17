#pragma once
// location-referencing.xlsx (or legacy CSV), by location hierarchy code: the description and Uniclass Co, En and SL
// codes and titles for each location, used to fill in what a matching IFC_Filter.Location leaves out.

#include <array>
#include <map>
#include <string>
namespace ifcfilter {
// classification holds Co code, Co title, En code, En title, SL code and SL title. A field the CSV gives
// more than one value for, across rows of the same location, is left empty rather than guessed.
struct LocationEntry { std::string description; std::array<std::string,6> classification; };
using LocationReference = std::map<std::string,LocationEntry>;
LocationReference load_location_reference(const std::string& path);
}

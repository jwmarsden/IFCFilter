#pragma once
#include <string>
namespace ifcfilter {
// Empty for unknown or inactive codes; otherwise the authoritative title.
std::string uniclass_title(const std::string& code);
}

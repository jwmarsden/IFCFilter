#pragma once
#include <string>
#include <vector>
namespace ifcfilter {
// Read the first worksheet as text cells from a standard, unencrypted XLSX.
std::vector<std::vector<std::string>> read_xlsx(const std::string& path);
}

#pragma once

// What a run needs to know about the machine it's on.

#include <cstdint>
#include <string>

namespace ifcfilter {

uint64_t available_memory();  // in bytes; 0 if it can't be told
std::string program_path();   // this program, for running more of it

// Moves a finished file over the one it replaces. Windows refuses for a moment while something such as
// a virus scanner or a sync tool has either open, so this tries for some seconds before it gives up.
void replace_file(const std::string& from, const std::string& to);

}  // namespace ifcfilter

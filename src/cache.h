#pragma once

// A viewing cache of an IFC model, laid out for a viewer to load in pieces: see docs/cache-format.md

#include <string>
#include <vector>

namespace ifcfilter {

// Writes output from the inputs: one model, or several into one cache, such as a discipline's. Returns a process exit code.
int cache(const std::vector<std::string>& inputs, const std::string& output);

}  // namespace ifcfilter

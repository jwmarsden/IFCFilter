#pragma once

// A viewing cache of an IFC model, laid out for a viewer to load in pieces: see docs/cache-format.md

#include <string>

namespace ifcfilter {

// Writes output from input. Returns a process exit code.
int cache(const std::string& input, const std::string& output);

}  // namespace ifcfilter

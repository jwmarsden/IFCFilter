#pragma once
// ifcfilter cache: the .ifcc viewing cache, a SQLite file of a model's meshes, tiles, tree and properties
// that a viewer such as IFCViewer loads in pieces. docs/cache-format.md is the format.

#include <string>
#include <vector>
namespace ifcfilter {
struct CacheOptions {
    bool detail = false;         // retry approximate geometry and process complex openings
    bool if_changed = false;     // reuse matching caches and preserve their detailed profile
    std::vector<std::string> detail_elements; // retry only these GUIDs with IfcOpenShell
};
// Writes output from the inputs: one model, or several into one cache, each under an element named after
// its file. A model with nothing to draw gets no cache. Returns a process exit code.
int cache(const std::vector<std::string>& inputs, const std::string& output, const CacheOptions& options = {});
}

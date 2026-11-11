#pragma once

// Sign faces moved forward off their posts. A face is often modelled with its back exactly on its post's front,
// so the two flicker in a viewer, and some posts come through the plate. Forward is the side of the plate its
// legend is on: its border, symbols and text.

#include <cstdint>
#include <string>
#include <vector>

namespace ifcfilter {

class SpfFile;

// forward-if SET.PROPERTY=VALUE MM: assemblies with this property move forward by this much
struct ForwardRule {
    std::string set, property, value;
    double millimetres = 0;
};

struct Forward {
    uint64_t faces = 0;    // moved
    uint64_t unsure = 0;   // picked, but which way is forward couldn't be told, so left
};

// Writes the model to path with the picked assemblies' parts placed forward, if any are; faces is 0 if not,
// and nothing is written
Forward move_forward(const SpfFile& spf, const std::vector<ForwardRule>& rules, const std::string& path);

}  // namespace ifcfilter

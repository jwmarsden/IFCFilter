#pragma once

// A mesh's edges, less the ones on a flat surface. The same rule as IFCViewer's src-tauri/src/edges.rs, which
// applies it to caches written before this.

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace ifcfilter {

using EdgePoint = std::array<double, 3>;

// The edges of those given not on a flat surface: where every face of the mesh along it is in one plane,
// within 30 degrees, or a millimetre for a sliver, which way each faces aside, and there's surface on both
// sides of it, or on one side and in that plane just past it all along it (a T-junction, a seam, a piece lying
// on another). Surface on one side only is an outline, and stays. Points in the same place are one.
std::vector<std::pair<uint32_t, uint32_t>> flat_edges_out(const std::vector<EdgePoint>& points, const std::vector<uint32_t>& indices,
                                                           const std::vector<std::pair<uint32_t, uint32_t>>& edges);

}  // namespace ifcfilter

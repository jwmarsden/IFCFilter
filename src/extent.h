#pragma once

// Where things reach in the world, worked out from their text without a geometry engine: exactly for
// shapes made of points, a little over for profiles given by sizes, and not at all for what isn't known.

#include "rigid.h"
#include "spf.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace bimpass {

// An axis-aligned box in the world, in the model's length unit
struct Box {
    Vector low{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vector high{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    bool empty() const { return low[0] > high[0]; }
    void add(const Vector& p);
    void add(const Box& b);
};

// The air between two boxes; 0 if they touch or overlap
double gap_between(const Box& a, const Box& b);

class Extents {
  public:
    explicit Extents(const SpfFile& spf) : spf_(spf) {}

    // Where an object placement puts things in the world; none for a grid placement
    std::optional<Rigid> placement(std::string_view reference_text);

    // Grows box by where a representation item reaches, placed by to_world. False if that can't be worked out.
    bool item(uint32_t item, const Rigid& to_world, Box& box) { return reach(item, to_world, box, 0); }

    // Where a product's body reaches; none if any of it can't be worked out
    std::optional<Box> product(uint32_t product);

  private:
    bool reach(uint32_t item, const Rigid& to_world, Box& box, int depth);
    bool points_under(uint32_t item, const Rigid& to_world, Box& box);
    bool profile_box(uint32_t profile, double& low_x, double& high_x, double& low_y, double& high_y);

    const SpfFile& spf_;
    std::unordered_map<uint32_t, std::optional<Rigid>> placements_;
};

// The face sets and faceted breps an opening reaches: those of the openings themselves, and those of what they
// cut whose box meets the opening's box. Taking a solid from a mesh is fragile, so these are left exactly as they are. An
// item whose reach, or whose opening's, can't be worked out counts as reached.
std::unordered_set<uint32_t> sets_openings_touch(const SpfFile& spf);

}  // namespace bimpass

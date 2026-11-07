#pragma once

// Rotations and moves, read from and written to IFC's text. Shared by what makes new mapped items.

#include "spf.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bimpass {

using Vector = std::array<double, 3>;

// A rotation and a move, with no scale or mirror
struct Rigid {
    std::array<Vector, 3> axes{Vector{1, 0, 0}, Vector{0, 1, 0}, Vector{0, 0, 1}};  // where x, y and z point
    Vector origin{0, 0, 0};
};

double dot(const Vector& a, const Vector& b);
Vector cross(const Vector& a, const Vector& b);
Vector minus(const Vector& a, const Vector& b);
std::optional<Vector> normalised(const Vector& v);
Vector applied(const Rigid& a, const Vector& v);
Rigid compose(const Rigid& a, const Rigid& b);
Rigid inverse(const Rigid& a);

// The numbers in a point or direction's list; a 2D one has no z
Vector numbers_in(std::string_view list);

std::optional<Rigid> axis_placement(const SpfFile& spf, uint32_t id);

// A mapped item's target. None if it scales or mirrors, as what's made from it couldn't do the same.
std::optional<Rigid> operator_of(const SpfFile& spf, uint32_t id);

std::string reference(uint32_t id);
std::string list_of(const std::vector<uint32_t>& ids);

// New instances, numbered up from the input's last
class Emitter {
  public:
    explicit Emitter(uint32_t next_id) : next_id_(next_id) {}

    uint32_t emit(const std::string& text);
    uint32_t emit_once(const std::string& text);  // for what repeats a lot, such as directions
    uint32_t emit_operator(const Rigid& target);
    uint32_t emit_identity();                     // an axis placement that changes nothing

    std::vector<std::string>& lines() { return lines_; }

  private:
    uint32_t emit_vector(const char* type, const Vector& v, int places);

    uint32_t next_id_;
    uint32_t identity_ = 0;
    std::vector<std::string> lines_;
    std::unordered_map<std::string, uint32_t> written_;
};

}  // namespace bimpass

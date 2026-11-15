#include "rigid.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ifcfilter {
namespace {

std::optional<Vector> direction(const SpfFile& spf, std::string_view reference_text, const Vector& otherwise) {
    const auto ids = refs_in(reference_text);
    if (ids.empty()) return otherwise;
    const auto attributes = attributes_of(spf.line(ids[0]));
    if (attributes.empty()) return std::nullopt;
    return normalised(numbers_in(attributes[0]));
}

std::optional<Vector> point(const SpfFile& spf, std::string_view reference_text) {
    const auto ids = refs_in(reference_text);
    if (ids.size() != 1) return std::nullopt;
    const auto attributes = attributes_of(spf.line(ids[0]));
    if (attributes.empty()) return std::nullopt;
    return numbers_in(attributes[0]);
}

std::string real(double value, int places) {
    char buffer[64];
    int length = std::snprintf(buffer, sizeof buffer, "%.*f", places, value);
    while (buffer[length - 1] == '0') --length;
    if (length == 3 && buffer[0] == '-' && buffer[1] == '0' && buffer[2] == '.') return "0.";
    return std::string(buffer, static_cast<size_t>(length));
}

}  // namespace

double dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vector cross(const Vector& a, const Vector& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vector minus(const Vector& a, const Vector& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

std::optional<Vector> normalised(const Vector& v) {
    const double length = std::sqrt(dot(v, v));
    if (length < 1e-12) return std::nullopt;
    return Vector{v[0] / length, v[1] / length, v[2] / length};
}

Vector applied(const Rigid& a, const Vector& v) {
    Vector out = a.origin;
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < 3; ++i) out[i] += a.axes[axis][i] * v[axis];
    }
    return out;
}

Rigid compose(const Rigid& a, const Rigid& b) {
    Rigid rotation;
    rotation.axes = a.axes;
    Rigid out;
    for (int axis = 0; axis < 3; ++axis) out.axes[axis] = applied(rotation, b.axes[axis]);
    out.origin = applied(a, b.origin);
    return out;
}

Rigid inverse(const Rigid& a) {
    Rigid out;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out.axes[i][j] = a.axes[j][i];
        out.origin[i] = -dot(a.axes[i], a.origin);
    }
    return out;
}

Vector numbers_in(std::string_view list) {
    Vector out{0, 0, 0};
    int axis = 0;
    for (const char* p = list.data(), *end = p + list.size(); p < end && axis < 3;) {
        if (*p == '-' || *p == '+' || *p == '.' || (*p >= '0' && *p <= '9')) {
            char* after = nullptr;
            const double value = std::strtod(p, &after);
            if (after == p) {
                ++p;
                continue;
            }
            out[axis++] = value;
            p = after;
        } else {
            ++p;
        }
    }
    return out;
}

std::optional<Rigid> axis_placement(const SpfFile& spf, uint32_t id) {
    if (spf.type(id) != "IFCAXIS2PLACEMENT3D") return std::nullopt;
    const auto attributes = attributes_of(spf.line(id));
    if (attributes.size() != 3) return std::nullopt;
    const auto location = point(spf, attributes[0]);
    const auto z = direction(spf, attributes[1], {0, 0, 1});
    const auto x = direction(spf, attributes[2], {1, 0, 0});
    if (!location || !z || !x) return std::nullopt;
    const double along = dot(*x, *z);
    const auto square = normalised({(*x)[0] - along * (*z)[0], (*x)[1] - along * (*z)[1], (*x)[2] - along * (*z)[2]});
    if (!square) return std::nullopt;
    Rigid out;
    out.axes = {*square, cross(*z, *square), *z};
    out.origin = *location;
    return out;
}

std::optional<Rigid> operator_of(const SpfFile& spf, uint32_t id) {
    // The non-uniform kind adds a scale for y and z; Bentley writes it for most of LA's planting, all scales 1
    const auto type = spf.type(id);
    const bool nonuniform = type == "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM";
    if (type != "IFCCARTESIANTRANSFORMATIONOPERATOR3D" && !nonuniform) return std::nullopt;
    const auto attributes = attributes_of(spf.line(id));
    if (attributes.size() != (nonuniform ? 7u : 5u)) return std::nullopt;
    // Within a part in a million of 1, as 0.99999999999999989 is common
    const auto one = [](std::string_view scale) {
        if (scale == "$") return true;
        return std::abs(numbers_in(scale)[0] - 1) <= 1e-6;
    };
    if (!one(attributes[3]) || (nonuniform && (!one(attributes[5]) || !one(attributes[6])))) return std::nullopt;
    const auto x = direction(spf, attributes[0], {1, 0, 0});
    const auto y = direction(spf, attributes[1], {0, 1, 0});
    const auto location = point(spf, attributes[2]);
    if (!x || !y || !location) return std::nullopt;
    const auto z = direction(spf, attributes[4], cross(*x, *y));
    if (!z) return std::nullopt;
    const Vector square_y = cross(*z, *x);
    if (dot(square_y, *y) < 0) return std::nullopt;
    Rigid out;
    out.axes = {*x, square_y, *z};
    out.origin = *location;
    return out;
}

std::string reference(uint32_t id) { return "#" + std::to_string(id); }

std::string list_of(const std::vector<uint32_t>& ids) {
    std::string out = "(";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ',';
        out += reference(ids[i]);
    }
    return out + ")";
}

uint32_t Emitter::emit(const std::string& text) {
    const uint32_t id = next_id_++;
    lines_.push_back(reference(id) + "=" + text);
    return id;
}

uint32_t Emitter::emit_once(const std::string& text) {
    const auto seen = written_.find(text);
    if (seen != written_.end()) return seen->second;
    const uint32_t id = emit(text);
    written_[text] = id;
    return id;
}

uint32_t Emitter::emit_vector(const char* type, const Vector& v, int places) {
    return emit_once(std::string(type) + "((" + real(v[0], places) + "," + real(v[1], places) + "," + real(v[2], places) + "));");
}

uint32_t Emitter::emit_operator(const Rigid& target) {
    const uint32_t x = emit_vector("IFCDIRECTION", target.axes[0], 12);
    const uint32_t y = emit_vector("IFCDIRECTION", target.axes[1], 12);
    const uint32_t z = emit_vector("IFCDIRECTION", target.axes[2], 12);
    const uint32_t origin = emit_vector("IFCCARTESIANPOINT", target.origin, 6);
    return emit_once("IFCCARTESIANTRANSFORMATIONOPERATOR3D(" + reference(x) + "," + reference(y) + "," + reference(origin) + ",1.," + reference(z) + ");");
}

uint32_t Emitter::emit_placement(const Rigid& placement) {
    const uint32_t origin = emit_vector("IFCCARTESIANPOINT", placement.origin, 6);
    const uint32_t z = emit_vector("IFCDIRECTION", placement.axes[2], 12);
    const uint32_t x = emit_vector("IFCDIRECTION", placement.axes[0], 12);
    return emit_once("IFCAXIS2PLACEMENT3D(" + reference(origin) + "," + reference(z) + "," + reference(x) + ");");
}

uint32_t Emitter::emit_identity() {
    if (!identity_) identity_ = emit("IFCAXIS2PLACEMENT3D(" + reference(emit_vector("IFCCARTESIANPOINT", {0, 0, 0}, 6)) + ",$,$);");
    return identity_;
}

}  // namespace ifcfilter

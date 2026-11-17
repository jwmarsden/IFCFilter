#include "metres.h"

#include <ifcparse/schema.h>

#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace ifcfilter {
namespace {

enum class Kind { None, Length, Area, Volume };

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    return text;
}

// An SI prefix's power of ten: .MILLI. is -3, none is 0
std::optional<int> prefix_power(std::string_view prefix) {
    static const std::map<std::string_view, int> powers = {
        {"$", 0}, {".DECI.", -1}, {".CENTI.", -2}, {".MILLI.", -3}, {".MICRO.", -6}, {".DECA.", 1}, {".HECTO.", 2}, {".KILO.", 3}};
    const auto at = powers.find(trimmed(prefix));
    if (at == powers.end()) return std::nullopt;
    return at->second;
}

// What a declared type measures, following defined types down: IfcPositiveLengthMeasure is an IfcLengthMeasure.
// Aggregates of measures, such as a point's coordinates, measure the same.
Kind kind_of(const ifcopenshell::parameter_type* type) {
    for (int depth = 0; type && depth < 16; ++depth) {
        if (const auto* aggregate = type->as_aggregation_type()) {
            type = aggregate->type_of_element();
            continue;
        }
        const auto* named = type->as_named_type();
        if (!named) return Kind::None;
        const auto* declared = named->declared_type();
        const std::string& name = declared->name();
        if (name == "IfcLengthMeasure") return Kind::Length;
        if (name == "IfcAreaMeasure") return Kind::Area;
        if (name == "IfcVolumeMeasure") return Kind::Volume;
        const auto* defined = declared->as_type_declaration();
        if (!defined) return Kind::None;
        type = defined->declared_type();
    }
    return Kind::None;
}

// A number as STEP writes a real: always with a point, as 1. or 1.5E-05
std::string real(double value) {
    char text[64];
    const auto result = std::to_chars(text, text + sizeof text, value);
    std::string out(text, result.ptr);
    const auto e = out.find_first_of("eE");
    const std::string mantissa = out.substr(0, e);
    std::string exponent = e == std::string::npos ? "" : out.substr(e + 1);
    std::string written = mantissa.find('.') == std::string::npos ? mantissa + "." : mantissa;
    if (!exponent.empty()) written += "E" + exponent;
    return written;
}

// Each number in text divided by divisor: a power of ten such as 1000, as dividing by it rounds better than
// multiplying by 0.001. References, enumerations, strings and what's unset are left as they are.
std::string scaled(std::string_view text, double divisor, bool& changed) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\'') {  // a string, to its closing quote; '' is a quote inside it
            size_t end = i + 1;
            while (end < text.size()) {
                if (text[end] == '\'' && (end + 1 >= text.size() || text[end + 1] != '\'')) break;
                end += text[end] == '\'' ? 2 : 1;
            }
            out.append(text.substr(i, end + 1 - i));
            i = end + 1;
            continue;
        }
        if (c == '#' || (c == '.' && i + 1 < text.size() && std::isalpha(static_cast<unsigned char>(text[i + 1])))) {
            // a reference, or an enumeration such as .T.
            size_t end = i + 1;
            while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
            if (c == '.' && end < text.size() && text[end] == '.') ++end;
            out.append(text.substr(i, end - i));
            i = end;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || ((c == '-' || c == '+' || c == '.') && i + 1 < text.size() && (std::isdigit(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '.'))) {
            size_t end = i;
            if (text[end] == '-' || text[end] == '+') ++end;
            while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.')) ++end;
            if (end < text.size() && (text[end] == 'E' || text[end] == 'e')) {
                ++end;
                if (end < text.size() && (text[end] == '-' || text[end] == '+')) ++end;
                while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            }
            const std::string number(text.substr(i, end - i));
            char* stop = nullptr;
            const double value = std::strtod(number.c_str(), &stop);
            if (stop && *stop == '\0' && std::isfinite(value)) {
                out += real(value / divisor);
                changed = true;
            } else {
                out += number;
            }
            i = end;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// A measure given its own type, as a property's or a select's value is: IFCLENGTHMEASURE(1500.)
std::string typed_scaled(std::string_view text, const std::array<double, 4>& divisors, bool& changed) {
    static const std::pair<std::string_view, Kind> measures[] = {
        {"IFCLENGTHMEASURE(", Kind::Length}, {"IFCPOSITIVELENGTHMEASURE(", Kind::Length}, {"IFCNONNEGATIVELENGTHMEASURE(", Kind::Length},
        {"IFCAREAMEASURE(", Kind::Area}, {"IFCVOLUMEMEASURE(", Kind::Volume}};
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t best = std::string_view::npos;
        Kind kind = Kind::None;
        size_t length = 0;
        for (const auto& [name, k] : measures) {
            const auto at = text.find(name, i);
            // POSITIVELENGTHMEASURE holds LENGTHMEASURE: only a name that starts where nothing of a name is before it
            if (at == std::string_view::npos || (at > 0 && std::isalpha(static_cast<unsigned char>(text[at - 1])))) continue;
            if (at < best) best = at, kind = k, length = name.size();
        }
        if (best == std::string_view::npos) break;
        const size_t open = best + length;
        const size_t close = text.find(')', open);
        if (close == std::string_view::npos) break;
        out.append(text.substr(i, open - i));
        out += scaled(text.substr(open, close - open), divisors[static_cast<int>(kind)], changed);
        i = close;
    }
    out.append(text.substr(i));
    return out;
}

// A trim's parameter, IFCPARAMETERVALUE(1500.), along a line, where it's a length
std::string typed_scaled_parameter(std::string_view text, double divisor, bool& changed) {
    static const std::string_view name = "IFCPARAMETERVALUE(";
    std::string out;
    size_t i = 0;
    for (size_t at; (at = text.find(name, i)) != std::string_view::npos;) {
        const size_t open = at + name.size();
        const size_t close = text.find(')', open);
        if (close == std::string_view::npos) break;
        out.append(text.substr(i, open - i));
        out += scaled(text.substr(open, close - open), divisor, changed);
        i = close;
    }
    out.append(text.substr(i));
    return out;
}

bool is_unit(std::string_view type) {
    return type == "IFCSIUNIT" || type == "IFCCONVERSIONBASEDUNIT" || type == "IFCCONVERSIONBASEDUNITWITHOFFSET" || type == "IFCDERIVEDUNIT" ||
           type == "IFCCONTEXTDEPENDENTUNIT" || type == "IFCMONETARYUNIT";
}

}  // namespace

Metres to_metres(const SpfFile& spf, const std::string& schema_name, const std::string& path) {
    Metres metres;
    // The project's units
    std::map<std::string, uint32_t> unit_of;  // .LENGTHUNIT. and the like, and its unit
    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        if (spf.type(id) != "IFCPROJECT") continue;
        const auto attributes = attributes_of(spf.line(id));
        if (attributes.size() < 9) break;
        const auto assignment = refs_in(attributes[8]);
        if (assignment.size() != 1) break;
        const auto listed = attributes_of(spf.line(assignment[0]));
        for (const uint32_t unit : listed.empty() ? std::vector<uint32_t>{} : refs_in(listed[0])) {
            const auto a = attributes_of(spf.line(unit));
            for (const char* type : {".LENGTHUNIT.", ".AREAUNIT.", ".VOLUMEUNIT."}) {
                if (a.size() >= 2 && trimmed(a[1]) == type) unit_of[type] = unit;
            }
        }
        break;
    }
    // Each number is divided by this, by what it measures: 1000 for millimetres
    std::array<double, 4> divisors{1, 1, 1, 1};
    const std::pair<const char*, Kind> kinds[] = {{".LENGTHUNIT.", Kind::Length}, {".AREAUNIT.", Kind::Area}, {".VOLUMEUNIT.", Kind::Volume}};
    std::unordered_map<uint32_t, std::string> replaced;
    for (const auto& [type, kind] : kinds) {
        const auto found = unit_of.find(type);
        if (found == unit_of.end()) continue;
        const uint32_t unit = found->second;
        const auto line = spf.line(unit);
        const auto a = attributes_of(line);
        if (spf.type(unit) != "IFCSIUNIT" || a.size() != 4) {
            metres.refused = std::string(type) + " isn't an SI unit";
            return metres;
        }
        const auto power = prefix_power(a[2]);
        if (!power) {
            metres.refused = std::string(type) + " has the prefix " + std::string(trimmed(a[2]));
            return metres;
        }
        if (*power == 0) continue;
        const int dimensions = kind == Kind::Length ? 1 : kind == Kind::Area ? 2 : 3;
        divisors[static_cast<int>(kind)] = std::pow(10.0, -*power * dimensions);
        replaced[unit] = std::string(line.substr(0, line.find('(') + 1)) + std::string(a[0]) + "," + std::string(a[1]) + ",$," + std::string(a[3]) + ");";
    }
    metres.length = 1 / divisors[1], metres.area = 1 / divisors[2], metres.volume = 1 / divisors[3];
    if (replaced.empty()) return metres;

    const auto* schema = ifcopenshell::schema_by_name(
        schema_name.rfind("IFC2X3", 0) == 0 ? "IFC2X3" : schema_name.rfind("IFC4X3", 0) == 0 ? "IFC4X3_ADD2" : "IFC4");
    // Which attributes of each type are measures, found once a type
    std::unordered_map<std::string, std::vector<Kind>> kinds_of;
    const auto attribute_kinds = [&](std::string_view type) -> const std::vector<Kind>& {
        const std::string name(type);
        const auto known = kinds_of.find(name);
        if (known != kinds_of.end()) return known->second;
        std::vector<Kind> found;
        try {
            if (const auto* entity = schema->declaration_by_name(name)->as_entity()) {
                for (const auto* attribute : entity->all_attributes()) found.push_back(kind_of(attribute->type_of_attribute()));
            }
        } catch (const std::exception&) {
        }
        return kinds_of.emplace(name, std::move(found)).first->second;
    };

    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        const auto line = spf.line(id);
        if (line.empty() || replaced.count(id)) continue;
        const auto type = spf.type(id);
        if (is_unit(type) || type == "IFCUNITASSIGNMENT") continue;
        auto attributes = attributes_of(line);
        // What gives its own unit, such as a property or quantity that names one, is in that unit, not the project's
        bool own_unit = false;
        for (const auto& attribute : attributes) {
            for (const uint32_t ref : refs_in(attribute)) own_unit = own_unit || is_unit(spf.type(ref));
        }
        if (own_unit) continue;
        const auto& kinds_here = attribute_kinds(type);
        std::vector<std::string> written(attributes.begin(), attributes.end());
        bool changed = false;
        for (size_t i = 0; i < written.size(); ++i) {
            const Kind kind = i < kinds_here.size() ? kinds_here[i] : Kind::None;
            if (kind != Kind::None) written[i] = scaled(written[i], divisors[static_cast<int>(kind)], changed);
            else written[i] = typed_scaled(written[i], divisors, changed);
        }
        // Lengths the schema doesn't type as lengths: a context's precision, and where along a line it's trimmed
        if (type == "IFCGEOMETRICREPRESENTATIONCONTEXT" && written.size() > 3) written[3] = scaled(written[3], divisors[1], changed);
        if (type == "IFCTRIMMEDCURVE" && written.size() > 2) {
            const auto basis = refs_in(attributes[0]);
            if (basis.size() == 1 && spf.type(basis[0]) == "IFCLINE") {
                for (int t = 1; t <= 2; ++t) written[t] = typed_scaled_parameter(written[t], divisors[1], changed);
            }
        }
        // A map conversion's offsets are in the map's unit if it gives one, and its scale takes project units there
        if (type == "IFCMAPCONVERSION" && written.size() >= 8) {
            const auto target = refs_in(attributes[1]);
            const auto crs = target.size() == 1 ? attributes_of(spf.line(target[0])) : std::vector<std::string_view>{};
            const bool map_unit = crs.size() > 6 && trimmed(crs[6]) != "$";
            if (map_unit) {
                written[2] = std::string(attributes[2]), written[3] = std::string(attributes[3]), written[4] = std::string(attributes[4]);
                const auto scale = trimmed(attributes[7]);
                const double was = scale == "$" ? 1 : std::strtod(std::string(scale).c_str(), nullptr);
                written[7] = real(was * divisors[1]);
                changed = true;
            }
        }
        if (!changed) continue;
        std::string text(line.substr(0, line.find('(') + 1));
        for (size_t i = 0; i < written.size(); ++i) text.append(i ? "," : "").append(written[i]);
        text += ");";
        replaced[id] = std::move(text);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error(path + ": can't be written");
    out << spf.header();
    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        const auto line = spf.line(id);
        if (line.empty()) continue;
        const auto at = replaced.find(id);
        out << (at != replaced.end() ? std::string_view(at->second) : line) << '\n';
    }
    out << "ENDSEC;\nEND-ISO-10303-21;\n";
    if (!out) throw std::runtime_error(path + ": write failed");
    metres.lines = replaced.size();
    return metres;
}

}  // namespace ifcfilter

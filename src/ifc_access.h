#pragma once

// Schema-independent reading of IFC instances by attribute name, so one build handles IFC2X3 and IFC4.

#include <ifcparse/file.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bimpass {

using Instances = std::vector<express::base>;

inline Instances all_of(ifcopenshell::file& file, const std::string& type) {
    try {
        return file.instances_by_type(type);
    } catch (const ifcopenshell::exception&) {
        return {};  // not in this file's schema
    }
}

inline std::optional<ifcopenshell::attribute_value> attribute(const express::entity& e, const std::string& name) {
    if (!e) return std::nullopt;  // an optional link that isn't set, such as a storey's Representation
    try {
        auto value = e.get(name);
        if (value.isNull()) return std::nullopt;
        return value;
    } catch (const ifcopenshell::exception&) {
        return std::nullopt;  // not an attribute of this type, in this schema
    }
}

inline std::string text(const express::entity& e, const std::string& name) {
    const auto value = attribute(e, name);
    if (!value) return "";
    try {
        return static_cast<std::string>(*value);
    } catch (const ifcopenshell::exception&) {
        return "";
    }
}

inline express::entity link(const express::entity& e, const std::string& name) {
    const auto value = attribute(e, name);
    if (!value) return {};
    return static_cast<express::base>(*value).as<express::entity>();
}

inline Instances links(const express::entity& e, const std::string& name) {
    const auto value = attribute(e, name);
    if (!value || value->size() == 0) return {};
    return static_cast<Instances>(*value);
}

inline std::optional<double> number(const express::entity& e, const std::string& name) {
    const auto value = attribute(e, name);
    if (!value) return std::nullopt;
    if (value->type() == ifcopenshell::Argument_DOUBLE) return static_cast<double>(*value);
    if (value->type() == ifcopenshell::Argument_ENTITY_INSTANCE) {  // IfcNormalisedRatioMeasure and the like
        const auto wrapped = static_cast<express::base>(*value);
        if (wrapped.declaration().as_entity() == nullptr) {
            const auto inner = wrapped.get_attribute_value(0);
            if (inner.type() == ifcopenshell::Argument_DOUBLE) return static_cast<double>(inner);
        }
    }
    return std::nullopt;
}

// A site ID's value is an IfcText or IfcLabel wrapping the string
inline std::string nominal_text(const express::entity& property) {
    const auto value = attribute(property, "NominalValue");
    if (!value) return "";
    if (value->type() == ifcopenshell::Argument_STRING) return static_cast<std::string>(*value);
    if (value->type() != ifcopenshell::Argument_ENTITY_INSTANCE) return "";
    const auto inner = static_cast<express::base>(*value).get_attribute_value(0);
    return inner.type() == ifcopenshell::Argument_STRING ? static_cast<std::string>(inner) : "";
}

// How see-through each styled item is. Items take it from an IfcStyledItem, and tessellated
// ones can also take it from an IfcIndexedColourMap.
inline std::unordered_map<uint32_t, double> item_transparency(ifcopenshell::file& file) {
    std::unordered_map<uint32_t, double> out;
    const auto note = [&](uint32_t id, double t) {
        auto [it, added] = out.emplace(id, t);
        if (!added) it->second = std::max(it->second, t);
    };
    const std::function<std::optional<double>(const express::entity&)> style_of =
        [&](const express::entity& style) -> std::optional<double> {
            if (!style) return std::nullopt;
            if (style.declaration().is("IfcPresentationStyleAssignment") || style.declaration().is("IfcSurfaceStyle")) {
                std::optional<double> best;
                for (const auto& s : links(style, "Styles")) {
                    if (const auto t = style_of(s.as<express::entity>())) best = std::max(best.value_or(0.0), *t);
                }
                return best;
            }
            if (style.declaration().is("IfcSurfaceStyleShading") || style.declaration().is("IfcSurfaceStyleRendering")) {
                return number(style, "Transparency");
            }
            return std::nullopt;
        };
    for (const auto& styled : all_of(file, "IfcStyledItem")) {
        const auto e = styled.as<express::entity>();
        const auto item = link(e, "Item");
        if (!item) continue;
        for (const auto& s : links(e, "Styles")) {
            if (const auto t = style_of(s.as<express::entity>())) note(item.id(), *t);
        }
    }
    for (const auto& map : all_of(file, "IfcIndexedColourMap")) {
        const auto e = map.as<express::entity>();
        const auto target = link(e, "MappedTo");
        const auto opacity = number(e, "Opacity");
        if (target && opacity) note(target.id(), 1.0 - *opacity);
    }
    return out;
}

// One element's family: Revit writes "Family:Type:12345" in Name, and the type in ObjectType
inline std::string family_of(const express::entity& product) {
    static const std::regex trailing_id(":[0-9]+$");
    std::string name = text(product, "ObjectType");
    if (name.empty()) name = text(product, "Name");
    return std::regex_replace(name, trailing_id, "");
}

}  // namespace bimpass

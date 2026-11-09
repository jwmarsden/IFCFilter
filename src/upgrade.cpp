#include "upgrade.h"

#include <ifcparse/schema.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <fstream>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ifcfilter {
namespace fs = std::filesystem;
namespace {

// What IFC4 renamed without changing what it holds. Anything else in a different place is refused.
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> RENAMED = {{
    {"CompositionType", "PredefinedType"},  // of IfcBuildingElementProxy, whose new list holds all the old values
    {"Id", "Identification"},
    {"ItemReference", "Identification"},
}};

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// Where the name in FILE_SCHEMA is, as an offset and a length; a length of 0 if it isn't there
std::pair<size_t, size_t> schema_name_in(std::string_view header) {
    const auto at = header.find("FILE_SCHEMA");
    if (at == std::string_view::npos) return {0, 0};
    const auto open = header.find('\'', at);
    if (open == std::string_view::npos) return {0, 0};
    const auto close = header.find('\'', open + 1);
    if (close == std::string_view::npos) return {0, 0};
    return {open + 1, close - open - 1};
}

bool is_number(std::string_view value) {
    if (!value.empty() && (value[0] == '-' || value[0] == '+')) value.remove_prefix(1);
    return !value.empty() && (std::isdigit(static_cast<unsigned char>(value[0])) || value[0] == '.');
}

bool holds(const ifcopenshell::select_type& select, const ifcopenshell::declaration* wanted) {
    for (const auto* item : select.select_list()) {
        if (item == wanted) return true;
        if (const auto* inner = item->as_select_type(); inner && holds(*inner, wanted)) return true;
    }
    return false;
}

class Upgrader {
  public:
    explicit Upgrader(const SpfFile& spf)
        : spf_(spf), before_(ifcopenshell::schema_by_name("IFC2X3")), after_(ifcopenshell::schema_by_name("IFC4")) {}

    Upgrade run(const fs::path& path) {
        Upgrade upgrade;
        {
            std::ofstream out(path, std::ios::binary);
            const auto header = spf_.header();
            const auto [at, length] = schema_name_in(header);
            out << header.substr(0, at) << "IFC4" << header.substr(at + length);
            for (uint32_t id = 0; id < spf_.max_id(); ++id) {
                if (!spf_.line(id).empty()) convert(id, out, upgrade);
            }
            out << "ENDSEC;\nEND-ISO-10303-21;\n";
        }
        if (!refused_.empty()) {
            fs::remove(path);
            for (const auto& [reason, seen] : refused_) {
                upgrade.refused.push_back(reason + (seen.second.empty() ? "" : ", such as " + seen.second) + " (" + std::to_string(seen.first) + " times)");
            }
        }
        return upgrade;
    }

  private:
    // What's known about a type: only worked out once for each
    struct Type {
        const ifcopenshell::entity* entity = nullptr;
        std::string refused;  // why this type can't go across, whatever its values
        int predefined = -1, element_type = -1;  // where these attributes are, if it has them
    };

    void refuse(const std::string& reason, const std::string& example = "") {
        auto& seen = refused_[reason];
        if (!seen.first) seen.second = example;
        ++seen.first;
    }

    const ifcopenshell::entity* entity_in(const ifcopenshell::schema_definition* schema, const std::string& name) const {
        try {
            return schema->declaration_by_name(name)->as_entity();
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    const Type& type_of(const std::string& name, bool from_before) {
        const auto known = types_.find(name);
        if (known != types_.end()) return known->second;
        Type type;
        type.entity = entity_in(after_, name);
        if (!type.entity) {
            type.refused = name + " isn't in IFC4";
        } else if (type.entity->is_abstract()) {
            type.refused = name + " can't be used directly in IFC4";
        } else {
            const auto& now = type.entity->all_attributes();
            for (size_t i = 0; i < now.size(); ++i) {
                if (now[i]->name() == "PredefinedType") type.predefined = static_cast<int>(i);
                if (now[i]->name() == "ElementType") type.element_type = static_cast<int>(i);
            }
            if (from_before) {
                const auto* old_entity = entity_in(before_, name);
                if (!old_entity) {
                    type.refused = name + " isn't in IFC2X3";
                } else {
                    const auto& then = old_entity->all_attributes();
                    if (then.size() > now.size()) type.refused = name + " has fewer attributes in IFC4";
                    for (size_t i = 0; i < then.size() && i < now.size(); ++i) {
                        if (then[i]->name() == now[i]->name()) continue;
                        const auto rename = std::pair<std::string_view, std::string_view>(then[i]->name(), now[i]->name());
                        if (std::find(RENAMED.begin(), RENAMED.end(), rename) == RENAMED.end()) {
                            type.refused = name + "." + then[i]->name() + " is now " + now[i]->name();
                        }
                    }
                }
            }
        }
        return types_.emplace(name, std::move(type)).first->second;
    }

    bool fits_simple(std::string_view value, ifcopenshell::simple_type::data_type type) const {
        using simple = ifcopenshell::simple_type;
        switch (type) {
            case simple::binary_type: return !value.empty() && value[0] == '"';
            case simple::boolean_type: return value == ".T." || value == ".F.";
            case simple::logical_type: return value == ".T." || value == ".F." || value == ".U.";
            case simple::integer_type: return is_number(value) && value.find('.') == std::string_view::npos;
            case simple::number_type:
            case simple::real_type: return is_number(value);
            case simple::string_type: return !value.empty() && value[0] == '\'';
            default: return false;
        }
    }

    bool fits(std::string_view value, const ifcopenshell::parameter_type* type) {
        if (const auto* named = type->as_named_type()) return fits(value, named->declared_type());
        if (const auto* simple = type->as_simple_type()) return fits_simple(value, simple->declared_type());
        if (const auto* list = type->as_aggregation_type()) {
            if (value.size() < 2 || value.front() != '(' || value.back() != ')') return false;
            if (trimmed(value.substr(1, value.size() - 2)).empty()) return true;
            for (const auto item : attributes_of(value)) {
                if (!fits(trimmed(item), list->type_of_element())) return false;
            }
            return true;
        }
        return false;
    }

    bool fits(std::string_view value, const ifcopenshell::declaration* declared) {
        if (const auto* entity = declared->as_entity()) {
            if (value.size() < 2 || value[0] != '#') return false;
            uint32_t target = 0;
            for (const char c : value.substr(1)) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
                target = target * 10 + (c - '0');
            }
            if (spf_.line(target).empty()) return false;
            const auto& target_type = type_of(std::string(spf_.type(target)), true);
            return target_type.entity && target_type.entity->is(*entity);
        }
        if (const auto* listed = declared->as_enumeration_type()) {
            if (value.size() < 3 || value.front() != '.' || value.back() != '.') return false;
            const auto word = value.substr(1, value.size() - 2);
            const auto& items = listed->enumeration_items();
            return std::find(items.begin(), items.end(), word) != items.end();
        }
        if (const auto* defined = declared->as_type_declaration()) return fits(value, defined->declared_type());
        if (const auto* select = declared->as_select_type()) {
            // A value in a select says what it is: IFCLABEL('...')
            if (!value.empty() && std::isalpha(static_cast<unsigned char>(value[0]))) {
                const auto open = value.find('(');
                if (open == std::string_view::npos || value.back() != ')') return false;
                const ifcopenshell::declaration* wrapper = nullptr;
                try {
                    wrapper = after_->declaration_by_name(std::string(value.substr(0, open)));
                } catch (const std::exception&) {
                    return false;
                }
                return holds(*select, wrapper) && fits(trimmed(value.substr(open + 1, value.size() - open - 2)), wrapper);
            }
            for (const auto* item : select->select_list()) {
                if (fits(value, item)) return true;
            }
        }
        return false;
    }

    void convert(uint32_t id, std::ofstream& out, Upgrade& upgrade) {
        const auto line = spf_.line(id);
        std::string name(spf_.type(id));
        const auto attributes = attributes_of(line);
        std::vector<std::string> values;
        values.reserve(attributes.size() + 2);
        for (const auto attribute : attributes) values.emplace_back(trimmed(attribute));
        if (values.size() == 1 && values[0].empty()) values.clear();

        bool changed = false;
        if (name == "IFCRELCONNECTSPORTTOELEMENT" && values.size() == 6) {
            // IFC4 only lets this relate to distribution elements, and has ports nested in what they're on instead
            name = "IFCRELNESTS";
            values = {values[0], values[1], values[2], values[3], values[5], "(" + values[4] + ")"};
            changed = true;
            ++upgrade.ports;
        }

        const Type& type = type_of(name, !changed);
        if (!type.refused.empty()) {
            refuse(type.refused);
            return;
        }
        const auto& now = type.entity->all_attributes();
        if (values.size() > now.size()) {
            refuse(name + " has too many attributes");
            return;
        }
        if (values.size() < now.size()) {
            values.resize(now.size(), "$");
            changed = true;
            ++upgrade.lengthened;
        }

        // A predefined type that's gone becomes user defined, under its old name
        if (type.predefined >= 0 && type.element_type >= 0) {
            auto& predefined = values[type.predefined];
            if (predefined != "$" && !fits(predefined, now[type.predefined]->type_of_attribute()) &&
                fits(".USERDEFINED.", now[type.predefined]->type_of_attribute()) && predefined.size() > 2 && predefined.front() == '.') {
                auto& element_type = values[type.element_type];
                if (element_type == "$") element_type = "'" + predefined.substr(1, predefined.size() - 2) + "'";
                predefined = ".USERDEFINED.";
                changed = true;
                ++upgrade.retyped;
            }
        }

        const auto& derived = type.entity->derived();
        for (size_t i = 0; i < values.size(); ++i) {
            const auto& value = values[i];
            const bool is_derived = i < derived.size() && derived[i];
            const bool fine = value == "*" ? is_derived
                : is_derived               ? false
                : value == "$"             ? now[i]->optional()
                                           : fits(value, now[i]->type_of_attribute());
            if (!fine) refuse(name + "." + now[i]->name() + " can't hold what's there", value.substr(0, 40));
        }

        if (!changed) {
            out << line << '\n';
            return;
        }
        out << '#' << id << '=' << name << '(';
        for (size_t i = 0; i < values.size(); ++i) out << (i ? "," : "") << values[i];
        out << ");\n";
    }

    const SpfFile& spf_;
    const ifcopenshell::schema_definition* before_;
    const ifcopenshell::schema_definition* after_;
    std::unordered_map<std::string, Type> types_;
    std::map<std::string, std::pair<uint64_t, std::string>> refused_;  // how often, and the first value
};

}  // namespace

std::string schema_in_header(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string start(1 << 16, '\0');
    in.read(start.data(), static_cast<std::streamsize>(start.size()));
    start.resize(static_cast<size_t>(in.gcount()));
    const auto [at, length] = schema_name_in(start);
    return start.substr(at, length);
}

Upgrade upgrade_to_ifc4(const SpfFile& spf, const fs::path& path) {
    return Upgrader(spf).run(path);
}

}  // namespace ifcfilter

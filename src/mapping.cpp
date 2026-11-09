#include "mapping.h"

#include "rigid.h"
#include "spf.h"
#include "system.h"
#include "upgrade.h"

#include <ifcparse/schema.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace bimpass {
namespace fs = std::filesystem;
namespace {

constexpr size_t EXAMPLES = 3;
constexpr size_t EXAMPLE_LENGTH = 60;

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

// What's between a value's quotes, as it's written, with '' for a quote
std::string quoted_in(std::string_view value) {
    const auto open = value.find('\'');
    const auto close = value.rfind('\'');
    return open == std::string_view::npos || close <= open ? std::string() : std::string(value.substr(open + 1, close - open - 1));
}

// A value as a person would read it: a string's text, or a number or enumeration as it's written
std::string readable(std::string_view value) {
    if (value == "$" || value == "*") return {};
    if (value.find('\'') != std::string_view::npos) return quoted_in(value);
    const auto open = value.find('(');
    const auto close = value.rfind(')');
    if (open != std::string_view::npos && close != std::string_view::npos && close > open) return std::string(value.substr(open + 1, close - open - 1));
    return std::string(value);
}

std::string csv_field(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

// One line of a CSV file, as Excel writes it: quoted fields can hold commas, quotes and line breaks
bool read_csv_row(std::istream& in, std::vector<std::string>& fields) {
    fields.clear();
    std::string field;
    bool quoted = false, any = false;
    char c;
    while (in.get(c)) {
        any = true;
        if (quoted) {
            if (c == '"') {
                if (in.peek() == '"') {
                    in.get(c);
                    field += '"';
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            fields.push_back(std::move(field));
            field.clear();
        } else if (c == '\n') {
            break;
        } else if (c != '\r') {
            field += c;
        }
    }
    if (!any) return false;
    fields.push_back(std::move(field));
    return true;
}

const std::vector<std::string> COLUMNS = {"kind", "class", "set", "name", "action", "new_name", "elements", "models", "disciplines", "on_classes", "examples"};

struct Row {
    std::string kind, ifc_class, set, name, action, new_name;
    uint64_t elements = 0;
    std::set<std::string> models, disciplines;
    std::map<std::string, uint64_t> on_classes;
    std::vector<std::string> examples;

    void example(const std::string& value) {
        if (value.empty() || examples.size() >= EXAMPLES) return;
        const std::string cut = value.size() > EXAMPLE_LENGTH ? value.substr(0, EXAMPLE_LENGTH) + "..." : value;
        if (std::find(examples.begin(), examples.end(), cut) == examples.end()) examples.push_back(cut);
    }
};

int order_of(const std::string& kind) {
    static const std::vector<std::string> KINDS = {"class", "attribute", "set", "property"};
    const auto at = std::find(KINDS.begin(), KINDS.end(), kind);
    return static_cast<int>(at - KINDS.begin());
}

std::string key_of(const std::string& kind, const std::string& ifc_class, const std::string& set, const std::string& name) {
    return std::to_string(order_of(kind)) + '\x1f' + upper(ifc_class) + '\x1f' + set + '\x1f' + name;
}

bool is_text(const ifcopenshell::parameter_type* type) {
    for (int depth = 0; type && depth < 8; ++depth) {
        if (const auto* simple = type->as_simple_type()) return simple->declared_type() == ifcopenshell::simple_type::string_type;
        const auto* named = type->as_named_type();
        if (!named) return false;
        const auto* declared = named->declared_type()->as_type_declaration();
        if (!declared) return false;
        type = declared->declared_type();
    }
    return false;
}

bool is_set_type(std::string_view type) { return type == "IFCPROPERTYSET" || type == "IFCELEMENTQUANTITY"; }

bool is_property_type(std::string_view type) {
    const bool property = type.rfind("IFCPROPERTY", 0) == 0 && type != "IFCPROPERTYSET" && type.find("RELATIONSHIP") == std::string_view::npos &&
                          type != "IFCPROPERTYENUMERATION" && type != "IFCPROPERTYSETTEMPLATE" && type != "IFCPROPERTYTEMPLATE";
    return property || type.rfind("IFCQUANTITY", 0) == 0 || type == "IFCCOMPLEXPROPERTY" || type == "IFCPHYSICALCOMPLEXQUANTITY";
}

// UTF-8 text, as Excel saves it, as a STEP string's contents: quotes and backslashes doubled, and what isn't
// ASCII as \X2\ hex
std::string step_text(const std::string& utf8) {
    std::string out;
    std::string wide;
    const auto flush = [&]() {
        if (wide.empty()) return;
        out += "\\X2\\" + wide + "\\X0\\";
        wide.clear();
    };
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            flush();
            if (c == '\'') out += "''";
            else if (c == '\\') out += "\\\\";
            else out += static_cast<char>(c);
            ++i;
            continue;
        }
        uint32_t code = 0;
        size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        code = c & (length == 4 ? 0x07 : length == 3 ? 0x0F : 0x1F);
        for (size_t k = 1; k < length && i + k < utf8.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        i += length;
        char hex[16];
        if (code > 0xFFFF) code = 0xFFFD;  // outside what \X2\ holds
        std::snprintf(hex, sizeof hex, "%04X", code);
        wide += hex;
    }
    flush();
    return out;
}

// Where a set's name and members are
std::pair<size_t, size_t> set_attributes(std::string_view type) { return type == "IFCELEMENTQUANTITY" ? std::make_pair(size_t(2), size_t(5)) : std::make_pair(size_t(2), size_t(4)); }

class Inventory {
  public:
    void add(const fs::path& path) {
        const SpfFile spf(path.string());
        const std::string schema_name = schema_in_header(path);
        const ifcopenshell::schema_definition* schema = nullptr;
        try {
            schema = ifcopenshell::schema_by_name(schema_name.rfind("IFC2X3", 0) == 0 ? "IFC2X3" : schema_name.rfind("IFC4X3", 0) == 0 ? "IFC4X3_ADD2" : "IFC4");
        } catch (const std::exception&) {
            return;
        }
        const std::string model = path.stem().string();
        const std::string discipline = fs::absolute(path).parent_path().filename().string();
        std::unordered_map<std::string, Row> rows;
        const auto row = [&](const std::string& kind, const std::string& ifc_class, const std::string& set, const std::string& name) -> Row& {
            Row& r = rows[key_of(kind, ifc_class, set, name)];
            if (r.kind.empty()) r = Row{kind, ifc_class, set, name};
            return r;
        };

        // Classes and their text attributes, for what's an object or a type of one
        struct Kind {
            const ifcopenshell::entity* entity = nullptr;
            std::vector<std::pair<size_t, std::string>> text;  // index, and name, of each text attribute after GlobalId
        };
        std::unordered_map<std::string, Kind> kinds;
        const auto* object = schema->declaration_by_name("IfcObjectDefinition")->as_entity();
        const auto* type_object = schema->declaration_by_name("IfcTypeObject")->as_entity();
        std::unordered_map<uint32_t, std::string> class_of;  // what objects are, to say what a set is on
        std::unordered_map<uint32_t, std::pair<std::string, std::vector<uint32_t>>> sets;
        std::unordered_map<uint32_t, uint64_t> set_count;
        std::unordered_map<uint32_t, std::map<std::string, uint64_t>> set_classes;
        std::unordered_map<uint32_t, std::pair<std::string, std::string>> properties;  // name, and an example value

        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            const auto line = spf.line(id);
            if (line.empty()) continue;
            const std::string type(spf.type(id));
            if (is_set_type(type)) {
                const auto attributes = attributes_of(line);
                const auto [name_at, members_at] = set_attributes(type);
                if (attributes.size() > members_at) sets[id] = {quoted_in(attributes[name_at]), refs_in(attributes[members_at])};
                continue;
            }
            if (is_property_type(type)) {
                const auto attributes = attributes_of(line);
                if (attributes.empty()) continue;
                const size_t value_at = type.rfind("IFCQUANTITY", 0) == 0 ? 3 : 2;
                properties[id] = {quoted_in(attributes[0]), attributes.size() > value_at ? readable(attributes[value_at]) : std::string()};
                continue;
            }
            if (type == "IFCRELDEFINESBYPROPERTIES") {
                const auto attributes = attributes_of(line);
                if (attributes.size() != 6) continue;
                const auto related = refs_in(attributes[4]);
                for (const uint32_t set : refs_in(attributes[5])) set_count[set] += related.size();
                continue;
            }
            if (type.rfind("IFCREL", 0) == 0) continue;
            auto known = kinds.find(type);
            if (known == kinds.end()) {
                Kind kind;
                try {
                    const auto* entity = schema->declaration_by_name(type)->as_entity();
                    if (entity && entity->is(*object)) {
                        kind.entity = entity;
                        const auto& all = entity->all_attributes();
                        for (size_t i = 1; i < all.size(); ++i) {
                            if (is_text(all[i]->type_of_attribute())) kind.text.emplace_back(i, all[i]->name());
                        }
                    }
                } catch (const std::exception&) {
                }
                known = kinds.emplace(type, std::move(kind)).first;
            }
            if (!known->second.entity) continue;
            const std::string& name = known->second.entity->name();
            class_of[id] = name;
            Row& c = row("class", name, "", "");
            ++c.elements;
            const auto attributes = attributes_of(line);
            for (const auto& [index, attribute] : known->second.text) {
                if (index >= attributes.size() || attributes[index] == "$") continue;
                Row& a = row("attribute", name, "", attribute);
                ++a.elements;
                a.example(readable(attributes[index]));
            }
            // A type's own sets, which count as one each
            if (known->second.entity->is(*type_object) && attributes.size() > 5) {
                for (const uint32_t set : refs_in(attributes[5])) {
                    ++set_count[set];
                    ++set_classes[set][name];
                }
            }
        }
        // What a set is on is only known once every object has been read
        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            if (spf.line(id).empty() || spf.type(id) != "IFCRELDEFINESBYPROPERTIES") continue;
            const auto attributes = attributes_of(spf.line(id));
            if (attributes.size() != 6) continue;
            for (const uint32_t set : refs_in(attributes[5])) {
                for (const uint32_t r : refs_in(attributes[4])) {
                    const auto c = class_of.find(r);
                    if (c != class_of.end()) ++set_classes[set][c->second];
                }
            }
        }
        for (const auto& [id, set] : sets) {
            const std::string& set_name = set.first;
            Row& s = row("set", "", set_name, "");
            const uint64_t count = set_count[id];
            s.elements += count;
            for (const auto& [c, n] : set_classes[id]) s.on_classes[c] += n;
            for (const uint32_t member : set.second) {
                const auto property = properties.find(member);
                if (property == properties.end()) continue;
                Row& p = row("property", "", set_name, property->second.first);
                p.elements += count;
                for (const auto& [c, n] : set_classes[id]) p.on_classes[c] += n;
                p.example(property->second.second);
            }
        }

        const std::lock_guard<std::mutex> guard(lock_);
        for (auto& [key, r] : rows) {
            Row& into = rows_[key];
            if (into.kind.empty()) {
                into.kind = r.kind;
                into.ifc_class = r.ifc_class;
                into.set = r.set;
                into.name = r.name;
            }
            into.elements += r.elements;
            into.models.insert(model);
            into.disciplines.insert(discipline);
            for (const auto& [c, n] : r.on_classes) into.on_classes[c] += n;
            for (const auto& e : r.examples) into.example(e);
        }
    }

    // Keeps what a person has written in an earlier copy
    void keep_from(const std::string& csv_path) {
        std::ifstream in(csv_path, std::ios::binary);
        if (!in) return;
        if (in.peek() == 0xEF) in.ignore(3);  // Excel's byte order mark
        std::vector<std::string> fields;
        if (!read_csv_row(in, fields)) return;
        std::map<std::string, size_t> at;
        for (size_t i = 0; i < fields.size(); ++i) at[trimmed(fields[i])] = i;
        for (const char* needed : {"kind", "class", "set", "name", "action", "new_name"}) {
            if (!at.count(needed)) throw std::runtime_error(csv_path + ": no " + needed + " column");
        }
        const auto get = [&](const char* column) { return at[column] < fields.size() ? trimmed(fields[at[column]]) : std::string(); };
        while (read_csv_row(in, fields)) {
            if (fields.size() == 1 && trimmed(fields[0]).empty()) continue;
            Row& r = rows_[key_of(get("kind"), get("class"), get("set"), get("name"))];
            if (r.kind.empty()) {
                r.kind = get("kind");
                r.ifc_class = get("class");
                r.set = get("set");
                r.name = get("name");
            }
            r.action = get("action");
            r.new_name = get("new_name");
        }
    }

    void write(const std::string& csv_path) const {
        const std::string temporary = csv_path + ".new";
        {
            std::ofstream out(temporary, std::ios::binary);
            out << "\xEF\xBB\xBF";  // so Excel reads it as UTF-8
            for (size_t i = 0; i < COLUMNS.size(); ++i) out << (i ? "," : "") << COLUMNS[i];
            out << "\r\n";
            for (const auto& [key, r] : rows_) {
                std::vector<std::pair<uint64_t, std::string>> classes;
                for (const auto& [c, n] : r.on_classes) classes.emplace_back(n, c);
                std::sort(classes.rbegin(), classes.rend());
                std::string on, disciplines, examples;
                for (size_t i = 0; i < classes.size() && i < 4; ++i) on += (i ? "; " : "") + classes[i].second + " " + std::to_string(classes[i].first);
                if (classes.size() > 4) on += "; ...";
                for (const auto& d : r.disciplines) disciplines += (disciplines.empty() ? "" : " ") + d;
                for (const auto& e : r.examples) examples += (examples.empty() ? "" : " | ") + e;
                out << csv_field(r.kind) << ',' << csv_field(r.ifc_class) << ',' << csv_field(r.set) << ',' << csv_field(r.name) << ',' << csv_field(r.action) << ','
                    << csv_field(r.new_name) << ',' << r.elements << ',' << r.models.size() << ',' << csv_field(disciplines) << ',' << csv_field(on) << ',' << csv_field(examples) << "\r\n";
            }
        }
        replace_file(temporary, csv_path);
    }

    size_t size() const { return rows_.size(); }

  private:
    std::mutex lock_;
    std::map<std::string, Row> rows_;  // in order: classes, attributes, sets, properties, each by name
};

std::vector<fs::path> ifc_files(const std::vector<std::string>& inputs) {
    std::vector<fs::path> out;
    for (const std::string& input : inputs) {
        if (fs::is_directory(input)) {
            for (const auto& entry : fs::directory_iterator(input)) {
                const auto ext = upper(entry.path().extension().string());
                if (entry.is_regular_file() && ext == ".IFC" && entry.path().string().find(".original.") == std::string::npos) out.push_back(entry.path());
            }
        } else {
            out.emplace_back(input);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

bool Mapping::empty() const {
    return retype.empty() && blank.empty() && delete_set.empty() && rename_set.empty() && delete_property.empty() && rename_property.empty();
}

Mapping load_mapping(const std::string& path) {
    Mapping mapping;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(path + ": can't be read");
    if (in.peek() == 0xEF) in.ignore(3);
    std::vector<std::string> fields;
    if (!read_csv_row(in, fields)) return mapping;
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < fields.size(); ++i) at[trimmed(fields[i])] = i;
    for (const char* needed : {"kind", "class", "set", "name", "action", "new_name"}) {
        if (!at.count(needed)) throw std::runtime_error(path + ": no " + needed + " column");
    }
    size_t line = 1;
    while (read_csv_row(in, fields)) {
        ++line;
        const auto get = [&](const char* column) { return at[column] < fields.size() ? trimmed(fields[at[column]]) : std::string(); };
        const std::string kind = get("kind"), action = upper(get("action")), new_name = get("new_name");
        if (action.empty() || action == "KEEP") continue;
        const auto fail = [&](const std::string& why) { throw std::runtime_error(path + " line " + std::to_string(line) + ": " + why); };
        if ((action == "RENAME") == new_name.empty()) fail(action == "RENAME" ? "rename needs a new_name" : "a new_name only goes with rename");
        if (kind == "class") {
            if (action != "RENAME") fail("a class can only be renamed, to another class");
            mapping.retype[upper(get("class"))] = new_name;
        } else if (kind == "attribute") {
            if (action != "DELETE") fail("an attribute can only be deleted, which leaves it empty");
            mapping.blank.emplace(upper(get("class")), get("name"));
        } else if (kind == "set") {
            if (action == "DELETE") mapping.delete_set.insert(get("set"));
            else if (action == "RENAME") mapping.rename_set[get("set")] = new_name;
            else fail("unknown action " + action);
        } else if (kind == "property") {
            if (action == "DELETE") mapping.delete_property.emplace(get("set"), get("name"));
            else if (action == "RENAME") mapping.rename_property[{get("set"), get("name")}] = new_name;
            else fail("unknown action " + action);
        } else {
            fail("unknown kind " + kind);
        }
    }
    return mapping;
}

MappingStats apply_mapping(const std::string& path, const Mapping& mapping) {
    MappingStats stats;
    if (mapping.empty()) return stats;
    const std::string temporary = path + ".mapping";
    {
        const SpfFile spf(path);
        const uint32_t max_id = spf.max_id();
        const std::string schema_name = schema_in_header(path);
        const ifcopenshell::schema_definition* schema = ifcopenshell::schema_by_name(schema_name.rfind("IFC2X3", 0) == 0 ? "IFC2X3" : "IFC4");
        std::set<std::string> warned;
        const auto warn = [&](const std::string& message) {
            if (warned.insert(message).second) stats.warnings.push_back(message);
        };

        std::unordered_map<uint32_t, std::string> replaced;
        std::vector<std::string> added;
        uint32_t next_id = max_id;
        std::vector<bool> gone(max_id, false);
        std::unordered_set<uint32_t> members_before, members_after;

        // Sets and the properties in them
        std::unordered_map<std::string, uint32_t> renamed_copy;  // property, and new name, to the copy with it
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto line = spf.line(id);
            if (line.empty() || !is_set_type(spf.type(id))) continue;
            const auto attributes = attributes_of(line);
            const auto [name_at, members_at] = set_attributes(spf.type(id));
            if (attributes.size() <= members_at) continue;
            const std::string name = quoted_in(attributes[name_at]);
            const auto members = refs_in(attributes[members_at]);
            for (const uint32_t m : members) members_before.insert(m);
            if (mapping.delete_set.count(name)) {
                gone[id] = true;
                ++stats.sets_deleted;
                continue;
            }
            std::vector<uint32_t> kept;
            bool changed = false;
            for (const uint32_t member : members) {
                const auto parts = attributes_of(spf.line(member));
                const std::string property = parts.empty() ? std::string() : quoted_in(parts[0]);
                if (mapping.delete_property.count({name, property})) {
                    changed = true;
                    ++stats.properties_deleted;
                    continue;
                }
                const auto rename = mapping.rename_property.find({name, property});
                if (rename == mapping.rename_property.end()) {
                    kept.push_back(member);
                    continue;
                }
                const std::string copy_key = std::to_string(member) + '\x1f' + rename->second;
                auto copy = renamed_copy.find(copy_key);
                if (copy == renamed_copy.end()) {
                    const auto text = spf.line(member);
                    const auto open = text.find('\'');
                    const auto close = open == std::string_view::npos ? open : text.find('\'', open + 1);
                    if (close == std::string_view::npos) {
                        kept.push_back(member);
                        continue;
                    }
                    const uint32_t new_id = next_id++;
                    std::string rewritten = "#" + std::to_string(new_id) + std::string(text.substr(text.find('='), open - text.find('=') + 1)) + step_text(rename->second) + std::string(text.substr(close));
                    added.push_back(std::move(rewritten));
                    copy = renamed_copy.emplace(copy_key, new_id).first;
                }
                kept.push_back(copy->second);
                changed = true;
                ++stats.properties_renamed;
            }
            const auto set_rename = mapping.rename_set.find(name);
            if (kept.empty()) {
                // A set can't be empty
                gone[id] = true;
                ++stats.sets_emptied;
                continue;
            }
            for (const uint32_t m : kept) members_after.insert(m);
            if (!changed && set_rename == mapping.rename_set.end()) continue;
            std::string text = "#" + std::to_string(id) + "=" + std::string(spf.type(id)) + "(";
            for (size_t i = 0; i < attributes.size(); ++i) {
                if (i) text += ',';
                if (i == name_at && set_rename != mapping.rename_set.end()) text += "'" + step_text(set_rename->second) + "'";
                else if (i == members_at) text += list_of(kept);
                else text += std::string(attributes[i]);
            }
            text += ");";
            replaced[id] = std::move(text);
            if (set_rename != mapping.rename_set.end()) ++stats.sets_renamed;
        }
        // Properties no set has any more go, unless something else, such as a complex property, uses them
        std::unordered_set<uint32_t> used_elsewhere;
        for (uint32_t id = 0; id < max_id; ++id) {
            if (spf.line(id).empty() || is_set_type(spf.type(id)) || members_before.count(id)) continue;
            if (spf.type(id) == "IFCCOMPLEXPROPERTY" || spf.type(id) == "IFCPHYSICALCOMPLEXQUANTITY") {
                for_each_ref(spf.line(id), [&](uint32_t next) { used_elsewhere.insert(next); });
            }
        }
        for (const uint32_t m : members_before) {
            if (!members_after.count(m) && !used_elsewhere.count(m) && m < max_id) gone[m] = true;
        }

        // Classes and attributes
        std::unordered_map<std::string, std::pair<const ifcopenshell::entity*, std::vector<std::pair<size_t, bool>>>> plans;  // type: new class, blanked attributes
        const auto* element = schema->declaration_by_name("IfcElement")->as_entity();
        const size_t element_attributes = element->all_attributes().size();
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto line = spf.line(id);
            if (line.empty() || gone[id]) continue;
            const std::string type(spf.type(id));
            auto plan = plans.find(type);
            if (plan == plans.end()) {
                const ifcopenshell::entity* from = nullptr;
                try {
                    from = schema->declaration_by_name(type)->as_entity();
                } catch (const std::exception&) {
                }
                const ifcopenshell::entity* to = nullptr;
                std::vector<std::pair<size_t, bool>> blanks;
                if (from) {
                    const auto retype = mapping.retype.find(type);
                    if (retype != mapping.retype.end()) {
                        try {
                            to = schema->declaration_by_name(retype->second)->as_entity();
                        } catch (const std::exception&) {
                        }
                        if (!to || to->is_abstract() || !to->is(*element) || !from->is(*element)) {
                            warn(from->name() + " can't become " + retype->second + ": both have to be kinds of IfcElement that can be used directly");
                            to = nullptr;
                        }
                    }
                    const auto& all = from->all_attributes();
                    for (size_t i = 0; i < all.size(); ++i) {
                        if (!mapping.blank.count({type, all[i]->name()})) continue;
                        if (!all[i]->optional()) warn(from->name() + "." + all[i]->name() + " can't be left empty");
                        else blanks.emplace_back(i, true);
                    }
                }
                plan = plans.emplace(type, std::make_pair(to, std::move(blanks))).first;
            }
            const auto& [to, blanks] = plan->second;
            if (!to && blanks.empty()) continue;
            auto attributes = attributes_of(replaced.count(id) ? std::string_view(replaced[id]) : line);
            std::vector<std::string> values(attributes.begin(), attributes.end());
            for (const auto& [index, _] : blanks) {
                if (index < values.size() && values[index] != "$") {
                    values[index] = "$";
                    ++stats.blanked;
                }
            }
            std::string name = type;
            if (to) {
                // What an element has is kept; what only the old class had, such as its predefined type, isn't
                values.resize(element_attributes);
                values.resize(to->all_attributes().size(), "$");
                name = upper(to->name());
                ++stats.retyped;
            }
            std::string text = "#" + std::to_string(id) + "=" + name + "(";
            for (size_t i = 0; i < values.size(); ++i) text += (i ? "," : "") + values[i];
            replaced[id] = text + ");";
        }

        const auto is_gone = [&](uint32_t id) { return id < gone.size() && gone[id]; };
        std::ofstream out(temporary, std::ios::binary);
        out << spf.header();
        std::string text;
        for (uint32_t id = 0; id < max_id; ++id) {
            if (spf.line(id).empty() || gone[id]) continue;
            const auto at = replaced.find(id);
            const std::string_view line = at == replaced.end() ? spf.line(id) : std::string_view(at->second);
            // A relationship to a set that's gone goes too; a type's list of sets just loses it
            const WhenEmpty when_empty = spf.type(id).rfind("IFCREL", 0) == 0 ? WhenEmpty::Drop : WhenEmpty::Null;
            if (rewrite(line, is_gone, when_empty, text)) out << text << '\n';
        }
        for (const std::string& line : added) out << line << '\n';
        out << "ENDSEC;\nEND-ISO-10303-21;\n";
    }
    replace_file(temporary, path);
    return stats;
}

int inventory(const std::vector<std::string>& inputs, const std::string& csv_path, unsigned jobs) {
    const auto files = ifc_files(inputs);
    if (files.empty()) {
        std::cerr << "no models found\n";
        return 2;
    }
    Inventory all;
    all.keep_from(csv_path);
    std::atomic<size_t> next{0}, done{0};
    std::mutex print;
    const auto work = [&]() {
        while (true) {
            const size_t i = next++;
            if (i >= files.size()) return;
            try {
                all.add(files[i]);
            } catch (const std::exception& e) {
                const std::lock_guard<std::mutex> guard(print);
                std::cerr << files[i].string() << ": " << e.what() << "\n";
            }
            const size_t n = ++done;
            if (n % 25 == 0 || n == files.size()) {
                const std::lock_guard<std::mutex> guard(print);
                std::cout << n << "/" << files.size() << " models read\n" << std::flush;
            }
        }
    };
    if (!jobs) jobs = std::max(1u, std::thread::hardware_concurrency() / 2);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < jobs; ++t) threads.emplace_back(work);
    for (auto& t : threads) t.join();
    all.write(csv_path);
    std::cout << csv_path << ": " << all.size() << " rows from " << files.size() << " models\n";
    return 0;
}

}  // namespace bimpass

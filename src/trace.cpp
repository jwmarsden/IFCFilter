#include "trace.h"

#include "site.h"
#include "spf.h"
#include "system.h"
#include "uniclass.h"

#include <fstream>
#include <array>
#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ifcfilter {
namespace {

// What's between a value's quotes, as it's written, with '' for a quote
std::string quoted_in(std::string_view value) {
    const auto open = value.find('\'');
    const auto close = value.rfind('\'');
    return open == std::string_view::npos || close <= open ? std::string() : std::string(value.substr(open + 1, close - open - 1));
}

// The GUID a line starts with, if it has one, without splitting the whole line
std::string_view guid_of(std::string_view line) {
    const auto open = line.find('(');
    if (open == std::string_view::npos || open + 25 >= line.size() || line[open + 1] != '\'' || line[open + 24] != '\'') return {};
    return line.substr(open + 2, 22);
}

// Plain text as STEP writes it, without its quotes
std::string escaped(const std::string& text) {
    std::string out;
    for (const char c : text) {
        out += c;
        if (c == '\'' || c == '\\') out += c;
    }
    return out;
}

// Values as one, in order, each once
std::string joined(const std::vector<std::string>& values) {
    std::string out;
    std::set<std::string> seen;
    for (const std::string& value : values) {
        if (value.empty() || !seen.insert(value).second) continue;
        out += (out.empty() ? "" : "; ") + value;
    }
    return out;
}

struct Found {
    std::string zone, site, location, design, identifier, description, package_code, package_name;
    std::array<std::string, 12> classification;
};

std::string csv(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

}  // namespace

TraceStats trace(const std::string& path, const SpfFile& input, const std::string& file_name, const std::vector<Traced>& elements, const SiteLookup& corrections, const PropertySchema& property_schema, const LocationReference& location_reference, const ElementCorrections& element_corrections) {
    TraceStats stats;
    const Schema& s = schema();

    // The model's own zone, from its file name, if it's one: a model of a group of zones, has none
    std::string model_zone;
    {
        std::stringstream parts(file_name.substr(0, file_name.rfind('.')));
        std::string part;
        while (model_zone.empty() && std::getline(parts, part, '-')) {
            if (s.zones.count(part)) model_zone = part;
        }
    }

    // The input's sets of properties on what's traced
    std::unordered_set<uint32_t> wanted;
    for (const Traced& traced : elements) wanted.insert(traced.sources.begin(), traced.sources.end());
    std::unordered_map<uint32_t, std::vector<uint32_t>> sets_of;
    for (uint32_t id = 0; id < input.max_id(); ++id) {
        if (input.line(id).empty() || input.type(id) != "IFCRELDEFINESBYPROPERTIES") continue;
        const auto attributes = attributes_of(input.line(id));
        if (attributes.size() != 6) continue;
        const auto set = refs_in(attributes[5]);
        if (set.size() != 1) continue;
        for (const uint32_t element : refs_in(attributes[4])) {
            if (wanted.count(element)) sets_of[element].push_back(set[0]);
        }
    }
    // Type properties also supply inherited classification values.
    for (uint32_t id = 0; id < input.max_id(); ++id) {
        if (input.type(id) != "IFCRELDEFINESBYTYPE") continue;
        const auto a = attributes_of(input.line(id));
        if (a.size() != 6) continue;
        for (const auto type : refs_in(a[5])) {
            const auto ta = attributes_of(input.line(type));
            if (ta.size() <= 5) continue;
            const auto inherited = refs_in(ta[5]);
            for (const auto element : refs_in(a[4])) if (wanted.count(element)) {
                auto& sets = sets_of[element];
                sets.insert(sets.end(), inherited.begin(), inherited.end());
            }
        }
    }
    std::unordered_map<uint32_t, std::vector<uint32_t>> products_of_type;
    for (uint32_t id = 0; id < input.max_id(); ++id) {
        if (input.type(id) != "IFCRELDEFINESBYTYPE") continue;
        const auto a = attributes_of(input.line(id)); if (a.size() != 6) continue;
        for (const auto type : refs_in(a[5])) for (const auto product : refs_in(a[4]))
            if (wanted.count(product)) products_of_type[type].push_back(product);
    }
    std::unordered_map<uint32_t, std::array<std::string, 12>> associations;
    const std::array<std::string, 6> prefixes = {"CO_", "EN_", "SL_", "EF_", "SS_", "PR_"};
    for (uint32_t id = 0; id < input.max_id(); ++id) {
        if (input.type(id) != "IFCRELASSOCIATESCLASSIFICATION") continue;
        const auto a = attributes_of(input.line(id)); if (a.size() != 6) continue;
        for (const auto reference : refs_in(a[5])) {
            if (input.type(reference) != "IFCCLASSIFICATIONREFERENCE") continue;
            const auto r = attributes_of(input.line(reference)); if (r.size() < 3) continue;
            const std::string code = quoted_in(r[1]);
            std::string upper = code;
            for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (uniclass_title(code).empty()) continue;
            for (size_t t = 0; t < prefixes.size(); ++t) if (upper.rfind(prefixes[t], 0) == 0) {
                for (const auto target : refs_in(a[4])) {
                    std::vector<uint32_t> products;
                    if (wanted.count(target)) products.push_back(target);
                    auto inherited = products_of_type.find(target);
                    if (inherited != products_of_type.end()) products.insert(products.end(), inherited->second.begin(), inherited->second.end());
                    for (const auto product : products) {
                        associations[product][t * 2] = code;
                        associations[product][t * 2 + 1] = quoted_in(r[2]);
                    }
                }
            }
        }
    }
    std::unordered_map<uint32_t, Found> in_set;
    const auto read_set = [&](uint32_t id) -> const Found& {
        const auto seen = in_set.find(id);
        if (seen != in_set.end()) return seen->second;
        Found& found = in_set[id];
        if (input.type(id) != "IFCPROPERTYSET") return found;
        const auto attributes = attributes_of(input.line(id));
        if (attributes.size() != 5) return found;
        const std::string name = quoted_in(attributes[2]);
        for (const uint32_t property : refs_in(attributes[4])) {
            if (input.type(property) != "IFCPROPERTYSINGLEVALUE") continue;
            const auto parts = attributes_of(input.line(property));
            if (parts.size() < 3) continue;
            const std::string property_name = quoted_in(parts[0]);
            const std::string value = quoted_in(parts[2]);
            std::string lower;
            for (const unsigned char c : property_name) if (std::isalnum(c)) lower += static_cast<char>(std::tolower(c));
            const std::array<const char*, 6> tables = {"co", "en", "sl", "ef", "ss", "pr"};
            if (!NOT_A_SITE.count(value)) for (size_t t = 0; t < tables.size(); ++t) {
                const std::string prefix = std::string("uniclass") + tables[t];
                if (lower == prefix + "code" || lower == prefix + "value") found.classification[t * 2] = value;
                if (lower == prefix + "title" || lower == prefix + "description" || (t == 2 && lower == "uniclassslitle")) found.classification[t * 2 + 1] = value;
            }
            if (name == TRACE_SET && property_name == "Identifier") found.identifier = value;
            if (name == TRACE_SET && property_name == "Description") found.description = value;
            if (property_name == s.zone_property && s.zones.count(value)) found.zone = value;
            if (s.site_sets.count(name) && property_name == s.site_property && is_one_site(value)) found.site = value;
            if (property_name == s.location_property && !NOT_A_SITE.count(value)) found.location = value;
            if (s.design_sets.count(name) && property_name == s.design_property && !NOT_A_SITE.count(value)) found.design = value;
            if (name == s.project_set && !s.code_property.empty() && property_name == s.code_property) found.package_code = value;
            if (name == s.project_set && property_name == s.long_name_property) found.package_name = value;
        }
        return found;
    };
    const auto found_on = [&](uint32_t element) {
        Found found;
        const auto sets = sets_of.find(element);
        if (sets != sets_of.end()) {
            for (const uint32_t set : sets->second) {
                const Found& in = read_set(set);
                if (found.zone.empty()) found.zone = in.zone;
                if (found.site.empty()) found.site = in.site;
                if (found.location.empty()) found.location = in.location;
                if (found.design.empty()) found.design = in.design;
                if (found.identifier.empty()) found.identifier = in.identifier;
                if (found.description.empty()) found.description = in.description;
                if (found.package_code.empty()) found.package_code = in.package_code;
                if (found.package_name.empty()) found.package_name = in.package_name;
                for (size_t i = 0; i < found.classification.size(); ++i) if (found.classification[i].empty()) found.classification[i] = in.classification[i];
            }
        }
        const auto associated = associations.find(element);
        if (associated != associations.end()) for (size_t i = 0; i < found.classification.size(); ++i)
            if (found.classification[i].empty()) found.classification[i] = associated->second[i];
        if (found.zone.empty()) found.zone = model_zone;
        return found;
    };

    // Select the most common complete project set among contributing source elements.
    // IDs differ between otherwise identical sets, so compare their property definitions.
    std::map<std::string, std::pair<size_t, uint32_t>> project_sets;
    for (const uint32_t element : wanted) {
        const auto at = sets_of.find(element);
        if (at == sets_of.end()) continue;
        for (const uint32_t id : at->second) {
            if (input.type(id) != "IFCPROPERTYSET") continue;
            const auto a = attributes_of(input.line(id));
            if (a.size() != 5 || quoted_in(a[2]) != s.project_set) continue;
            std::vector<std::string> definitions;
            for (const uint32_t p : refs_in(a[4])) {
                const auto line = input.line(p);
                const auto equals = line.find('=');
                if (equals != std::string_view::npos) definitions.emplace_back(line.substr(equals + 1));
            }
            std::sort(definitions.begin(), definitions.end());
            std::string key;
            for (const auto& definition : definitions) key += definition + '\n';
            auto& candidate = project_sets[key];
            ++candidate.first;
            candidate.second = id;
        }
    }
    uint32_t primary_set = 0;
    size_t primary_count = 0;
    for (const auto& [key, candidate] : project_sets) {
        if (candidate.first > primary_count) primary_count = candidate.first, primary_set = candidate.second;
    }
    stats.project_missing = !elements.empty() && !primary_set;

    // One row per source element preserves the association between file, GUID and project.
    {
        const std::string manifest_path = path.substr(0, path.find_last_of('.')) + ".manifest.csv";
        std::ofstream manifest(manifest_path, std::ios::binary);
        if (!manifest) throw std::runtime_error(manifest_path + ": can't be written");
        manifest << "output_guid,source_file,source_guid,project_code,project_name\r\n";
        for (const Traced& traced : elements) {
            for (const uint32_t source : traced.sources) {
                const Found on = found_on(source);
                manifest << csv(traced.guid) << ',' << csv(file_name) << ',' << csv(std::string(guid_of(input.line(source)))) << ','
                         << csv(on.package_code) << ',' << csv(on.package_name) << "\r\n";
            }
        }
        if (!manifest) throw std::runtime_error(manifest_path + ": couldn't finish writing");
    }

    const std::string temporary = path + ".trace";
    {
        const SpfFile spf(path);
        std::unordered_map<std::string, const Traced*> by_guid;
        for (const Traced& traced : elements) by_guid.emplace(traced.guid, &traced);
        std::string owner = "$";
        std::vector<std::pair<uint32_t, const Traced*>> found;
        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            const auto line = spf.line(id);
            if (line.empty()) continue;
            if (owner == "$" && spf.type(id) == "IFCOWNERHISTORY") owner = "#" + std::to_string(id);
            const auto guid = guid_of(line);
            if (guid.empty()) continue;
            const auto at = by_guid.find(std::string(guid));
            if (at == by_guid.end()) continue;
            found.emplace_back(id, at->second);
            by_guid.erase(at);
        }
        stats.missing = by_guid.size();

        uint32_t next = spf.max_id();
        std::vector<std::string> added;
        // Copy the primary project's property graph into the output, including units if present.
        std::unordered_map<uint32_t, uint32_t> copied;
        const std::function<uint32_t(uint32_t)> copy = [&](uint32_t id) -> uint32_t {
            const auto known = copied.find(id);
            if (known != copied.end()) return known->second;
            const uint32_t dest = next++;
            copied[id] = dest;
            const auto line = input.line(id);
            const auto equals = line.find('=');
            if (equals == std::string_view::npos) throw std::runtime_error("project property references a missing instance");
            std::string text;
            const auto body = line.substr(equals + 1);
            bool quoted = false;
            for (size_t i = 0; i < body.size(); ++i) {
                const char c = body[i];
                if (c == '\'') {
                    if (quoted && i + 1 < body.size() && body[i + 1] == '\'') { text += "''"; ++i; continue; }
                    quoted = !quoted;
                }
                if (!quoted && c == '#' && i + 1 < body.size() && std::isdigit(static_cast<unsigned char>(body[i + 1]))) {
                    uint32_t ref = 0;
                    while (i + 1 < body.size() && std::isdigit(static_cast<unsigned char>(body[i + 1]))) ref = ref * 10 + static_cast<uint32_t>(body[++i] - '0');
                    text += "#" + std::to_string(copy(ref));
                } else text += c;
            }
            added.push_back("#" + std::to_string(dest) + "=" + text);
            return dest;
        };
        uint32_t project_copy = 0;
        if (primary_set) {
            const auto a = attributes_of(input.line(primary_set));
            std::vector<uint32_t> properties;
            for (const uint32_t property : refs_in(a[4])) properties.push_back(copy(property));
            if (!properties.empty()) {
                project_copy = next++;
                std::string list;
                for (const auto id : properties) list += (list.empty() ? "" : ",") + std::string("#") + std::to_string(id);
                added.push_back("#" + std::to_string(project_copy) + "=IFCPROPERTYSET('" + derived_guid(file_name + "/primary project") + "'," + owner + ",'" + escaped(s.project_set) + "',$,(" + list + ")); ");
            }
        }
        std::unordered_map<std::string, uint32_t> values;  // a property as it's written, to its id
        const auto property = [&](const char* name, const std::string& value) -> std::string {
            const char* type = value.size() <= 255 ? "IFCLABEL" : "IFCTEXT";
            const std::string text = std::string("IFCPROPERTYSINGLEVALUE('") + name + "',$," + type + "('" + value + "'),$);";
            const auto at = values.find(text);
            if (at != values.end()) return "#" + std::to_string(at->second);
            values.emplace(text, next);
            added.push_back("#" + std::to_string(next) + "=" + text);
            return "#" + std::to_string(next++);
        };
        const auto name_key = [](std::string_view text) {
            std::string key;
            for (unsigned char c : text) if (std::isalnum(c)) key += static_cast<char>(std::tolower(c));
            return key;
        };
        // Bring through only workbook-listed source properties. If contributors disagree, leave the field blank.
        const auto source_properties = [&](const Traced& traced, const std::string& set_name, const SiteCorrection* correction) {
            struct Candidate { uint32_t id = 0; std::string definition; bool conflict = false; };
            std::map<std::string, Candidate> candidates;
            const auto allowed = property_schema.find(set_name);
            if (allowed == property_schema.end()) return std::vector<uint32_t>{};
            std::unordered_set<std::string> wanted_names;
            for (const auto& name : allowed->second) wanted_names.insert(name_key(name));
            for (const auto source : traced.sources) {
                const auto source_sets = sets_of.find(source);
                if (source_sets == sets_of.end()) continue;
                for (const auto set : source_sets->second) {
                    if (input.type(set) != "IFCPROPERTYSET") continue;
                    const auto a = attributes_of(input.line(set));
                    if (a.size() != 5 || name_key(quoted_in(a[2])) != name_key(set_name)) continue;
                    for (const auto id : refs_in(a[4])) {
                        const auto line = input.line(id);
                        if (input.type(id) != "IFCPROPERTYSINGLEVALUE") continue;
                        const auto pa = attributes_of(line);
                        if (pa.size() < 3 || pa[2] == "$" || pa[2] == "*") continue;
                        const auto key = name_key(quoted_in(pa[0]));
                        if (!wanted_names.count(key)) continue;
                        const auto equals = line.find('=');
                        if (equals == std::string_view::npos) continue;
                        auto& candidate = candidates[key];
                        const std::string definition(line.substr(equals + 1));
                        if (!candidate.id) candidate.id = id, candidate.definition = definition;
                        else if (candidate.definition != definition) candidate.conflict = true;
                    }
                }
            }
            std::vector<uint32_t> result;
            const auto setkey = name_key(set_name);
            for (const auto& [key, candidate] : candidates) {
                if (candidate.conflict) continue;
                if (setkey == "ifcfilter" && correction) {
                    bool overridden = false;
                    for (const auto& [column, value] : correction->extra)
                        if (!value.empty() && name_key(column) == key) { overridden = true; break; }
                    if (overridden) continue;
                }
                if (setkey == "ifcfilter" && (key == "siteid" || key == "assettype" || key == "designid" || key == "designtag" || key == "description" || key == "identifier" || key == "zone" || key == "location" || key == "locationdescription" || key == "sourcefile")) continue;
                if (setkey == "classification" && (key == "cocode" || key == "codescription" || key == "encode" || key == "endescription" || key == "slcode" || key == "sldescription" || key == "efcode" || key == "efdescription" || key == "sscode" || key == "ssdescription" || key == "prcode" || key == "prdescription")) continue;
                // With a hierarchy-set, every Hierarchy field is resolved from it below
                if (setkey == "hierarchy" && (key == "location" || !schema().hierarchy_set.empty())) continue;
                result.push_back(copy(candidate.id));
            }
            return result;
        };

        const std::string source_file = escaped(file_name);
        std::set<std::string> unknown_classifications;
        for (const auto& [id, traced] : found) {
            std::vector<std::string> zones, sites, types, locations, designs, identifiers, descriptions;
            for (const uint32_t source : traced->sources) {
                const Found on = found_on(source);
                const auto corrected = element_corrections.find(source);
                const std::string site = corrected != element_corrections.end() && !corrected->second.site.empty() ? escaped(corrected->second.site) : on.site;
                zones.push_back(on.zone);
                sites.push_back(site);
                types.push_back(corrected != element_corrections.end() && !corrected->second.type.empty() ? escaped(corrected->second.type) : (site.empty() ? "" : asset_type_of(site)));
                locations.push_back(on.location);
                designs.push_back(on.design);
                identifiers.push_back(on.identifier);
                descriptions.push_back(on.description);
            }
            const auto correction_at = corrections.find(joined(sites));
            const SiteCorrection* correction = correction_at == corrections.end() ? nullptr : &correction_at->second;
            if (correction) {
                if (!correction->zone.empty()) zones = {escaped(correction->zone)};
                if (!correction->type.empty()) types = {escaped(correction->type)};
                if (!correction->location.empty()) locations = {escaped(correction->location)};
                if (!correction->design.empty()) designs = {escaped(correction->design)};
                if (!correction->identifier.empty()) identifiers = {escaped(correction->identifier)};
                if (!correction->description.empty()) descriptions = {escaped(correction->description)};
            }
            // An element's own location correction comes before the site lookup's and the model's
            if (std::any_of(traced->sources.begin(), traced->sources.end(), [&](uint32_t source) {
                    const auto at = element_corrections.find(source);
                    return at != element_corrections.end() && !at->second.location.empty();
                })) {
                const std::vector<std::string> before = locations;
                locations.clear();
                for (size_t i = 0; i < traced->sources.size(); ++i) {
                    const auto at = element_corrections.find(traced->sources[i]);
                    if (at != element_corrections.end() && !at->second.location.empty()) locations.push_back(escaped(at->second.location));
                    else locations.push_back(before.size() == traced->sources.size() ? before[i] : before.empty() ? std::string() : before.front());
                }
            }
            const auto location_at = location_reference.find(joined(locations));
            const std::string location_description = location_at == location_reference.end() ? "" : escaped(location_at->second.description);
            std::string list = property("SourceFile", source_file);
            const std::pair<const char*, const std::vector<std::string>*> more[] = {{"Zone", &zones}, {"SiteID", &sites}, {"Asset Type", &types}, {"Location", &locations}, {"DesignTag", &designs}, {"Identifier", &identifiers}, {"Description", &descriptions}};
            for (const auto& [name, of] : more) {
                const std::string value = joined(*of);
                list += "," + property(name, value);
            }
            for (const auto& [set_name, fields] : property_schema) if (name_key(set_name) == "ifcfilter") {
                for (const auto& field : fields) if (name_key(field) == "locationdescription") list += "," + property(field.c_str(), location_description);
                // The model the element came from, as its input file is named, which a combined file keeps for each
                for (const auto& field : fields) if (name_key(field) == "modelfile") list += "," + property(field.c_str(), source_file);
                if (correction) for (const auto& field : fields) {
                    const auto key = name_key(field);
                    if (key == "siteid" || key == "assettype" || key == "designid" || key == "designtag" || key == "description" || key == "identifier" || key == "zone" || key == "location" || key == "locationdescription" || key == "sourcefile" || key == "modelfile") continue;
                    for (const auto& [column, value] : correction->extra) if (!value.empty() && name_key(column) == key) {
                        list += "," + property(field.c_str(), escaped(value));
                        break;
                    }
                }
                for (const auto extra : source_properties(*traced, set_name, correction)) list += ",#" + std::to_string(extra);
            }
            // Viewers commonly show sets in relationship order, so put project information first.
            if (project_copy) added.push_back("#" + std::to_string(next++) + "=IFCRELDEFINESBYPROPERTIES('" + derived_guid(traced->guid + "/primary project") + "'," + owner + ",$,$,(#" + std::to_string(id) + "),#" + std::to_string(project_copy) + ");");
            const uint32_t set = next++;
            added.push_back("#" + std::to_string(set) + "=IFCPROPERTYSET('" + derived_guid(traced->guid + "/" + TRACE_SET) + "'," + owner + ",'" + TRACE_SET + "',$,(" + list + "));");
            added.push_back("#" + std::to_string(next++) + "=IFCRELDEFINESBYPROPERTIES('" + derived_guid(traced->guid + "/" + TRACE_SET + "/defines") + "'," + owner + ",$,$,(#" +
                            std::to_string(id) + "),#" + std::to_string(set) + ");");
            const std::array<const char*, 12> labels = {"Co Value", "Co Description", "En Value", "En Description", "SL Value", "SL Description", "EF Value", "EF Description", "Ss Value", "Ss Description", "Pr Value", "Pr Description"};
            std::array<std::vector<std::string>, 12> classifications;
            for (const uint32_t source : traced->sources) {
                auto source_values = found_on(source).classification;
                for (size_t i = 0; i < source_values.size(); i += 2) {
                    if (!source_values[i].empty()) {
                        const auto title = uniclass_title(source_values[i]);
                        if (title.empty()) unknown_classifications.insert(source_values[i]);
                        else if (source_values[i + 1].empty()) source_values[i + 1] = escaped(title);
                    }
                }
                for (size_t i = 0; i < source_values.size(); ++i) classifications[i].push_back(source_values[i]);
            }
            if (correction) for (size_t table = 0; table < correction->codes.size(); ++table) {
                const auto& code = correction->codes[table];
                if (code.empty()) continue;
                const auto title = uniclass_title(code);
                if (title.empty()) unknown_classifications.insert(code);
                classifications[table * 2] = {escaped(code)};
                classifications[table * 2 + 1] = {escaped(title)};
            }
            if (location_at != location_reference.end()) for (size_t i = 0; i < location_at->second.classification.size(); i += 2) {
                const auto& code = location_at->second.classification[i];
                if (code.empty()) continue;
                classifications[i] = {escaped(code)};
                const auto& title = location_at->second.classification[i + 1];
                classifications[i + 1] = {escaped(title.empty() ? uniclass_title(code) : title)};
            }
            std::string classification_list;
            for (size_t i = 0; i < labels.size(); ++i) classification_list += (classification_list.empty() ? "" : ",") + property(labels[i], joined(classifications[i]));
            for (const auto& [set_name, _] : property_schema) if (name_key(set_name) == "classification")
                for (const auto extra : source_properties(*traced, set_name, correction)) classification_list += ",#" + std::to_string(extra);
            const uint32_t classification_set = next++;
            added.push_back("#" + std::to_string(classification_set) + "=IFCPROPERTYSET('" + derived_guid(traced->guid + "/Classification") + "'," + owner + ",'Classification',$,(" + classification_list + "));" );
            added.push_back("#" + std::to_string(next++) + "=IFCRELDEFINESBYPROPERTIES('" + derived_guid(traced->guid + "/Classification/defines") + "'," + owner + ",$,$,(#" + std::to_string(id) + "),#" + std::to_string(classification_set) + ");");
            for (const auto& [set_name, _] : property_schema) {
                const auto key = name_key(set_name);
                if (key == "ifcfilter" || key == "classification" || key == name_key(s.project_set)) continue;
                const auto extra = source_properties(*traced, set_name, correction);
                std::string properties;
                for (const auto property_id : extra) properties += (properties.empty() ? "" : ",") + std::string("#") + std::to_string(property_id);
                if (key == "hierarchy") {
                    // Location is the element's own; every other field comes from the rules' hierarchy-set, under its
                    // own name or the one hierarchy-source gives it. Conflicting values across merged source elements
                    // are left blank.
                    const Schema& rules = schema();
                    std::map<std::string, std::string> source_of;  // field key to source property key
                    if (!rules.hierarchy_set.empty())
                        for (const auto& field : property_schema.at(set_name)) {
                            const auto renamed = rules.hierarchy_sources.find(field);
                            if (name_key(field) != "location") source_of[name_key(field)] = name_key(renamed == rules.hierarchy_sources.end() ? field : renamed->second);
                        }
                    std::set<std::string> wanted;
                    for (const auto& [_, source_key] : source_of) wanted.insert(source_key);
                    std::map<std::string, std::set<std::string>> parent_values;
                    for (const uint32_t source : traced->sources) {
                        const auto source_sets = sets_of.find(source);
                        if (wanted.empty() || source_sets == sets_of.end()) continue;
                        for (const uint32_t source_set : source_sets->second) {
                            if (input.type(source_set) != "IFCPROPERTYSET") continue;
                            const auto a = attributes_of(input.line(source_set));
                            if (a.size() != 5 || name_key(quoted_in(a[2])) != name_key(rules.hierarchy_set)) continue;
                            for (const uint32_t source_property : refs_in(a[4])) {
                                if (input.type(source_property) != "IFCPROPERTYSINGLEVALUE") continue;
                                const auto pa = attributes_of(input.line(source_property));
                                if (pa.size() < 3) continue;
                                const auto source_key = name_key(quoted_in(pa[0]));
                                if (!wanted.count(source_key)) continue;
                                const auto candidate = quoted_in(pa[2]);
                                if (!candidate.empty()) parent_values[source_key].insert(candidate);
                            }
                        }
                    }
                    for (const auto& field : property_schema.at(set_name)) {
                        const auto field_key = name_key(field);
                        std::string source_key;
                        if (const auto at = source_of.find(field_key); at != source_of.end()) source_key = at->second;
                        else if (field_key != "location") continue;
                        std::string value;
                        if (field_key == "location") value = joined(locations);
                        else {
                            const auto found_values = parent_values.find(source_key);
                            if (found_values != parent_values.end() && found_values->second.size() == 1) value = *found_values->second.begin();
                        }
                        properties += (properties.empty() ? "" : ",") + property(field.c_str(), value);
                    }
                }
                if (properties.empty()) continue;
                const uint32_t extra_set = next++;
                added.push_back("#" + std::to_string(extra_set) + "=IFCPROPERTYSET('" + derived_guid(traced->guid + "/" + set_name) + "'," + owner + ",'" + escaped(set_name) + "',$,(" + properties + "));");
                added.push_back("#" + std::to_string(next++) + "=IFCRELDEFINESBYPROPERTIES('" + derived_guid(traced->guid + "/" + set_name + "/defines") + "'," + owner + ",$,$,(#" + std::to_string(id) + "),#" + std::to_string(extra_set) + ");");
            }
            ++stats.elements;
        }

        if (!unknown_classifications.empty()) {
            std::cerr << "  warning: " << unknown_classifications.size() << " Uniclass codes in " << file_name
                      << " were not found in the supplied active tables; source values were retained:";
            for (const auto& code : unknown_classifications) std::cerr << ' ' << code;
            std::cerr << "\n";
        }
        std::ofstream out(temporary, std::ios::binary);
        out << spf.header();
        std::unordered_set<uint32_t> products;
        for (const auto& [id, traced] : found) products.insert(id);
        std::unordered_set<uint32_t> product_types;
        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            if (spf.type(id) != "IFCRELDEFINESBYTYPE") continue;
            const auto a = attributes_of(spf.line(id));
            if (a.size() != 6) continue;
            const auto related = refs_in(a[4]);
            if (std::none_of(related.begin(), related.end(), [&](uint32_t product) { return products.count(product); })) continue;
            for (const auto type : refs_in(a[5])) product_types.insert(type);
        }
        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            const auto line = spf.line(id);
            if (line.empty()) continue;
            if (spf.type(id) == "IFCRELDEFINESBYPROPERTIES") {
                std::string rewritten;
                if (rewrite(line, [&](uint32_t ref) { return products.count(ref) != 0; }, WhenEmpty::Drop, rewritten)) out << rewritten << '\n';
                continue;
            }
            if (product_types.count(id)) {
                const auto a = attributes_of(line);
                if (a.size() > 5) {
                    out << line.substr(0, static_cast<size_t>(a[5].data() - line.data())) << '$'
                        << line.substr(static_cast<size_t>(a[5].data() - line.data()) + a[5].size()) << '\n';
                    continue;
                }
            }
            out << line << '\n';
        }
        for (const std::string& line : added) out << line << '\n';
        out << "ENDSEC;\nEND-ISO-10303-21;\n";
    }
    replace_file(temporary, path);
    return stats;
}

}  // namespace ifcfilter







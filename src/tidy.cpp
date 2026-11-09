#include "tidy.h"

#include "site.h"
#include "spf.h"
#include "system.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef IFCFILTER_VERSION
#define IFCFILTER_VERSION "dev"
#endif

namespace ifcfilter {
namespace {

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// What's between a value's outer quotes, as written, so escapes stay as they are; empty if it isn't a string
std::string raw_string(std::string_view value) {
    const auto open = value.find('\'');
    const auto close = value.rfind('\'');
    return open == std::string_view::npos || close <= open ? std::string() : std::string(value.substr(open + 1, close - open - 1));
}

// Text as a STEP string, quotes and all
std::string quoted(const std::string& text) {
    std::string out = "'";
    for (const char c : text) {
        out += c;
        if (c == '\'') out += '\'';
    }
    return out + "'";
}

std::string utc_text(std::time_t when) {
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &when);
#else
    gmtime_r(&when, &parts);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &parts);
    return buffer;
}

std::string with_attributes(std::string_view line, const std::vector<std::string>& attributes) {
    std::string text(line.substr(0, line.find('(') + 1));
    for (size_t i = 0; i < attributes.size(); ++i) text.append(i ? "," : "").append(attributes[i]);
    return text + ");";
}

// A header statement, from its keyword to the ; that ends it, outside strings
std::pair<size_t, size_t> statement(std::string_view header, std::string_view keyword) {
    const auto at = header.find(keyword);
    if (at == std::string_view::npos) return {std::string_view::npos, 0};
    bool in_string = false;
    for (size_t i = at; i < header.size(); ++i) {
        if (header[i] == '\'') in_string = !in_string;
        else if (header[i] == ';' && !in_string) return {at, i + 1 - at};
    }
    return {std::string_view::npos, 0};
}

std::string new_header(std::string_view header, const std::string& file_name, const std::string& source, const std::string& now) {
    std::string out(header);
    const auto [name_at, name_size] = statement(out, "FILE_NAME(");
    if (name_at != std::string::npos) {
        const auto attributes = attributes_of(std::string_view(out).substr(name_at, name_size));
        if (attributes.size() == 7) {
            const std::string before = raw_string(attributes[4]);
            const std::string preprocessor = std::string("IFCFilter ") + IFCFILTER_VERSION + (before.empty() ? "" : " (from " + before + ")");
            const std::string text = "FILE_NAME(" + quoted(file_name) + "," + quoted(now) + "," + std::string(trimmed(attributes[2])) + "," + std::string(trimmed(attributes[3])) + "," +
                                     quoted(preprocessor) + "," + std::string(trimmed(attributes[5])) + "," + std::string(trimmed(attributes[6])) + ");";
            out.replace(name_at, name_size, text);
        }
    }
    const auto [description_at, description_size] = statement(out, "FILE_DESCRIPTION(");
    if (description_at != std::string::npos) {
        const auto attributes = attributes_of(std::string_view(out).substr(description_at, description_size));
        const auto list = attributes.empty() ? std::string_view() : trimmed(attributes[0]);
        if (attributes.size() == 2 && list.size() >= 2 && list.front() == '(' && list.back() == ')') {
            const std::string note = quoted("IFCFilter [Source: " + source + ", " + now + "]");
            const std::string inner(list.substr(1, list.size() - 2));
            const std::string text = "FILE_DESCRIPTION((" + inner + (trimmed(inner).empty() ? "" : ",") + note + ")," + std::string(trimmed(attributes[1])) + ");";
            out.replace(description_at, description_size, text);
        }
    }
    return out;
}

}  // namespace

TidyStats tidy(const std::string& path, const std::string& source) {
    namespace fs = std::filesystem;
    TidyStats stats;
    const std::string temporary = path + ".tidy";
    const std::time_t when = std::time(nullptr);
    const std::string now = utc_text(when);
    {
        const SpfFile spf(path);
        const uint32_t max_id = spf.max_id();

        // Property sets by name, and what each element has
        std::unordered_map<uint32_t, std::string> set_names;
        std::vector<bool> culled(max_id, false);
        std::unordered_map<uint32_t, std::vector<uint32_t>> sets_of, objects_of;
        std::vector<uint32_t> contained, histories;
        std::unordered_map<uint32_t, std::vector<uint32_t>> parts_of;
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto type = spf.type(id);
            if (type == "IFCPROPERTYSET") {
                const auto attributes = attributes_of(spf.line(id));
                if (attributes.size() != 5) continue;
                set_names[id] = raw_string(attributes[2]);
                if (schema().culled_sets.count(set_names[id])) {
                    culled[id] = true;
                    ++stats.sets_dropped;
                }
            } else if (type == "IFCRELDEFINESBYPROPERTIES") {
                const auto attributes = attributes_of(spf.line(id));
                if (attributes.size() != 6) continue;
                const auto sets = refs_in(attributes[5]);
                if (sets.size() != 1) continue;
                for (const uint32_t object : refs_in(attributes[4])) {
                    sets_of[object].push_back(sets[0]);
                    objects_of[sets[0]].push_back(object);
                }
            } else if (type == "IFCRELCONTAINEDINSPATIALSTRUCTURE") {
                const auto attributes = attributes_of(spf.line(id));
                if (attributes.size() == 6) {
                    for (const uint32_t element : refs_in(attributes[4])) contained.push_back(element);
                }
            } else if (type == "IFCRELAGGREGATES") {
                const auto attributes = attributes_of(spf.line(id));
                const auto whole = attributes.size() == 6 ? refs_in(attributes[4]) : std::vector<uint32_t>{};
                if (whole.size() == 1) parts_of[whole[0]] = refs_in(attributes[5]);
            } else if (type == "IFCOWNERHISTORY") {
                histories.push_back(id);
            }
        }

        // A property's value as written, from the first of an element's sets of that name that has it
        const auto value_of = [&](uint32_t object, const std::set<std::string>& set_names_wanted, const std::string& property_name) {
            const auto sets = sets_of.find(object);
            if (sets == sets_of.end()) return std::string();
            for (const uint32_t set : sets->second) {
                const auto name = set_names.find(set);
                if (name == set_names.end() || !set_names_wanted.count(name->second)) continue;
                for (const uint32_t property : refs_in(attributes_of(spf.line(set))[4])) {
                    const auto parts = attributes_of(spf.line(property));
                    if (parts.size() > 2 && raw_string(parts[0]) == property_name) {
                        const std::string value = raw_string(parts[2]);
                        if (!value.empty()) return value;
                    }
                }
            }
            return std::string();
        };

        std::unordered_map<uint32_t, std::string> rewritten;
        const auto current = [&](uint32_t id) {
            const auto done = rewritten.find(id);
            return done != rewritten.end() ? std::string_view(done->second) : spf.line(id);
        };
        std::vector<uint32_t> maybe;  // properties to leave out if nothing else has them

        // What Data says that another set on everything it's on says too is left out of it, and Data goes if that's
        // all it says; what only it says stays. A culled set doesn't count, as it's going.
        const auto said_elsewhere = [&](uint32_t object, uint32_t except, const std::string& name, std::string_view value) {
            const auto sets = sets_of.find(object);
            if (sets == sets_of.end()) return false;
            for (const uint32_t set : sets->second) {
                const auto set_name = set_names.find(set);
                if (set == except || set_name == set_names.end() || set_name->second == schema().duplicate_set || culled[set]) continue;
                for (const uint32_t property : refs_in(attributes_of(spf.line(set))[4])) {
                    const auto parts = attributes_of(spf.line(property));
                    if (parts.size() > 2 && raw_string(parts[0]) == name && trimmed(parts[2]) == value) return true;
                }
            }
            return false;
        };
        for (const auto& [set, objects] : objects_of) {
            const auto name = set_names.find(set);
            if (name == set_names.end() || schema().duplicate_set.empty() || name->second != schema().duplicate_set || culled[set] || objects.empty()) continue;
            const auto attributes = attributes_of(spf.line(set));
            const auto properties = refs_in(attributes[4]);
            std::vector<uint32_t> kept;
            for (const uint32_t property : properties) {
                const auto parts = attributes_of(spf.line(property));
                const bool repeated = parts.size() > 2 && std::all_of(objects.begin(), objects.end(), [&](uint32_t object) {
                    return said_elsewhere(object, set, raw_string(parts[0]), trimmed(parts[2]));
                });
                if (repeated) {
                    maybe.push_back(property);
                    ++stats.repeats_dropped;
                } else {
                    kept.push_back(property);
                }
            }
            if (kept.size() == properties.size()) continue;
            if (kept.empty()) {
                culled[set] = true;
                ++stats.sets_dropped;
                continue;
            }
            std::vector<std::string> rewritten_attributes;
            for (const auto a : attributes) rewritten_attributes.emplace_back(a);
            std::string list = "(";
            for (size_t i = 0; i < kept.size(); ++i) list += (i ? ",#" : "#") + std::to_string(kept[i]);
            rewritten_attributes[4] = list + ")";
            rewritten[set] = with_attributes(spf.line(set), rewritten_attributes);
        }

        // What's left out: culled sets, the relationships that give them, and their properties if nothing else has them
        std::vector<bool> dropped(max_id, false);
        for (uint32_t id = 0; id < max_id; ++id) {
            if (!culled[id]) continue;
            dropped[id] = true;
            const auto attributes = attributes_of(spf.line(id));
            for (const uint32_t property : refs_in(attributes[4])) maybe.push_back(property);
        }
        for (uint32_t id = 0; id < max_id; ++id) {
            if (spf.type(id) != "IFCRELDEFINESBYPROPERTIES") continue;
            const auto attributes = attributes_of(spf.line(id));
            const auto sets = attributes.size() == 6 ? refs_in(attributes[5]) : std::vector<uint32_t>{};
            if (sets.size() == 1 && sets[0] < max_id && culled[sets[0]]) dropped[id] = true;
        }
        if (!maybe.empty()) {
            std::vector<bool> used(max_id, false);
            for (uint32_t id = 0; id < max_id; ++id) {
                if (dropped[id] || spf.line(id).empty()) continue;
                for_each_ref(current(id), [&](uint32_t next) {
                    if (next < max_id) used[next] = true;
                });
            }
            for (const uint32_t property : maybe) {
                if (property < max_id && !used[property]) dropped[property] = true;
            }
        }

        // Elements: what's on a level, and what those are made of
        std::unordered_set<uint32_t> elements;
        std::vector<uint32_t> stack(contained.begin(), contained.end());
        while (!stack.empty()) {
            const uint32_t id = stack.back();
            stack.pop_back();
            if (!elements.insert(id).second) continue;
            const auto parts = parts_of.find(id);
            if (parts != parts_of.end()) stack.insert(stack.end(), parts->second.begin(), parts->second.end());
        }

        for (const uint32_t id : elements) {
            if (id >= max_id) continue;
            const auto line = spf.line(id);
            std::vector<std::string> attributes;
            for (const auto a : attributes_of(line)) attributes.emplace_back(a);
            if (attributes.size() < 4) continue;
            const std::string site = value_of(id, schema().site_sets, schema().site_property);
            bool changed = false;
            if (is_one_site(site)) {
                // An asset's part is named after its site, and described as its component
                if (raw_string(attributes[2]) != site) {
                    attributes[2] = "'" + site + "'";
                    changed = true;
                    ++stats.named;
                }
                const std::string component = value_of(id, schema().site_sets, schema().component_property);
                if (!NOT_A_SITE.count(component) && raw_string(attributes[3]) != component) {
                    attributes[3] = "'" + component + "'";
                    changed = true;
                    ++stats.described;
                }
            } else {
                // Anything else is its family and type, without the Revit element ID Revit adds; that's its tag
                const std::string name = raw_string(attributes[2]);
                const auto colon = name.rfind(':');
                if (colon != std::string::npos && colon + 1 < name.size() &&
                    std::all_of(name.begin() + colon + 1, name.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    attributes[2] = "'" + name.substr(0, colon) + "'";
                    changed = true;
                    ++stats.named;
                }
            }
            if (changed) rewritten[id] = with_attributes(line, attributes);
        }

        // An asset type is what its site ID starts with. Where a set says otherwise, it's given a property that says
        // the right one, as the old one may be on other sets; a set on elements of different types stays as it is.
        std::map<std::string, uint32_t> corrected_property;  // the corrected property's text, and its new id
        std::vector<std::string> added;
        uint32_t next_id = max_id + 2;  // after the organization and application below
        for (const auto& [set, objects] : objects_of) {
            const auto name = set_names.find(set);
            if (name == set_names.end() || !schema().site_sets.count(name->second) || culled[set]) continue;
            const std::string set_line(current(set));
            const auto set_attributes = attributes_of(set_line);
            uint32_t wrong = 0;
            std::string written;
            for (const uint32_t property : refs_in(set_attributes[4])) {
                const auto parts = attributes_of(spf.line(property));
                if (parts.size() > 2 && raw_string(parts[0]) == schema().type_property) {
                    wrong = property;
                    written = raw_string(parts[2]);
                }
            }
            if (!wrong) continue;
            std::set<std::string> types;
            for (const uint32_t object : objects) {
                const std::string site = value_of(object, schema().site_sets, schema().site_property);
                types.insert(is_one_site(site) ? asset_type_of(site) : std::string());
            }
            if (types.size() != 1 || types.begin()->empty() || *types.begin() == written) {
                if (types.size() > 1) ++stats.types_mixed;
                continue;
            }
            // The same property with the right value in its string
            const auto line = spf.line(wrong);
            std::vector<std::string> parts;
            for (const auto a : attributes_of(line)) parts.emplace_back(a);
            const auto open = parts[2].find('\'');
            const auto close = parts[2].rfind('\'');
            if (open == std::string::npos || close <= open) continue;
            parts[2] = parts[2].substr(0, open + 1) + *types.begin() + parts[2].substr(close);
            std::string text = with_attributes(line.substr(line.find('=') + 1), parts);
            auto [at, fresh] = corrected_property.emplace(text, next_id);
            if (fresh) {
                added.push_back("#" + std::to_string(next_id) + "=" + text);
                ++next_id;
            }
            std::vector<std::string> attributes;
            for (const auto a : set_attributes) attributes.emplace_back(a);
            std::vector<uint32_t> properties = refs_in(set_attributes[4]);
            std::replace(properties.begin(), properties.end(), wrong, at->second);
            std::string list = "(";
            for (size_t i = 0; i < properties.size(); ++i) list += (i ? ",#" : "#") + std::to_string(properties[i]);
            attributes[4] = list + ")";
            rewritten[set] = with_attributes(set_line, attributes);
            stats.types_corrected += objects.size();
        }

        // The project is named after the model and its package, the site after the project, buildings after the zone; levels have no Revit level type
        std::map<std::string, size_t> packages, zones;
        for (const auto& [object, sets] : sets_of) {
            const std::string package = value_of(object, {schema().project_set}, schema().long_name_property);
            if (!package.empty()) ++packages[package];
            const std::string zone = value_of(object, {schema().project_set}, schema().building_property);
            if (!zone.empty()) ++zones[zone];
        }
        const auto most = [](const std::map<std::string, size_t>& counts) {
            std::string best;
            size_t n = 0;
            for (const auto& [value, count] : counts) {
                if (count > n) best = value, n = count;
            }
            return best;
        };
        const std::string package = most(packages), zone = most(zones);
        const std::string model = fs::path(source).stem().string();
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto type = spf.type(id);
            if (type != "IFCPROJECT" && type != "IFCSITE" && type != "IFCBUILDING" && type != "IFCBUILDINGSTOREY") continue;
            const auto line = spf.line(id);
            std::vector<std::string> attributes;
            for (const auto a : attributes_of(line)) attributes.emplace_back(a);
            if (type == "IFCPROJECT" && attributes.size() == 9) {
                attributes[2] = quoted(model);
                if (!package.empty()) attributes[5] = "'" + package + "'";
            } else if (type == "IFCSITE" && attributes.size() >= 3 && !schema().site_name.empty()) {
                const std::string& site_name = schema().site_name;
                attributes[2] = quoted(site_name);
            } else if (type == "IFCBUILDING" && attributes.size() >= 8 && !zone.empty()) {
                attributes[2] = "'" + zone + "'";
            } else if (type == "IFCBUILDINGSTOREY" && attributes.size() >= 5) {
                attributes[4] = "$";
            } else {
                continue;
            }
            rewritten[id] = with_attributes(line, attributes);
        }

        // Owner histories say IFCFilter changed what they're on, now
        const uint32_t organization = max_id, application = max_id + 1;
        for (const uint32_t id : histories) {
            const auto line = spf.line(id);
            std::vector<std::string> attributes;
            for (const auto a : attributes_of(line)) attributes.emplace_back(a);
            if (attributes.size() != 8) continue;
            attributes[3] = ".MODIFIED.";
            attributes[4] = std::to_string(static_cast<long long>(when));
            attributes[6] = "#" + std::to_string(application);
            rewritten[id] = with_attributes(line, attributes);
            ++stats.histories;
        }

        // Sets in alphabetical order, as a viewer lists them in the order of the relationships that give them:
        // those are written last, numbered in that order, as nothing refers to them
        std::vector<std::pair<std::string, uint32_t>> relationships;
        for (uint32_t id = 0; id < max_id; ++id) {
            if (dropped[id] || spf.type(id) != "IFCRELDEFINESBYPROPERTIES") continue;
            const auto attributes = attributes_of(spf.line(id));
            const auto sets = attributes.size() == 6 ? refs_in(attributes[5]) : std::vector<uint32_t>{};
            const auto set_attributes = sets.size() == 1 ? attributes_of(spf.line(sets[0])) : std::vector<std::string_view>{};
            std::string name = set_attributes.size() > 2 ? raw_string(set_attributes[2]) : std::string();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            relationships.emplace_back(std::move(name), id);
        }
        std::sort(relationships.begin(), relationships.end());
        std::vector<bool> last(max_id, false);
        for (const auto& [name, id] : relationships) last[id] = true;

        std::ofstream out(temporary, std::ios::binary);
        out << new_header(spf.header(), fs::path(path).filename().string(), fs::path(source).filename().string(), now);
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto line = spf.line(id);
            if (line.empty() || dropped[id] || last[id]) continue;
            const auto done = rewritten.find(id);
            out << (done != rewritten.end() ? std::string_view(done->second) : line) << '\n';
        }
        if (stats.histories) {
            out << "#" << organization << "=IFCORGANIZATION($,'IFCFilter',$,$,$);\n";
            out << "#" << application << "=IFCAPPLICATION(#" << organization << "," << quoted(IFCFILTER_VERSION) << ",'IFCFilter','IFCFilter');\n";
        }
        for (const std::string& line : added) out << line << '\n';
        for (const auto& [name, id] : relationships) {
            const auto line = current(id);
            out << '#' << next_id++ << line.substr(line.find('=')) << '\n';
        }
        out << "ENDSEC;\nEND-ISO-10303-21;\n";
    }
    replace_file(temporary, path);
    return stats;
}

}  // namespace ifcfilter

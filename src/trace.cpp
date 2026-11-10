#include "trace.h"

#include "site.h"
#include "spf.h"
#include "system.h"

#include <fstream>
#include <set>
#include <sstream>
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
    std::string zone, site, location;
};

}  // namespace

TraceStats trace(const std::string& path, const SpfFile& input, const std::string& file_name, const std::vector<Traced>& elements) {
    TraceStats stats;
    if (elements.empty()) return stats;
    const Schema& s = schema();

    // The model's own zone, from its file name, if it's one: a model of a group of zones, such as PWA, has none
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
            if (property_name == s.zone_property && s.zones.count(value)) found.zone = value;
            if (s.site_sets.count(name) && property_name == s.site_property && !NOT_A_SITE.count(value)) found.site = value;
            if (property_name == s.location_property && !NOT_A_SITE.count(value)) found.location = value;
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
            }
        }
        if (found.zone.empty()) found.zone = model_zone;
        return found;
    };

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
        const std::string source_file = escaped(file_name);
        for (const auto& [id, traced] : found) {
            std::vector<std::string> guids, zones, sites, types, locations;
            for (const uint32_t source : traced->sources) {
                guids.emplace_back(guid_of(input.line(source)));
                const Found on = found_on(source);
                zones.push_back(on.zone);
                sites.push_back(on.site);
                types.push_back(on.site.empty() ? "" : asset_type_of(on.site));
                locations.push_back(on.location);
            }
            std::string list = property("SourceFile", source_file);
            const std::string all_guids = joined(guids);
            if (!all_guids.empty()) list += "," + property("SourceGUIDs", all_guids);
            const std::pair<const char*, const std::vector<std::string>*> more[] = {{"Zone", &zones}, {"SiteID", &sites}, {"Asset Type", &types}, {"Location", &locations}};
            for (const auto& [name, of] : more) {
                const std::string value = joined(*of);
                if (!value.empty()) list += "," + property(name, value);
            }
            const uint32_t set = next++;
            added.push_back("#" + std::to_string(set) + "=IFCPROPERTYSET('" + derived_guid(traced->guid + "/" + TRACE_SET) + "'," + owner + ",'" + TRACE_SET + "',$,(" + list + "));");
            added.push_back("#" + std::to_string(next++) + "=IFCRELDEFINESBYPROPERTIES('" + derived_guid(traced->guid + "/" + TRACE_SET + "/defines") + "'," + owner + ",$,$,(#" +
                            std::to_string(id) + "),#" + std::to_string(set) + ");");
            ++stats.elements;
        }

        std::ofstream out(temporary, std::ios::binary);
        out << spf.header();
        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            const auto line = spf.line(id);
            if (!line.empty()) out << line << '\n';
        }
        for (const std::string& line : added) out << line << '\n';
        out << "ENDSEC;\nEND-ISO-10303-21;\n";
    }
    replace_file(temporary, path);
    return stats;
}

}  // namespace ifcfilter

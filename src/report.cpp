#include "report.h"

#include "ifc_access.h"

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bimpass {
namespace {

const std::set<std::string> SITE_PSETS = {"Asset", "Data"};  // the FD models keep everything in Data
const std::string SITE_PROPERTY = "SiteID";
const std::set<std::string> NOT_A_SITE = {"", "N/A", "PLACEHOLDER", "Placeholder", "placeholder"};

double megabytes(uint64_t bytes) { return bytes / 1048576.0; }

// Bytes of each instance as written in the file, and the total per entity type. Written to a
// flat array by id: the parser doesn't keep the text, and the array costs little.
struct FileBytes {
    std::vector<uint32_t> by_id;
    std::map<std::string, uint64_t> by_type;
    uint64_t total = 0;

    uint64_t of(uint32_t id) const { return id < by_id.size() ? by_id[id] : 0; }

    explicit FileBytes(const std::string& path) {
        std::ifstream in(path);
        std::string line;
        uint32_t current = 0;
        std::string type;
        while (std::getline(in, line)) {
            const uint64_t size = line.size() + 1;
            total += size;
            if (line.size() > 1 && line[0] == '#') {
                const auto equals = line.find('=');
                const auto paren = line.find('(');
                if (equals == std::string::npos || paren == std::string::npos) continue;
                current = static_cast<uint32_t>(std::strtoul(line.c_str() + 1, nullptr, 10));
                type = line.substr(equals + 1, paren - equals - 1);
                type.erase(std::remove(type.begin(), type.end(), ' '), type.end());
                if (current >= by_id.size()) by_id.resize(current + 1024, 0);
            } else if (!current) {
                continue;  // header
            }
            by_id[current] += static_cast<uint32_t>(size);
            by_type[type] += size;
        }
    }
};

struct Item {
    uint64_t own_bytes = 0;  // of this definition, shared or not
    uint32_t faces = 0;
};

struct Totals {
    uint64_t elements = 0, items = 0, definitions = 0, faces = 0, bytes = 0;
    uint64_t transparent_items = 0, mixed_elements = 0, transparent_elements = 0, heavy = 0;
    uint64_t max_faces = 0;
};

class Walker {
  public:
    Walker(const FileBytes& bytes, const std::unordered_map<uint32_t, double>& transparency, double threshold)
        : bytes_(bytes), transparency_(transparency), threshold_(threshold) {}

    struct Result {
        uint64_t items = 0, faces = 0, bytes = 0, shown_bytes = 0, definitions = 0, transparent = 0, opaque = 0;
    };

    // bytes and definitions count each geometry definition once across the model, so
    // shared geometry isn't counted twice; items and faces count what the element shows.
    void walk_item(const express::entity& item, bool see_through, Result& out) {
        if (!item) return;
        see_through = see_through || is_transparent(item.id());
        if (item.declaration().is("IfcMappedItem")) {
            const auto source = link(item, "MappingSource");
            walk_representation(link(source, "MappedRepresentation"), see_through, out);
            return;
        }
        const auto& info = describe(item);
        ++out.items;
        out.faces += info.faces;
        out.shown_bytes += info.own_bytes;
        (see_through ? out.transparent : out.opaque) += 1;
        if (seen_.insert(item.id()).second) {
            out.bytes += info.own_bytes;
            ++out.definitions;
        }
    }

    void walk_representation(const express::entity& representation, bool see_through, Result& out) {
        if (!representation) return;
        out.bytes += seen_.insert(representation.id()).second ? bytes_.of(representation.id()) : 0;
        for (const auto& item : links(representation, "Items")) {
            walk_item(item.as<express::entity>(), see_through, out);
        }
    }

  private:
    bool is_transparent(uint32_t id) const {
        const auto it = transparency_.find(id);
        return it != transparency_.end() && it->second >= threshold_;
    }

    const Item& describe(const express::entity& item) {
        const auto found = items_.find(item.id());
        if (found != items_.end()) return found->second;
        Item info;
        info.own_bytes = bytes_.of(item.id());
        const auto& type = item.declaration();
        if (type.is("IfcPolygonalFaceSet")) {
            if (const auto coordinates = link(item, "Coordinates")) info.own_bytes += bytes_.of(coordinates.id());
            const auto faces = links(item, "Faces");
            info.faces = static_cast<uint32_t>(faces.size());
            for (const auto& face : faces) info.own_bytes += bytes_.of(face.id());
        } else if (type.is("IfcTriangulatedFaceSet")) {
            if (const auto coordinates = link(item, "Coordinates")) info.own_bytes += bytes_.of(coordinates.id());
            if (const auto indices = attribute(item, "CoordIndex")) info.faces = static_cast<uint32_t>(indices->size());
        } else if (type.is("IfcFacetedBrep") || type.is("IfcFacetedBrepWithVoids")) {
            if (const auto shell = link(item, "Outer")) {
                const auto faces = links(shell, "CfsFaces");
                info.faces = static_cast<uint32_t>(faces.size());
            }
        }
        return items_.emplace(item.id(), info).first->second;
    }

    const FileBytes& bytes_;
    const std::unordered_map<uint32_t, double>& transparency_;
    const double threshold_;
    std::unordered_map<uint32_t, Item> items_;
    std::unordered_set<uint32_t> seen_;
};

struct PsetTotals {
    uint64_t sets = 0, values = 0, bytes = 0;
};

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

struct Suspect {
    uint64_t faces = 0, bytes = 0, items = 0;
    std::string guid, type, family;
};

void write_suspects(const std::string& path, const std::vector<Suspect>& suspects) {
    std::ofstream csv(path);
    csv << "faces,bytes,items,guid,class,family\n";
    for (const auto& s : suspects) {
        csv << s.faces << ',' << s.bytes << ',' << s.items << ',' << s.guid << ',' << s.type << ',' << quoted(s.family) << '\n';
    }
}

void write_csv(const std::string& path, const std::map<std::pair<std::string, std::string>, Totals>& families) {
    std::ofstream csv(path);
    csv << "class,family,elements,bytes,items,definitions,faces,max_faces,heavy_elements,transparent_items,"
           "transparent_elements,mixed_elements\n";
    for (const auto& [key, t] : families) {
        csv << key.first << ',' << quoted(key.second) << ',' << t.elements << ',' << t.bytes << ',' << t.items << ','
            << t.definitions << ',' << t.faces << ',' << t.max_faces << ',' << t.heavy << ',' << t.transparent_items
            << ',' << t.transparent_elements << ',' << t.mixed_elements << '\n';
    }
}

}  // namespace

int report(const ReportOptions& options) {
    const auto start = std::chrono::steady_clock::now();
    const FileBytes bytes(options.ifc_path);

    ifcopenshell::file file(options.ifc_path);
    if (!file.good()) {
        std::cerr << options.ifc_path << ": can't be read as IFC\n";
        return 1;
    }

    const auto transparency = item_transparency(file);
    Walker walker(bytes, transparency, options.transparent);

    std::map<std::string, Totals> classes;
    std::map<std::pair<std::string, std::string>, Totals> families;
    std::vector<Suspect> heaviest;
    uint64_t geometry_bytes = 0;

    for (const auto& instance : all_of(file, "IfcProduct")) {
        const auto product = instance.as<express::entity>();
        const auto shape = link(product, "Representation");
        Walker::Result result;
        for (const auto& representation : links(shape, "Representations")) {
            walker.walk_representation(representation.as<express::entity>(), false, result);
        }
        const std::string type = product.declaration().name();
        for (Totals* t : {&classes[type], &families[{type, family_of(product)}]}) {
            ++t->elements;
            t->items += result.items;
            t->definitions += result.definitions;
            t->faces += result.faces;
            t->bytes += result.bytes;
            t->transparent_items += result.transparent;
            t->max_faces = std::max(t->max_faces, result.faces);
            if (result.faces >= options.heavy_faces) ++t->heavy;
            if (result.transparent && result.opaque) ++t->mixed_elements;
            if (result.transparent && !result.opaque) ++t->transparent_elements;
        }
        geometry_bytes += result.bytes;
        if (result.faces >= options.heavy_faces) {
            heaviest.push_back({result.faces, result.shown_bytes, result.items, text(product, "GlobalId"), type, family_of(product)});
        }
    }

    std::map<std::string, PsetTotals> psets;
    std::unordered_set<uint32_t> properties_seen;
    std::unordered_map<uint32_t, std::string> site_of;  // an element can hold one in both Asset and Data
    uint64_t property_bytes = 0;
    for (const auto& instance : all_of(file, "IfcRelDefinesByProperties")) {
        const auto relation = instance.as<express::entity>();
        const auto related = links(relation, "RelatedObjects");
        property_bytes += bytes.of(relation.id());
        const auto definition = link(relation, "RelatingPropertyDefinition");
        if (!definition) continue;
        const std::string name = text(definition, "Name");
        auto& totals = psets[name];
        const bool first = properties_seen.insert(definition.id()).second;
        if (first) {
            ++totals.sets;
            totals.bytes += bytes.of(definition.id());
            property_bytes += bytes.of(definition.id());
        }
        const bool holds_site = SITE_PSETS.count(name) != 0;
        for (const auto& p : links(definition, "HasProperties")) {
            const auto property = p.as<express::entity>();
            if (first && properties_seen.insert(property.id()).second) {
                ++totals.values;
                totals.bytes += bytes.of(property.id());
                property_bytes += bytes.of(property.id());
            }
            if (holds_site && text(property, "Name") == SITE_PROPERTY) {
                const std::string site = nominal_text(property);
                for (const auto& element : related) {
                    auto& held = site_of[element.id()];
                    if (held.empty() || NOT_A_SITE.count(held)) held = site;
                }
            }
        }
    }

    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
#ifndef _WIN32
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const double peak = usage.ru_maxrss / 1024.0;
#endif

    std::cout << std::fixed << std::setprecision(1);
    std::cout << std::filesystem::path(options.ifc_path).filename().string() << "\n"
              << "  schema " << file.schema()->name() << ", " << megabytes(bytes.total) << " MB, read in " << seconds
              << " s"
#ifndef _WIN32
              << ", peak memory " << peak << " MB"
#endif
              << "\n"
              << "  geometry reached from elements " << megabytes(geometry_bytes) << " MB, property sets "
              << megabytes(property_bytes) << " MB\n";

    std::cout << "\nBytes by entity type\n";
    std::vector<std::pair<uint64_t, std::string>> by_type;
    for (const auto& [type, size] : bytes.by_type) by_type.emplace_back(size, type);
    std::sort(by_type.rbegin(), by_type.rend());
    for (size_t i = 0; i < by_type.size() && i < 8; ++i) {
        std::cout << std::setw(9) << megabytes(by_type[i].first) << " MB " << std::setw(5)
                  << 100.0 * by_type[i].first / bytes.total << "%  " << by_type[i].second << "\n";
    }

    std::cout << "\nElements by class\n";
    std::vector<std::pair<std::string, Totals>> sorted(classes.begin(), classes.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    for (const auto& [type, t] : sorted) {
        std::cout << std::setw(9) << megabytes(t.bytes) << " MB " << std::setw(7) << t.elements << " el "
                  << std::setw(9) << t.faces << " faces  " << type << "\n";
    }

    std::cout << "\nHeaviest families (largest first; faces counted as shown, bytes counted once)\n"
              << "       MB    elem     defs     faces  max/elem  heavy  clear  family\n";
    std::vector<std::pair<std::pair<std::string, std::string>, Totals>> top(families.begin(), families.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    for (size_t i = 0; i < top.size() && i < options.top_families; ++i) {
        const auto& [key, t] = top[i];
        std::cout << std::setw(9) << megabytes(t.bytes) << std::setw(8) << t.elements << std::setw(9) << t.definitions
                  << std::setw(10) << t.faces << std::setw(10) << t.max_faces << std::setw(7) << t.heavy << std::setw(7)
                  << t.transparent_items << "  " << key.second << "\n";
    }

    std::sort(heaviest.begin(), heaviest.end(), [](const Suspect& a, const Suspect& b) { return a.faces > b.faces; });
    std::cout << "\nExplosions: " << heaviest.size() << " elements of " << options.heavy_faces << "+ faces\n";
    for (size_t i = 0; i < heaviest.size() && i < 5; ++i) {
        std::cout << std::setw(9) << heaviest[i].faces << "  " << heaviest[i].guid << "  " << heaviest[i].family << "\n";
    }

    uint64_t clear_items = 0, clear_elements = 0, mixed = 0;
    std::vector<std::pair<uint64_t, std::string>> clear_families;
    for (const auto& [key, t] : families) {
        clear_items += t.transparent_items;
        clear_elements += t.transparent_elements;
        mixed += t.mixed_elements;
        if (t.transparent_items) clear_families.emplace_back(t.transparent_items, key.second);
    }
    std::sort(clear_families.rbegin(), clear_families.rend());
    std::cout << "\nEnvelopes: " << clear_items << " items at transparency " << options.transparent << "+; "
              << clear_elements << " elements wholly clear, " << mixed << " part clear and part solid\n";
    for (size_t i = 0; i < clear_families.size() && i < 8; ++i) {
        std::cout << std::setw(9) << clear_families[i].first << "  " << clear_families[i].second << "\n";
    }

    std::map<std::string, uint64_t> site_elements;
    uint64_t not_a_site = 0;
    for (const auto& [element, site] : site_of) {
        if (NOT_A_SITE.count(site)) ++not_a_site;
        else ++site_elements[site];
    }
    uint64_t site_total = 0, shared_ids = 0, largest = 0;
    std::string largest_id;
    for (const auto& [site, count] : site_elements) {
        site_total += count;
        if (count > 1) ++shared_ids;
        if (count > largest) {
            largest = count;
            largest_id = site;
        }
    }
    std::cout << "\nSite IDs: " << site_elements.size() << " on " << site_total << " elements; " << shared_ids
              << " on more than one element";
    if (largest > 1) std::cout << " (most: " << largest_id << ", " << largest << ")";
    std::cout << "; " << not_a_site << " elements with a blank or placeholder ID\n";

    std::cout << "\nProperty sets by size\n";
    std::vector<std::pair<uint64_t, std::string>> set_sizes;
    for (const auto& [name, t] : psets) set_sizes.emplace_back(t.bytes, name);
    std::sort(set_sizes.rbegin(), set_sizes.rend());
    for (size_t i = 0; i < set_sizes.size() && i < 8; ++i) {
        const auto& t = psets[set_sizes[i].second];
        std::cout << std::setw(9) << megabytes(t.bytes) << " MB " << std::setw(8) << t.sets << " sets " << std::setw(9)
                  << t.values << " values  " << set_sizes[i].second << "\n";
    }

    if (!options.csv_path.empty()) write_csv(options.csv_path, families);
    if (!options.suspects_path.empty()) write_suspects(options.suspects_path, heaviest);
    return 0;
}

}  // namespace bimpass

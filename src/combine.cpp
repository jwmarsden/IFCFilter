#include "combine.h"

#include "extent.h"
#include "rigid.h"
#include "spf.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ifcfilter {
namespace {
namespace fs = std::filesystem;

constexpr const char* UNMANAGED = "Non-Managed Types";  // last of a building's levels, as split writes it

// A model's name, from its file's: T2DPAA-T2D-N5T-TU-M3D-101301
std::string model_name(const fs::path& path) {
    const std::string name = path.filename().string();
    return name.substr(0, name.find('.'));
}

std::string unquoted(std::string_view value) {
    const auto open = value.find('\'');
    const auto close = value.rfind('\'');
    return open == std::string_view::npos || close <= open ? std::string() : std::string(value.substr(open + 1, close - open - 1));
}

// A line's text after "#id=", with its references changed
std::string with_refs(std::string_view line, const std::function<uint32_t(uint32_t)>& change) {
    const auto equals = line.find('=');
    std::string_view text = equals == std::string_view::npos ? line : line.substr(equals + 1);
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    std::string out;
    out.reserve(text.size() + 16);
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\'') quoted = !quoted;
        if (!quoted && c == '#' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') {
            uint32_t id = 0;
            size_t j = i + 1;
            for (; j < text.size() && text[j] >= '0' && text[j] <= '9'; ++j) id = id * 10 + static_cast<uint32_t>(text[j] - '0');
            out += '#';
            out += std::to_string(change(id));
            i = j - 1;
            continue;
        }
        out += c;
    }
    return out;
}

std::string rebuilt(std::string_view type, const std::vector<std::string>& attributes) {
    std::string out(type);
    out += '(';
    for (size_t i = 0; i < attributes.size(); ++i) out += (i ? "," : "") + attributes[i];
    return out + ");";
}

std::vector<std::string> strings_of(std::string_view line) {
    std::vector<std::string> out;
    for (const auto part : attributes_of(line)) out.emplace_back(part);
    return out;
}

// What one file has of the structure that's shared
struct Structure {
    uint32_t project = 0;
    uint32_t context = 0;                           // the 3D model context
    std::map<std::string, uint32_t> subcontexts;    // identifier, type and view, and the subcontext
    std::vector<uint32_t> sites, buildings;
    std::map<std::string, uint32_t> storeys;        // name, and the first level of it
    std::unordered_map<uint32_t, uint32_t> placed;  // a site's, building's or level's placement, and it
    std::map<std::string, std::string> units;       // unit type, and what it is
};

Structure read_structure(const SpfFile& spf) {
    Structure s;
    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        if (spf.line(id).empty()) continue;
        const auto type = spf.type(id);
        if (type == "IFCPROJECT" && !s.project) {
            s.project = id;
        } else if (type == "IFCGEOMETRICREPRESENTATIONCONTEXT") {
            const auto a = attributes_of(spf.line(id));
            if (!s.context || (a.size() > 1 && a[1] == "'Model'")) s.context = id;
        } else if (type == "IFCGEOMETRICREPRESENTATIONSUBCONTEXT") {
            const auto a = attributes_of(spf.line(id));
            if (a.size() >= 9) s.subcontexts.emplace(std::string(a[0]) + "|" + std::string(a[1]) + "|" + std::string(a[8]), id);
        } else if (type == "IFCSITE" || type == "IFCBUILDING" || type == "IFCBUILDINGSTOREY") {
            const auto a = attributes_of(spf.line(id));
            if (a.size() < 6) continue;
            if (type == "IFCSITE") s.sites.push_back(id);
            else if (type == "IFCBUILDING") s.buildings.push_back(id);
            else s.storeys.emplace(unquoted(a[2]), id);
            const auto placement = refs_in(a[5]);
            if (placement.size() == 1) s.placed[placement[0]] = id;
        }
    }
    // The project's units, not those of properties that give their own
    const auto project = s.project ? attributes_of(spf.line(s.project)) : std::vector<std::string_view>{};
    const auto assignment = project.size() > 8 ? refs_in(project[8]) : std::vector<uint32_t>{};
    if (assignment.size() == 1) {
        const auto units = attributes_of(spf.line(assignment[0]));
        for (const uint32_t unit : units.empty() ? std::vector<uint32_t>{} : refs_in(units[0])) {
            const auto type = spf.type(unit);
            const auto a = attributes_of(spf.line(unit));
            if (type == "IFCSIUNIT" && a.size() == 4) s.units[std::string(a[1])] = std::string(a[2]) + std::string(a[3]);
            else if ((type == "IFCCONVERSIONBASEDUNIT" || type == "IFCCONVERSIONBASEDUNITWITHOFFSET") && a.size() >= 3) s.units[std::string(a[1])] = std::string(a[2]);
        }
    }
    return s;
}

uint32_t placement_of(const SpfFile& spf, uint32_t product) {
    const auto a = attributes_of(spf.line(product));
    const auto ids = a.size() > 5 ? refs_in(a[5]) : std::vector<uint32_t>{};
    return ids.size() == 1 ? ids[0] : 0;
}

struct GroupStats {
    size_t levels_joined = 0, levels_added = 0, placements_moved = 0, placements_left = 0;
};

// Writes the files, which share their units, as one. Returns false if they can't be.
bool combine_group(std::vector<fs::path> paths, const fs::path& out_path, GroupStats& stats) {
    // The biggest model's project, site and building are the ones kept, as most of what's combined is likely to be
    // of its zone, not the first by name's: in N5T's walls, that was C3S's portal panels
    std::stable_sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) { return fs::file_size(a) > fs::file_size(b); });
    std::vector<std::unique_ptr<SpfFile>> files;
    std::vector<Structure> structures;
    for (const fs::path& path : paths) {
        files.push_back(std::make_unique<SpfFile>(path.string()));
        structures.push_back(read_structure(*files.back()));
    }
    const Structure& base = structures[0];
    if (!base.project || !base.context || base.sites.empty() || base.buildings.empty()) {
        std::cerr << paths[0].string() << ": has no project, context, site or building to put the rest under\n";
        return false;
    }
    for (size_t f = 1; f < files.size(); ++f) {
        const auto& units = structures[f].units;
        for (const auto& [type, unit] : units) {
            const auto there = base.units.find(type);
            if (there == base.units.end() || there->second == unit) continue;
            if (type == ".LENGTHUNIT." || type == ".AREAUNIT." || type == ".VOLUMEUNIT.") {
                std::cerr << paths[f].string() << ": its " << type << " is " << unit << ", not " << there->second << " as in " << paths[0].filename().string()
                          << ", and converting isn't done\n";
                return false;
            }
            std::cerr << "warning: " << paths[f].filename().string() << ": its " << type << " is " << unit << ", not " << there->second << "\n";
        }
    }

    // Where each file's instances go in the one written: one after the other
    std::vector<uint32_t> offset(files.size(), 0);
    for (size_t f = 1; f < files.size(); ++f) offset[f] = offset[f - 1] + files[f - 1]->max_id();
    const uint32_t end = offset.back() + files.back()->max_id();
    Emitter emitter(end);

    // What's shared, in each file but the first, and what takes its place, as it's numbered in the file written
    struct Target {
        size_t file = 0;
        uint32_t id = 0;  // in that file
    };
    std::vector<std::unordered_map<uint32_t, uint32_t>> instead(files.size());
    std::vector<std::unordered_map<uint32_t, Target>> target_of(files.size());  // for the sites, buildings and levels
    std::vector<std::unordered_map<uint32_t, std::string>> rewritten(files.size());  // relationships with some of what they held taken out
    std::vector<uint32_t> new_levels;  // in the file written
    std::vector<std::unique_ptr<Extents>> extents;
    for (const auto& file : files) extents.push_back(std::make_unique<Extents>(*file));
    std::unordered_map<uint32_t, std::string> replaced;  // in the file written
    // Every level so far, by name: the first file's, and then each new one, so that levels of the same name in
    // any of the files are one
    std::map<std::string, Target> levels;
    for (const auto& [name, id] : base.storeys) levels.emplace(name, Target{0, id});
    // An application, such as Revit or IFCFilter, can only be in a file once by its identifier and version, as each
    // file has its own
    std::map<std::string, Target> applications;
    const auto application_key = [](const SpfFile& spf, uint32_t id) {
        const auto a = attributes_of(spf.line(id));
        return a.size() == 4 ? std::string(a[1]) + "|" + std::string(a[3]) : std::string();
    };
    for (uint32_t id = 0; id < files[0]->max_id(); ++id) {
        if (!files[0]->line(id).empty() && files[0]->type(id) == "IFCAPPLICATION") applications.emplace(application_key(*files[0], id), Target{0, id});
    }
    for (size_t f = 1; f < files.size(); ++f) {
        const SpfFile& spf = *files[f];
        const Structure& s = structures[f];
        auto& to = instead[f];
        const auto take = [&](uint32_t id, Target target) {
            to[id] = offset[target.file] + target.id;
            target_of[f][id] = target;
        };
        if (s.project) take(s.project, {0, base.project});
        if (s.context) take(s.context, {0, base.context});
        for (const auto& [key, id] : s.subcontexts) {
            const auto match = base.subcontexts.find(key);
            if (match != base.subcontexts.end()) take(id, {0, match->second});
        }
        for (const uint32_t site : s.sites) take(site, {0, base.sites[0]});
        for (const uint32_t building : s.buildings) take(building, {0, base.buildings[0]});
        for (const auto& [name, id] : s.storeys) {
            const auto [at, added] = levels.emplace(name, Target{f, id});
            if (added) continue;
            take(id, at->second);
            ++stats.levels_joined;
        }
        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            if (spf.line(id).empty() || spf.type(id) != "IFCAPPLICATION") continue;
            const auto [at, added] = applications.emplace(application_key(spf, id), Target{f, id});
            if (!added) take(id, at->second);
        }
        // What a file's building holds that isn't joined to something, such as a level the others don't have,
        // goes in the first file's building. What's under a level that's joined stays under it, apart from
        // levels joined to one that's already somewhere, as a level can only be in one place.
        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            if (spf.line(id).empty() || spf.type(id) != "IFCRELAGGREGATES") continue;
            const auto a = attributes_of(spf.line(id));
            const auto whole = a.size() == 6 ? refs_in(a[4]) : std::vector<uint32_t>{};
            if (whole.size() != 1 || !to.count(whole[0])) continue;
            std::vector<uint32_t> left;
            for (const uint32_t part : refs_in(a[5])) {
                if (!to.count(part)) left.push_back(part);
            }
            const bool building = std::find(s.buildings.begin(), s.buildings.end(), whole[0]) != s.buildings.end();
            if (building) {
                for (const uint32_t part : left) {
                    new_levels.push_back(offset[f] + part);
                    ++stats.levels_added;
                }
            }
            if (building || left.empty()) {
                to[id] = 0;  // left out
                continue;
            }
            auto attributes = strings_of(spf.line(id));
            std::vector<uint32_t> written;
            for (const uint32_t part : left) written.push_back(offset[f] + part);
            attributes[4] = reference(to.at(whole[0]));
            attributes[5] = list_of(written);
            rewritten[f][id] = rebuilt("IFCRELAGGREGATES", attributes);
        }
    }

    // The first file's building gets the new levels, in alphabetical order, with what has no site ID last
    std::unordered_map<uint32_t, std::string> level_name;
    for (size_t f = 0; f < files.size(); ++f) {
        for (const auto& [name, id] : structures[f].storeys) level_name[offset[f] + id] = name;
    }
    {
        const SpfFile& spf = *files[0];
        uint32_t aggregation = 0;
        for (uint32_t id = 0; id < spf.max_id() && !aggregation; ++id) {
            if (spf.line(id).empty() || spf.type(id) != "IFCRELAGGREGATES") continue;
            const auto a = attributes_of(spf.line(id));
            const auto whole = a.size() == 6 ? refs_in(a[4]) : std::vector<uint32_t>{};
            if (whole.size() == 1 && whole[0] == base.buildings[0]) aggregation = id;
        }
        std::vector<uint32_t> children = new_levels;
        auto attributes = aggregation ? strings_of(spf.line(aggregation)) : std::vector<std::string>{};
        if (aggregation) {
            const auto existing = refs_in(attributes[5]);
            children.insert(children.begin(), existing.begin(), existing.end());
        }
        std::stable_sort(children.begin(), children.end(), [&](uint32_t a, uint32_t b) {
            const auto& na = level_name[a];
            const auto& nb = level_name[b];
            const bool la = na == UNMANAGED, lb = nb == UNMANAGED;
            return la != lb ? lb : na < nb;
        });
        children.erase(std::unique(children.begin(), children.end()), children.end());
        if (aggregation) {
            attributes[5] = list_of(children);
            replaced[aggregation] = rebuilt("IFCRELAGGREGATES", attributes);
        } else if (!children.empty()) {
            emitter.emit("IFCRELAGGREGATES('" + derived_guid("combined levels@" + out_path.filename().string()) + "',$,$,$," + reference(base.buildings[0]) + "," +
                         list_of(children) + ");");
        }
        // The project is named after what it is now
        auto project = strings_of(spf.line(base.project));
        if (project.size() > 2) {
            project[2] = "'" + out_path.stem().stem().string() + "'";
            // and says what it's made of, which a viewer shows, as it doesn't show the header
            std::string made_of = "Combined from " + std::to_string(paths.size()) + " models:";
            for (size_t f = 0; f < paths.size(); ++f) made_of += (f ? ", " : " ") + model_name(paths[f]);
            if (project.size() > 3) project[3] = "'" + made_of + "'";
            replaced[base.project] = rebuilt("IFCPROJECT", project);
        }
    }

    std::ofstream out(out_path, std::ios::binary);
    {
        // The header names the file written and what it's made of
        std::string header(files[0]->header());
        const auto at = header.find("FILE_NAME");
        const auto open = at == std::string::npos ? at : header.find('\'', at);
        const auto close = open == std::string::npos ? open : header.find('\'', open + 1);
        if (close != std::string::npos) header.replace(open + 1, close - open - 1, out_path.filename().string());
        // and the description, a list of strings, gets one for each model it's made of, after what's there
        const auto described = header.find("FILE_DESCRIPTION");
        const auto list = described == std::string::npos ? described : header.find('(', header.find('(', described) + 1);
        if (list != std::string::npos) {
            // The end of the list: the first ')' outside a string
            bool quoted = false;
            size_t list_end = std::string::npos;
            for (size_t i = list + 1; i < header.size(); ++i) {
                if (header[i] == '\'') quoted = !quoted;
                else if (!quoted && header[i] == ')') {
                    list_end = i;
                    break;
                }
            }
            if (list_end != std::string::npos) {
                std::string models;
                for (const fs::path& path : paths) models += ",'Combined from " + model_name(path) + "'";
                header.insert(list_end, models);
            }
        }
        out << header;
    }
    for (size_t f = 0; f < files.size(); ++f) {
        const SpfFile& spf = *files[f];
        const auto& to = instead[f];
        const Structure& s = structures[f];
        const auto change = [&](uint32_t id) {
            const auto mapped = to.find(id);
            return mapped != to.end() && mapped->second ? mapped->second : offset[f] + id;
        };
        for (uint32_t id = 0; id < spf.max_id(); ++id) {
            const auto line = spf.line(id);
            if (line.empty() || to.count(id)) continue;
            const uint32_t written = offset[f] + id;
            const auto own = replaced.find(written);
            if (f == 0 && own != replaced.end()) {
                out << '#' << written << '=' << own->second << '\n';
                continue;
            }
            const auto changed = rewritten[f].find(id);
            if (changed != rewritten[f].end()) {
                out << '#' << written << '=' << changed->second << '\n';
                continue;
            }
            const auto type = spf.type(id);
            if (f > 0) {
                // Relationships of nothing but what's been left out go too, such as the sets of a site
                if (type == "IFCRELDEFINESBYPROPERTIES" || type == "IFCRELDEFINESBYTYPE" || type.rfind("IFCRELASSOCIATES", 0) == 0) {
                    const auto a = attributes_of(line);
                    const auto related = a.size() > 4 ? refs_in(a[4]) : std::vector<uint32_t>{};
                    if (!related.empty() && std::all_of(related.begin(), related.end(), [&](uint32_t r) { return to.count(r) > 0; })) continue;
                }
                // What a site, building or level that's been left out placed is placed from what's taken its
                // place, where it was
                if (type == "IFCLOCALPLACEMENT") {
                    const auto a = attributes_of(line);
                    const auto parent = a.size() == 2 ? refs_in(a[0]) : std::vector<uint32_t>{};
                    const auto spatial = parent.size() == 1 ? s.placed.find(parent[0]) : s.placed.end();
                    const auto target = spatial != s.placed.end() ? target_of[f].find(spatial->second) : target_of[f].end();
                    if (target != target_of[f].end()) {
                        const Target& there = target->second;
                        const uint32_t new_placement = placement_of(*files[there.file], there.id);
                        const auto old_parent = extents[f]->placement(a[0]);
                        const auto new_parent = new_placement ? extents[there.file]->placement(reference(new_placement)) : std::nullopt;
                        const auto local_ids = refs_in(a[1]);
                        const auto local = local_ids.size() == 1 ? axis_placement(spf, local_ids[0]) : std::nullopt;
                        if (old_parent && new_parent && local) {
                            const Rigid moved = compose(inverse(*new_parent), compose(*old_parent, *local));
                            out << '#' << written << "=IFCLOCALPLACEMENT(" << reference(offset[there.file] + new_placement) << "," << reference(emitter.emit_placement(moved))
                                << ");\n";
                            ++stats.placements_moved;
                            continue;
                        }
                        ++stats.placements_left;
                    }
                }
            }
            out << '#' << written << '=' << with_refs(line, change) << '\n';
        }
    }
    for (const std::string& line : emitter.lines()) out << line << '\n';
    out << "ENDSEC;\nEND-ISO-10303-21;\n";
    return true;
}

std::vector<fs::path> federation_files_in(const std::vector<std::string>& inputs) {
    std::vector<fs::path> out;
    for (const std::string& input : inputs) {
        if (fs::is_directory(input)) {
            for (const auto& entry : fs::recursive_directory_iterator(input)) {
                if (entry.is_regular_file() && entry.path().extension() == ".ifc") out.push_back(entry.path());
            }
        } else {
            out.emplace_back(input);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> parts_of(const std::string& stem) {
    std::vector<std::string> out;
    std::stringstream in(stem);
    for (std::string part; std::getline(in, part, '-');) out.push_back(part);
    return out;
}

std::string model_of(const fs::path& path) {
    const std::string name = path.filename().string();
    return name.substr(0, name.find('.'));
}

// One line of a CSV file, as Excel writes it
bool read_row(std::istream& in, std::vector<std::string>& fields) {
    fields.clear();
    std::string field;
    bool quoted = false, any = false;
    char c;
    while (in.get(c)) {
        any = true;
        if (quoted) {
            if (c == '"') {
                if (in.peek() == '"') {
                    field += '"';
                    in.get(c);
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

std::string csv_field(const std::string& text) {
    if (text.find_first_of(",\"\n") == std::string::npos) return text;
    std::string out = "\"";
    for (const char c : text) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

// What a model holds, to tell it apart: how many elements, and the names most of them have
std::pair<size_t, std::string> contents_of(const fs::path& path) {
    const SpfFile spf(path.string());
    std::map<std::string, size_t> names;
    size_t elements = 0;
    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        const auto line = spf.line(id);
        if (line.empty()) continue;
        const auto type = spf.type(id);
        if (type == "IFCSITE" || type == "IFCBUILDING" || type == "IFCBUILDINGSTOREY" || type == "IFCSPACE" || type == "IFCOPENINGELEMENT" || type.rfind("IFCREL", 0) == 0) continue;
        const auto a = attributes_of(line);
        // A product with a body: a GlobalId, then a placement and a shape
        if (a.size() < 8 || a[0].size() != 24 || a[0][0] != '\'' || refs_in(a[5]).size() != 1 || refs_in(a[6]).size() != 1) continue;
        ++elements;
        std::string name = unquoted(a[2]);
        while (!name.empty() && (std::isdigit(static_cast<unsigned char>(name.back())) || name.back() == ':')) name.pop_back();
        if (!name.empty()) ++names[name];
    }
    std::vector<std::pair<size_t, std::string>> sorted;
    for (const auto& [name, count] : names) sorted.emplace_back(count, name);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string text;
    for (size_t i = 0; i < sorted.size() && i < 3; ++i) text += (i ? "; " : "") + sorted[i].second + " (" + std::to_string(sorted[i].first) + ")";
    return {elements, text};
}

constexpr const char* REGISTER_HEADER = "combined,model,discipline,zone,mb,elements,contents";

// Reads the register, adds what's given that it doesn't have, and writes it again. Returns what to combine,
// by name; empty, with written true, if it didn't exist and has just been made.
std::map<std::string, std::vector<fs::path>> from_register(const CombineOptions& options, const std::vector<fs::path>& files, bool& written) {
    struct Row {
        std::vector<std::string> fields;  // as REGISTER_HEADER
    };
    std::vector<Row> rows;
    std::map<std::string, size_t> row_of;  // model, and its row
    written = !fs::exists(options.register_path);
    if (!written) {
        std::ifstream in(options.register_path, std::ios::binary);
        if (in.peek() == 0xEF) in.ignore(3);  // Excel's byte order mark
        std::vector<std::string> fields;
        read_row(in, fields);
        std::map<std::string, size_t> at;
        for (size_t i = 0; i < fields.size(); ++i) at[fields[i]] = i;
        if (!at.count("combined") || !at.count("model")) throw std::runtime_error(options.register_path + ": needs combined and model columns");
        while (read_row(in, fields)) {
            if (fields.size() == 1 && fields[0].empty()) continue;
            const auto get = [&](const char* column) {
                const auto i = at.find(column);
                return i != at.end() && i->second < fields.size() ? fields[i->second] : std::string();
            };
            Row row;
            for (const char* column : {"combined", "model", "discipline", "zone", "mb", "elements", "contents"}) row.fields.push_back(get(column));
            if (row.fields[1].empty()) continue;
            row_of[row.fields[1]] = rows.size();
            rows.push_back(std::move(row));
        }
    }
    std::map<std::string, fs::path> path_of;
    size_t added = 0;
    for (const fs::path& path : files) {
        const std::string model = model_of(path);
        path_of[model] = path;
        if (row_of.count(model)) continue;
        const auto parts = parts_of(model);
        const auto [elements, contents] = contents_of(path);
        char mb[32];
        std::snprintf(mb, sizeof mb, "%.1f", static_cast<double>(fs::file_size(path)) / (1 << 20));
        Row row;
        row.fields = {"", model, fs::absolute(path).parent_path().filename().string(),
                      options.name_part >= 1 && parts.size() >= static_cast<size_t>(options.name_part) ? parts[static_cast<size_t>(options.name_part - 1)] : "", mb,
                      std::to_string(elements), contents};
        row_of[model] = rows.size();
        rows.push_back(std::move(row));
        ++added;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.fields[1] < b.fields[1]; });  // by model, alphabetically
    {
        std::ofstream out(options.register_path, std::ios::binary);
        out << "\xEF\xBB\xBF" << REGISTER_HEADER << "\r\n";  // with a byte order mark, so Excel reads it as UTF-8
        for (const Row& row : rows) {
            for (size_t i = 0; i < row.fields.size(); ++i) out << (i ? "," : "") << csv_field(row.fields[i]);
            out << "\r\n";
        }
    }
    std::cout << options.register_path << ": " << rows.size() << " models, " << added << " of them new\n";

    std::map<std::string, std::vector<fs::path>> groups;
    for (const Row& row : rows) {
        const std::string& combined = row.fields[0];
        if (combined.empty()) continue;
        const auto path = path_of.find(row.fields[1]);
        if (path == path_of.end()) {
            std::cerr << "warning: " << row.fields[1] << " is to go in " << combined << ", but isn't in what was given, so it's left out\n";
            continue;
        }
        groups[combined].push_back(path->second);
    }
    return groups;
}

}  // namespace

int combine(const CombineOptions& options) {
    const auto files = federation_files_in(options.inputs);
    fs::create_directories(options.out_dir);
    std::vector<std::pair<fs::path, std::vector<fs::path>>> plan;  // what to write, and from what
    if (!options.register_path.empty()) {
        bool written = false;
        for (auto& [name, paths] : from_register(options, files, written)) {
            const std::string file = name.size() > 4 && name.substr(name.size() - 4) == ".ifc" ? name : name + ".federate.ifc";
            plan.emplace_back(fs::path(options.out_dir) / file, std::move(paths));
        }
        if (written) {
            std::cout << "fill in its combined column with what each model goes in, and run this again\n";
            return 0;
        }
        if (plan.empty()) std::cout << "nothing in its combined column, so nothing to do\n";
    } else {
        std::map<std::string, std::vector<fs::path>> groups;
        for (const fs::path& path : files) {
            const auto parts = parts_of(model_of(path));
            if (options.name_part < 1 || parts.size() < static_cast<size_t>(options.name_part)) {
                std::cerr << path.string() << ": hasn't a part " << options.name_part << " in its name, so it's left out\n";
                continue;
            }
            groups[parts[static_cast<size_t>(options.name_part - 1)]].push_back(path);
        }
        // Each is named after the parts of the name every file given has, in order, with its own part where it
        // goes: T2DPAA-T2D-N5T-TU-M3D, even for a zone of one model
        const auto first_parts = files.empty() ? std::vector<std::string>{} : parts_of(model_of(files[0]));
        std::vector<bool> shared(first_parts.size());
        for (size_t i = 0; i < first_parts.size(); ++i) {
            shared[i] = std::all_of(files.begin(), files.end(), [&](const fs::path& p) {
                const auto other = parts_of(model_of(p));
                return i < other.size() && other[i] == first_parts[i];
            });
        }
        for (auto& [key, paths] : groups) {
            std::string name;
            for (size_t i = 0; i < first_parts.size(); ++i) {
                if (i == static_cast<size_t>(options.name_part - 1)) name += (name.empty() ? "" : "-") + key;
                else if (shared[i]) name += (name.empty() ? "" : "-") + first_parts[i];
            }
            plan.emplace_back(fs::path(options.out_dir) / (name + ".federate.ifc"), std::move(paths));
        }
    }

    int result = 0;
    for (const auto& [out_path, paths] : plan) {
        GroupStats stats;
        uint64_t bytes_in = 0;
        for (const fs::path& path : paths) bytes_in += fs::file_size(path);
        if (!combine_group(paths, out_path, stats)) {
            result = 1;
            continue;
        }
        std::cout << out_path.filename().string() << ": " << paths.size() << " models, " << bytes_in / (1 << 20) << " -> " << fs::file_size(out_path) / (1 << 20) << " MB; "
                  << stats.levels_joined << " levels joined to one of the same name, " << stats.levels_added << " added; " << stats.placements_moved
                  << " placements now from the first model's site, building or levels" << (stats.placements_left ? ", " + std::to_string(stats.placements_left) + " left as they were" : "")
                  << "\n" << std::flush;
    }
    return result;
}

std::map<std::string, std::string> combine_groups(const std::string& register_path) {
    std::map<std::string, std::string> out;
    std::ifstream in(register_path, std::ios::binary);
    if (!in) throw std::runtime_error(register_path + ": can't be read");
    if (in.peek() == 0xEF) in.ignore(3);  // Excel's byte order mark
    std::vector<std::string> fields;
    read_row(in, fields);
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < fields.size(); ++i) at[fields[i]] = i;
    if (!at.count("combined") || !at.count("model")) throw std::runtime_error(register_path + ": needs combined and model columns");
    while (read_row(in, fields)) {
        const size_t c = at["combined"], m = at["model"];
        if (c < fields.size() && m < fields.size() && !fields[c].empty() && !fields[m].empty()) out[fields[m]] = fields[c];
    }
    return out;
}

bool combine_files(const std::vector<fs::path>& paths, const fs::path& out_path, std::string& summary) {
    GroupStats stats;
    uint64_t bytes_in = 0;
    for (const fs::path& path : paths) bytes_in += fs::file_size(path);
    fs::create_directories(out_path.parent_path());
    if (!combine_group(paths, out_path, stats)) return false;
    summary = std::to_string(paths.size()) + " models, " + std::to_string(bytes_in / (1 << 20)) + " -> " + std::to_string(fs::file_size(out_path) / (1 << 20)) + " MB; " +
              std::to_string(stats.levels_joined) + " levels joined to one of the same name, " + std::to_string(stats.levels_added) + " added; " +
              std::to_string(stats.placements_moved) + " placements now from the first model's site, building or levels";
    return true;
}

}  // namespace ifcfilter

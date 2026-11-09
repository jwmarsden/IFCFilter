#include "breps.h"

#include "extent.h"
#include "spf.h"
#include "system.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ifcfilter {
namespace {

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::string schema_of(std::string_view header) {
    const auto at = header.find("FILE_SCHEMA");
    if (at == std::string_view::npos) return {};
    const auto open = header.find('\'', at);
    const auto close = open == std::string_view::npos ? open : header.find('\'', open + 1);
    if (close == std::string_view::npos) return {};
    return std::string(header.substr(open + 1, close - open - 1));
}

void append_indices(const std::vector<uint32_t>& indices, std::string& out) {
    out += '(';
    for (size_t i = 0; i < indices.size(); ++i) {
        if (i) out += ',';
        out += std::to_string(indices[i]);
    }
    out += ')';
}

class Converter {
  public:
    Converter(const SpfFile& spf, bool tag_list, BrepStats& stats) : spf_(spf), tag_list_(tag_list), stats_(stats), next_id_(spf.max_id()) {}

    // The lines that replace a brep, ending with the face set under its id; empty if it stays as it is.
    // parts gets the shell and what it's made of.
    std::string convert(uint32_t brep, std::vector<uint32_t>& parts) {
        const auto attributes = attributes_of(spf_.line(brep));
        const auto shells = attributes.size() == 1 ? refs_in(attributes[0]) : std::vector<uint32_t>{};
        if (shells.size() != 1 || spf_.type(shells[0]) != "IFCCLOSEDSHELL") return {};
        const auto shell = attributes_of(spf_.line(shells[0]));
        if (shell.size() != 1) return {};
        parts.push_back(shells[0]);

        coordinates_.clear();
        index_of_.clear();
        std::string faces_text;
        std::vector<uint32_t> face_ids;
        uint64_t dropped = 0;
        for (const uint32_t face : refs_in(shell[0])) {
            if (spf_.type(face) != "IFCFACE") return {};
            const auto bounds = attributes_of(spf_.line(face));
            if (bounds.size() != 1) return {};
            parts.push_back(face);
            std::vector<std::vector<uint32_t>> loops_of_face;
            size_t outer_at = 0;
            bool outer_given = false;
            for (const uint32_t bound : refs_in(bounds[0])) {
                const auto type = spf_.type(bound);
                if (type != "IFCFACEOUTERBOUND" && type != "IFCFACEBOUND") return {};
                const auto bound_attributes = attributes_of(spf_.line(bound));
                const auto loops = bound_attributes.size() == 2 ? refs_in(bound_attributes[0]) : std::vector<uint32_t>{};
                if (loops.size() != 1 || spf_.type(loops[0]) != "IFCPOLYLOOP") return {};
                const auto loop = attributes_of(spf_.line(loops[0]));
                if (loop.size() != 1) return {};
                parts.push_back(bound);
                parts.push_back(loops[0]);
                std::vector<uint32_t> indices;
                for (const uint32_t point : refs_in(loop[0])) {
                    const uint32_t index = index_of(point, parts);
                    if (!index) return {};
                    if (indices.empty() || indices.back() != index) indices.push_back(index);
                }
                while (indices.size() > 1 && indices.front() == indices.back()) indices.pop_back();
                if (trimmed(bound_attributes[1]) == ".F.") std::reverse(indices.begin(), indices.end());
                if (type == "IFCFACEOUTERBOUND" && !outer_given) {
                    outer_at = loops_of_face.size();
                    outer_given = true;
                }
                loops_of_face.push_back(std::move(indices));
            }
            // The outer bound is the one that says so, or else the first; holes with no area are left out
            std::vector<uint32_t> outer;
            std::vector<std::vector<uint32_t>> inner;
            for (size_t i = 0; i < loops_of_face.size(); ++i) {
                if (i == outer_at) outer = std::move(loops_of_face[i]);
                else if (loops_of_face[i].size() > 2) inner.push_back(std::move(loops_of_face[i]));
            }
            if (outer.size() < 3) {
                ++dropped;
                continue;
            }
            const uint32_t id = next_id_++;
            faces_text += '#';
            faces_text += std::to_string(id);
            if (inner.empty()) {
                faces_text += "=IFCINDEXEDPOLYGONALFACE(";
                append_indices(outer, faces_text);
            } else {
                faces_text += "=IFCINDEXEDPOLYGONALFACEWITHVOIDS(";
                append_indices(outer, faces_text);
                faces_text += ",(";
                for (size_t i = 0; i < inner.size(); ++i) {
                    if (i) faces_text += ',';
                    append_indices(inner[i], faces_text);
                }
                faces_text += ')';
            }
            faces_text += ");\n";
            face_ids.push_back(id);
        }
        if (face_ids.empty()) return {};  // a face set can't be empty

        const uint32_t list = next_id_++;
        std::string out = "#" + std::to_string(list) + "=IFCCARTESIANPOINTLIST3D((";
        for (size_t i = 0; i < coordinates_.size(); ++i) {
            if (i) out += ',';
            out += coordinates_[i];
        }
        out += tag_list_ ? "),$);\n" : "));\n";
        out += faces_text;
        out += "#" + std::to_string(brep) + "=IFCPOLYGONALFACESET(#" + std::to_string(list) + ",.T.,(";
        for (size_t i = 0; i < face_ids.size(); ++i) {
            if (i) out += ',';
            out += '#';
            out += std::to_string(face_ids[i]);
        }
        out += "),$);";
        stats_.faces += face_ids.size();
        stats_.faces_dropped += dropped;
        return out;
    }

  private:
    // A point's place in the list, from 1, with points written the same way given the same place; 0 if it isn't a 3D point
    uint32_t index_of(uint32_t point, std::vector<uint32_t>& parts) {
        if (spf_.type(point) != "IFCCARTESIANPOINT") return 0;
        const auto attributes = attributes_of(spf_.line(point));
        if (attributes.size() != 1) return 0;
        std::string text;
        for (const char c : attributes[0]) {
            if (c != ' ') text += c;
        }
        if (std::count(text.begin(), text.end(), ',') != 2) return 0;
        parts.push_back(point);
        const auto [at, added] = index_of_.emplace(text, static_cast<uint32_t>(coordinates_.size() + 1));
        if (added) coordinates_.push_back(std::move(text));
        return at->second;
    }

    const SpfFile& spf_;
    const bool tag_list_;
    BrepStats& stats_;
    uint32_t next_id_;
    std::vector<std::string> coordinates_;
    std::unordered_map<std::string, uint32_t> index_of_;
};

}  // namespace

BrepStats breps_to_face_sets(const std::string& path) {
    namespace fs = std::filesystem;
    BrepStats stats;
    stats.bytes_before = stats.bytes_after = fs::file_size(path);
    const std::string temporary = path + ".breps";
    {
        const SpfFile spf(path);
        const std::string schema = schema_of(spf.header());
        if (schema.rfind("IFC4", 0) != 0) {
            stats.schema = false;
            return stats;
        }
        const uint32_t max_id = spf.max_id();
        Converter converter(spf, schema.rfind("IFC4X3", 0) == 0, stats);  // 4x3 point lists end with a tag list

        // What an opening reaches stays as it is, as taking a solid from a mesh is fragile
        const auto cut = sets_openings_touch(spf);
        std::unordered_map<uint32_t, std::string> converted;
        std::vector<bool> part(max_id, false);
        std::vector<uint32_t> parts;
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto type = spf.type(id);
            if (type == "IFCFACETEDBREPWITHVOIDS") ++stats.left;
            if (type != "IFCFACETEDBREP") continue;
            if (cut.count(id)) {
                ++stats.left;
                ++stats.left_cut;
                continue;
            }
            parts.clear();
            std::string lines = converter.convert(id, parts);
            if (lines.empty()) {
                ++stats.left;
                continue;
            }
            for (const uint32_t p : parts) part[p] = true;
            converted.emplace(id, std::move(lines));
            ++stats.converted;
        }
        if (converted.empty()) return stats;

        // A part that anything else uses stays, with what it's made of
        std::vector<bool> keep(max_id, false);
        std::vector<uint32_t> stack;
        for (uint32_t id = 0; id < max_id; ++id) {
            if (part[id] || converted.count(id) || spf.line(id).empty()) continue;
            for_each_ref(spf.line(id), [&](uint32_t next) {
                if (next < max_id && part[next] && !keep[next]) stack.push_back(next);
            });
        }
        while (!stack.empty()) {
            const uint32_t id = stack.back();
            stack.pop_back();
            if (keep[id]) continue;
            keep[id] = true;
            for_each_ref(spf.line(id), [&](uint32_t next) {
                if (next < max_id && part[next] && !keep[next]) stack.push_back(next);
            });
        }

        std::ofstream out(temporary, std::ios::binary);
        out << spf.header();
        std::string text;
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto line = spf.line(id);
            if (line.empty() || (part[id] && !keep[id])) continue;
            const auto done = converted.find(id);
            if (done != converted.end()) {
                out << done->second << '\n';
                continue;
            }
            // A representation of nothing but face sets now is a tessellation
            if (spf.type(id) == "IFCSHAPEREPRESENTATION") {
                const auto attributes = attributes_of(line);
                if (attributes.size() == 4 && trimmed(attributes[2]) == "'Brep'") {
                    const auto items = refs_in(attributes[3]);
                    if (!items.empty() && std::all_of(items.begin(), items.end(), [&](uint32_t item) { return converted.count(item) > 0; })) {
                        text.assign(line.substr(0, line.find('(') + 1));
                        text.append(attributes[0]).append(",").append(attributes[1]).append(",'Tessellation',").append(attributes[3]).append(");");
                        out << text << '\n';
                        continue;
                    }
                }
            }
            out << line << '\n';
        }
        out << "ENDSEC;\nEND-ISO-10303-21;\n";
    }
    replace_file(temporary, path);
    stats.bytes_after = fs::file_size(path);
    return stats;
}

}  // namespace ifcfilter

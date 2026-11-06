#include "split.h"

#include "compact.h"
#include "ifc_access.h"
#include "spf.h"
#include "zip.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bimpass {
namespace {

enum Output : uint8_t { FEDERATION = 0, HIDDEN = 1, ENVELOPES = 2 };
constexpr std::array<const char*, 3> OUTPUT_NAMES = {"federate", "hidden", "envelopes"};
constexpr std::array<const char*, 3> SUFFIXES = {".federate.ifc", ".hidden.ifc", ".envelopes.ifc"};
constexpr std::array<const char*, 3> FOLDERS = {"federate", "non-federate", "non-federate"};
constexpr const char* ZIPPED = "zip";  // added to an .ifc, makes .ifczip

// A family matches a rule if its name holds the rule's text
struct Rules {
    std::vector<std::string> keep_largest;  // the largest solid is the thing; the rest is detail inside it
    std::vector<std::string> physical;      // transparent, but really there (gratings)
    std::vector<std::string> envelope;      // opaque, but not really there (clearance envelopes)

    static bool matches(const std::vector<std::string>& list, const std::string& family) {
        return std::any_of(list.begin(), list.end(), [&](const std::string& s) { return family.find(s) != std::string::npos; });
    }

    static Rules load(const std::string& path) {
        Rules rules;
        if (path.empty()) return rules;
        std::ifstream in(path);
        if (!in) throw std::runtime_error(path + ": can't be read");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            const auto space = line.find(' ');
            const std::string action = line.substr(0, space);
            const std::string family = space == std::string::npos ? "" : line.substr(line.find_first_not_of(' ', space));
            if (action == "keep-largest") rules.keep_largest.push_back(family);
            else if (action == "physical") rules.physical.push_back(family);
            else if (action == "envelope") rules.envelope.push_back(family);
            else throw std::runtime_error(path + ": unknown rule " + action);
        }
        return rules;
    }
};

// 128 bits from a name, in IFC's GlobalId alphabet. The same part of the same element gets the same GUID on every run.
std::string derived_guid(const std::string& name) {
    static const char* ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";
    uint64_t hi = 0x6c62272e07bb0142ULL, lo = 0x62b821756295c58dULL;  // FNV-1a 128
    for (const unsigned char c : name) {
        lo ^= c;
        // multiply by 2^88 + 0x13B, in 32-bit parts so that it needs no 128-bit type
        const uint64_t low_part = (lo & 0xFFFFFFFFULL) * 0x13BULL;
        const uint64_t high_part = (lo >> 32) * 0x13BULL + (low_part >> 32);
        const uint64_t carry = high_part >> 32;
        const uint64_t new_hi = hi * 0x13BULL + carry + (lo << 24);
        lo = (high_part << 32) | (low_part & 0xFFFFFFFFULL);
        hi = new_hi;
    }
    std::string out(22, '0');
    for (int i = 21; i >= 0; --i) {
        out[i] = ALPHABET[static_cast<unsigned>(lo & 63)];
        lo = (lo >> 6) | (hi << 58);
        hi >>= 6;
    }
    return out;
}

struct Element {
    uint32_t id = 0;
    std::string guid, type, family;
    std::vector<uint32_t> leaves;       // body geometry items, through any mapping
    bool has_body = false;
    std::array<uint64_t, 3> items{}, faces{};
};

struct Leaf {
    uint32_t faces = 0;
    bool transparent = false;
};

class Model {
  public:
    std::unordered_map<uint32_t, std::vector<uint32_t>> representation_items, shape_representations;
    std::unordered_map<uint32_t, uint32_t> mapped_source, map_representation, element_shape;
    std::unordered_map<uint32_t, Leaf> leaves;
    std::unordered_map<uint32_t, Output> placed;  // leaves not in the federation file
    std::unordered_set<uint32_t> conflicts;
    std::vector<Element> elements;
    std::vector<bool> is_product, is_type, keeps_guid;  // by id
    std::vector<bool> is_root;
};

std::vector<uint32_t> ids_of(const Instances& instances) {
    std::vector<uint32_t> out;
    out.reserve(instances.size());
    for (const auto& i : instances) out.push_back(i.id());
    return out;
}

void gather_leaves(const Model& model, uint32_t representation, std::vector<uint32_t>& out) {
    const auto items = model.representation_items.find(representation);
    if (items == model.representation_items.end()) return;
    for (const uint32_t item : items->second) {
        const auto source = model.mapped_source.find(item);
        if (source == model.mapped_source.end()) {
            out.push_back(item);
            continue;
        }
        const auto mapped = model.map_representation.find(source->second);
        if (mapped != model.map_representation.end()) gather_leaves(model, mapped->second, out);
    }
}

// Plan area of a face set's bounding box, in the coordinates of its representation
double plan_area(const express::entity& item) {
    const auto coordinates = link(item, "Coordinates");
    const auto list = attribute(coordinates, "CoordList");
    if (!list) return 0;
    const auto points = static_cast<std::vector<std::vector<double>>>(*list);
    if (points.empty() || points[0].size() < 2) return 0;
    double x0 = points[0][0], x1 = x0, y0 = points[0][1], y1 = y0;
    for (const auto& p : points) {
        x0 = std::min(x0, p[0]);
        x1 = std::max(x1, p[0]);
        y0 = std::min(y0, p[1]);
        y1 = std::max(y1, p[1]);
    }
    return (x1 - x0) * (y1 - y0);
}

void classify(ifcopenshell::file& file, const Rules& rules, double threshold, uint32_t max_id, Model& model) {
    model.is_product.assign(max_id, false);
    model.is_type.assign(max_id, false);
    model.is_root.assign(max_id, false);
    model.keeps_guid.assign(max_id, false);
    for (const auto& i : all_of(file, "IfcRoot")) model.is_root[i.id()] = true;
    for (const auto& i : all_of(file, "IfcTypeObject")) model.is_type[i.id()] = true;
    for (const char* always : {"IfcProject", "IfcSpatialStructureElement"}) {
        for (const auto& i : all_of(file, always)) model.keeps_guid[i.id()] = true;
    }

    for (const char* type : {"IfcShapeModel"}) {
        for (const auto& i : all_of(file, type)) {
            model.representation_items[i.id()] = ids_of(links(i.as<express::entity>(), "Items"));
        }
    }
    for (const auto& i : all_of(file, "IfcProductRepresentation")) {
        model.shape_representations[i.id()] = ids_of(links(i.as<express::entity>(), "Representations"));
    }
    for (const auto& i : all_of(file, "IfcMappedItem")) {
        if (const auto source = link(i.as<express::entity>(), "MappingSource")) model.mapped_source[i.id()] = source.id();
    }
    for (const auto& i : all_of(file, "IfcRepresentationMap")) {
        if (const auto rep = link(i.as<express::entity>(), "MappedRepresentation")) model.map_representation[i.id()] = rep.id();
    }

    const auto transparency = item_transparency(file);
    const auto is_clear = [&](uint32_t id) {
        const auto t = transparency.find(id);
        return t != transparency.end() && t->second >= threshold;
    };

    std::unordered_map<uint32_t, Output> wanted;
    const auto want = [&](uint32_t leaf, Output output) {
        const auto [at, added] = wanted.emplace(leaf, output);
        if (!added && at->second != output) model.conflicts.insert(leaf);
    };

    for (const auto& instance : all_of(file, "IfcProduct")) {
        const auto product = instance.as<express::entity>();
        model.is_product[product.id()] = true;
        Element element;
        element.id = product.id();
        element.guid = text(product, "GlobalId");
        element.type = product.declaration().name();
        element.family = family_of(product);
        const auto shape = link(product, "Representation");
        if (shape) model.element_shape[product.id()] = shape.id();
        for (const auto& r : links(shape, "Representations")) {
            const auto representation = r.as<express::entity>();
            if (text(representation, "RepresentationIdentifier") != "Body") continue;
            element.has_body = true;
            gather_leaves(model, representation.id(), element.leaves);
        }

        // Openings cut things; they stay with what they cut
        const bool sorted = !product.declaration().is("IfcFeatureElement");
        const bool physical = Rules::matches(rules.physical, element.family);
        const bool envelope = Rules::matches(rules.envelope, element.family);
        const bool keep_largest = Rules::matches(rules.keep_largest, element.family);
        uint32_t largest = 0;
        double largest_area = -1;
        for (const uint32_t id : element.leaves) {
            auto [at, added] = model.leaves.emplace(id, Leaf{});
            const auto item = file.instance_by_id(static_cast<int>(id)).as<express::entity>();
            if (added) {
                at->second.transparent = is_clear(id);
                const auto& type = item.declaration();
                if (type.is("IfcPolygonalFaceSet")) at->second.faces = static_cast<uint32_t>(links(item, "Faces").size());
                else if (type.is("IfcTriangulatedFaceSet")) {
                    if (const auto indices = attribute(item, "CoordIndex")) at->second.faces = static_cast<uint32_t>(indices->size());
                }
            }
            if (keep_largest && !at->second.transparent) {
                const double area = plan_area(item);
                if (area > largest_area) {
                    largest_area = area;
                    largest = id;
                }
            }
        }
        for (const uint32_t id : element.leaves) {
            if (!sorted) want(id, FEDERATION);
            else if (envelope || (model.leaves[id].transparent && !physical)) want(id, ENVELOPES);
            else if (keep_largest && largest && id != largest) want(id, HIDDEN);
            else want(id, FEDERATION);
        }
        model.elements.push_back(std::move(element));
    }
    for (const auto& [leaf, output] : wanted) {
        if (output != FEDERATION && !model.conflicts.count(leaf)) model.placed[leaf] = output;
    }
}

// What each output doesn't have: geometry that went elsewhere, and everything left empty by that
std::vector<bool> gone_from(const Model& model, Output output, uint32_t max_id, const std::vector<bool>& in_output) {
    std::vector<bool> gone(max_id, false);
    const auto output_of = [&](uint32_t leaf) {
        const auto at = model.placed.find(leaf);
        return at == model.placed.end() ? FEDERATION : at->second;
    };
    std::unordered_map<uint32_t, bool> memo;
    const std::function<bool(uint32_t)> has_any = [&](uint32_t representation) -> bool {
        const auto seen = memo.find(representation);
        if (seen != memo.end()) return seen->second;
        memo[representation] = false;
        bool any = false;
        for (const uint32_t item : model.representation_items.at(representation)) {
            const auto source = model.mapped_source.find(item);
            if (source == model.mapped_source.end()) {
                const bool here = output_of(item) == output;
                any = any || here;
                if (!here) gone[item] = true;
                continue;
            }
            const auto mapped = model.map_representation.find(source->second);
            const bool here = mapped != model.map_representation.end() && model.representation_items.count(mapped->second) && has_any(mapped->second);
            any = any || here;
            if (!here) gone[item] = true;
        }
        memo[representation] = any;
        if (!any) gone[representation] = true;
        return any;
    };
    for (const auto& [representation, items] : model.representation_items) has_any(representation);
    for (const auto& [map, representation] : model.map_representation) {
        if (gone[representation]) gone[map] = true;
    }
    for (const auto& [shape, representations] : model.shape_representations) {
        if (!representations.empty() && std::all_of(representations.begin(), representations.end(), [&](uint32_t r) { return gone[r]; })) gone[shape] = true;
    }
    for (const auto& element : model.elements) {
        if (!in_output[element.id]) gone[element.id] = true;
    }
    return gone;
}

enum class Kind : uint8_t { Plain, Relationship, Follower, Layer };

// Instances that nothing points to, because they point at what they apply to
Kind kind_of(std::string_view type) {
    if (type.rfind("IFCREL", 0) == 0) return Kind::Relationship;
    if (type == "IFCPRESENTATIONLAYERASSIGNMENT" || type == "IFCPRESENTATIONLAYERWITHSTYLE") return Kind::Layer;
    if (type == "IFCSTYLEDITEM" || type == "IFCINDEXEDCOLOURMAP" || type == "IFCINDEXEDTRIANGLETEXTUREMAP" || type == "IFCINDEXEDPOLYGONALTEXTUREMAP" ||
        type == "IFCMATERIALDEFINITIONREPRESENTATION" || type == "IFCSHAPEASPECT") {
        return Kind::Follower;
    }
    return Kind::Plain;
}

class Writer {
  public:
    Writer(const SpfFile& spf, const Model& model, Output output, std::vector<bool> gone)
        : spf_(spf), model_(model), output_(output), gone_(std::move(gone)), included_(spf.max_id(), false) {}

    WhenEmpty policy(uint32_t id) const { return model_.is_product[id] || model_.is_type[id] ? WhenEmpty::Null : WhenEmpty::Drop; }

    void include(uint32_t root) {
        std::vector<uint32_t> stack{root};
        std::string text;
        while (!stack.empty()) {
            const uint32_t id = stack.back();
            stack.pop_back();
            if (id >= included_.size() || included_[id] || gone_[id] || spf_.line(id).empty()) continue;
            included_[id] = true;
            for_each_ref(spf_.line(id), [&](uint32_t next) {
                if (next < included_.size() && !included_[next] && !gone_[next]) stack.push_back(next);
            });
        }
    }

    bool included(uint32_t id) const { return id < included_.size() && included_[id]; }

    // Elements and the things hanging off them
    void close_over(const std::vector<uint32_t>& relationships, const std::vector<uint32_t>& followers) {
        std::string text;
        const auto is_gone = [&](uint32_t id) { return id < gone_.size() && gone_[id]; };
        for (const uint32_t id : relationships) {
            if (!rewrite(spf_.line(id), is_gone, WhenEmpty::Drop, text)) continue;
            uint64_t elements = 0;
            for_each_ref(text, [&](uint32_t next) { elements += next < model_.is_product.size() && model_.is_product[next]; });
            if (output_ == FEDERATION || elements) include(id);
        }
        // A follower can only be reached from what it points at; it needs that here
        for (int pass = 0; pass < 2; ++pass) {
            for (const uint32_t id : followers) {
                if (included_[id] || !rewrite(spf_.line(id), is_gone, WhenEmpty::Drop, text)) continue;
                bool applies = false;
                for_each_ref(text, [&](uint32_t next) { applies = applies || included(next); });
                if (applies && spf_.type(id) != "IFCSHAPEASPECT") include(id);
                else if (spf_.type(id) == "IFCSHAPEASPECT") {
                    // Its product definition is its last reference
                    uint32_t last = 0;
                    for_each_ref(text, [&](uint32_t next) { last = next; });
                    if (included(last)) include(id);
                }
            }
        }
    }

    void write(const std::string& path, const std::vector<uint32_t>& layers) {
        std::ofstream out(path, std::ios::binary);
        out << spf_.header();
        std::string text;
        const auto is_gone = [&](uint32_t id) { return id < gone_.size() && gone_[id]; };
        const auto not_here = [&](uint32_t id) { return !included(id); };
        const std::unordered_set<uint32_t> layer_set(layers.begin(), layers.end());
        for (uint32_t id = 0; id < spf_.max_id(); ++id) {
            if (layer_set.count(id)) {
                if (rewrite(spf_.line(id), not_here, WhenEmpty::Drop, text)) out << text << '\n';
                continue;
            }
            if (!included_[id]) continue;
            if (!rewrite(spf_.line(id), is_gone, policy(id), text)) {
                ++dropped_;
                continue;
            }
            if (output_ != FEDERATION && model_.is_root[id] && !model_.keeps_guid[id] && !whole_.count(id)) {
                const auto open = text.find('\'');
                const auto close = text.find('\'', open + 1);
                if (open != std::string::npos && close != std::string::npos) {
                    const std::string guid = text.substr(open + 1, close - open - 1);
                    text.replace(open + 1, close - open - 1, derived_guid(guid + "/" + OUTPUT_NAMES[output_]));
                }
            }
            out << text << '\n';
        }
        out << "ENDSEC;\nEND-ISO-10303-21;\n";
    }

    std::unordered_set<uint32_t>& whole() { return whole_; }  // elements here and nowhere else keep their GUID
    uint64_t dropped() const { return dropped_; }
    const std::vector<bool>& included_all() const { return included_; }

  private:
    const SpfFile& spf_;
    const Model& model_;
    const Output output_;
    std::vector<bool> gone_;
    std::vector<bool> included_;
    std::unordered_set<uint32_t> whole_;
    uint64_t dropped_ = 0;
};

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

}  // namespace

int split(const SplitOptions& options) {
    namespace fs = std::filesystem;
    const fs::path input(options.ifc_path);
    const std::string stem = input.stem().string();
    const auto output_path = [&](Output output) { return fs::path(options.out_dir) / FOLDERS[output] / (stem + SUFFIXES[output]); };
    for (const Output output : {FEDERATION, HIDDEN, ENVELOPES}) {
        const auto target = output_path(output);
        fs::create_directories(target.parent_path());
        if (fs::exists(target) && fs::equivalent(target, input)) {
            std::cerr << target << " is the input: choose another output folder\n";
            return 2;
        }
    }

    const Rules rules = Rules::load(options.rules_path);
    const SpfFile spf(options.ifc_path);
    const uint32_t max_id = spf.max_id();
    Model model;
    std::vector<uint32_t> always;
    {
        // The parser is only for working out what goes where, and is the biggest user of memory
        auto file = std::make_unique<ifcopenshell::file>(options.ifc_path);
        if (!file->good()) {
            std::cerr << options.ifc_path << ": can't be read as IFC\n";
            return 1;
        }
        if (file->schema()->name() != "IFC4") {
            std::cerr << options.ifc_path << ": warning: schema is " << file->schema()->name() << ", and is written out as it is, not converted to IFC4\n";
        }
        classify(*file, rules, options.transparent, max_id, model);
        for (const char* type : {"IfcProject", "IfcSpatialStructureElement"}) {
            for (const auto& i : all_of(*file, type)) always.push_back(i.id());
        }
    }

    const auto output_of = [&](uint32_t leaf) {
        const auto at = model.placed.find(leaf);
        return at == model.placed.end() ? FEDERATION : at->second;
    };
    std::array<std::vector<bool>, 3> in_output;
    for (auto& v : in_output) v.assign(max_id, false);
    for (auto& element : model.elements) {
        for (const uint32_t leaf : element.leaves) {
            const Output output = output_of(leaf);
            ++element.items[output];
            element.faces[output] += model.leaves.at(leaf).faces;
        }
        const bool spatial = std::find(always.begin(), always.end(), element.id) != always.end();
        const uint64_t placed = element.items[HIDDEN] + element.items[ENVELOPES];
        // Only an element with no body left is off the federation file
        in_output[FEDERATION][element.id] = spatial || !element.has_body || element.items[FEDERATION] || !placed;
        in_output[HIDDEN][element.id] = spatial || element.items[HIDDEN];
        in_output[ENVELOPES][element.id] = spatial || element.items[ENVELOPES];
    }

    std::vector<uint32_t> relationships, followers, layers;
    for (uint32_t id = 0; id < max_id; ++id) {
        if (spf.line(id).empty()) continue;
        switch (kind_of(spf.type(id))) {
            case Kind::Relationship: relationships.push_back(id); break;
            case Kind::Follower: followers.push_back(id); break;
            case Kind::Layer: layers.push_back(id); break;
            case Kind::Plain: break;
        }
    }

    std::array<std::unique_ptr<Writer>, 3> writers;
    // The federation file goes last, as it also takes whatever nothing else reached
    for (const Output output : {HIDDEN, ENVELOPES, FEDERATION}) {
        writers[output] = std::make_unique<Writer>(spf, model, output, gone_from(model, output, max_id, in_output[output]));
        Writer& writer = *writers[output];
        for (const uint32_t id : always) writer.include(id);
        for (const auto& element : model.elements) {
            if (!in_output[output][element.id]) continue;
            writer.include(element.id);
            const uint32_t others = in_output[(output + 1) % 3][element.id] + in_output[(output + 2) % 3][element.id];
            if (!others) writer.whole().insert(element.id);
        }
        writer.close_over(relationships, followers);
    }
    Writer& federation = *writers[FEDERATION];
    uint64_t unreached = 0;
    const std::unordered_set<uint32_t> layer_set(layers.begin(), layers.end());
    for (uint32_t id = 0; id < max_id; ++id) {
        if (spf.line(id).empty() || layer_set.count(id)) continue;
        if (!writers[HIDDEN]->included(id) && !writers[ENVELOPES]->included(id) && !federation.included(id)) {
            // Rels and followers were already weighed; what's left is unused types, materials and the like
            const auto kind = kind_of(spf.type(id));
            if (kind == Kind::Plain) {
                federation.include(id);
                ++unreached;
            }
        }
    }

    // The input beside its outputs, to compare in a viewer: a link, so it costs nothing and can't be written to by mistake
    const auto original = fs::path(options.out_dir) / (stem + ".original.ifc");
    fs::remove(original);
    std::error_code no_link;
    fs::create_symlink(fs::absolute(input), original, no_link);  // Windows only allows this in developer mode
    if (no_link) std::cerr << "note: no " << original.filename().string() << " link: " << no_link.message() << "\n";

    std::array<uint64_t, 3> sizes{};
    std::array<CompactStats, 3> compacted{};
    for (const Output output : {FEDERATION, HIDDEN, ENVELOPES}) {
        const auto path = output_path(output);
        writers[output]->write(path.string(), layers);
        if (options.compact) compacted[output] = compact(path.string(), options.precision_mm);
        if (!options.zip) {
            sizes[output] = fs::file_size(path);
            continue;
        }
        const auto zipped = fs::path(path.string() + ZIPPED);
        zip_file(path.string(), zipped.string(), path.filename().string());
        fs::remove(path);
        sizes[output] = fs::file_size(zipped);
    }

    std::ofstream manifest(fs::path(options.out_dir) / (stem + ".manifest.csv"));
    manifest << "guid,class,family,output,output_guid,items,faces,action,reason\n";
    std::array<uint64_t, 3> element_counts{}, item_counts{}, face_counts{};
    uint64_t split_elements = 0, moved_elements = 0;
    for (const auto& element : model.elements) {
        const int places = in_output[FEDERATION][element.id] + in_output[HIDDEN][element.id] + in_output[ENVELOPES][element.id];
        const bool spatial = std::find(always.begin(), always.end(), element.id) != always.end();
        if (!spatial && places > 1) ++split_elements;
        if (!spatial && !in_output[FEDERATION][element.id]) ++moved_elements;
        for (const Output output : {FEDERATION, HIDDEN, ENVELOPES}) {
            if (!in_output[output][element.id]) continue;
            ++element_counts[output];
            item_counts[output] += element.items[output];
            face_counts[output] += element.faces[output];
            if (spatial && output != FEDERATION) continue;
            const bool whole = places == 1;
            const std::string guid = (output == FEDERATION || whole) ? element.guid : derived_guid(element.guid + "/" + OUTPUT_NAMES[output]);
            const char* reason = output == HIDDEN ? "keep-largest rule"
                : output != ENVELOPES ? ""
                : Rules::matches(rules.envelope, element.family) ? "envelope rule" : "transparent";
            manifest << element.guid << ',' << element.type << ',' << quoted(element.family) << ',' << OUTPUT_NAMES[output] << ',' << guid << ','
                     << element.items[output] << ',' << element.faces[output] << ',' << (whole ? (output == FEDERATION ? "kept" : "moved") : "split") << ',' << reason << '\n';
        }
    }

    const auto mb = [](uint64_t bytes) { return bytes / 1048576.0; };
    std::cout << std::fixed << std::setprecision(1) << input.filename().string() << ": " << mb(fs::file_size(input)) << " MB\n";
    for (const Output output : {FEDERATION, HIDDEN, ENVELOPES}) {
        std::cout << std::setw(12) << OUTPUT_NAMES[output] << std::setw(9) << mb(sizes[output]) << " MB " << std::setw(7) << element_counts[output] << " elements "
                  << std::setw(8) << item_counts[output] << " items " << std::setw(9) << face_counts[output] << " faces\n";
    }
    std::cout << "  " << split_elements << " elements split, " << moved_elements << " moved whole, " << model.conflicts.size()
              << " shared items wanted in two places (left in the federation file), " << unreached << " unused instances kept in the federation file\n";
    if (options.compact) {
        const CompactStats& federated = compacted[FEDERATION];
        std::cout << "  federate written compactly: " << mb(federated.bytes_before) << " -> " << mb(federated.bytes_after) << " MB, "
                  << federated.instances_before << " -> " << federated.instances_after << " instances, " << federated.sets_triangulated << " face sets as triangles, "
                  << federated.sets_left << " left as polygons (" << federated.faces_left << " faces), "
                  << federated.no_area_removed << " faces with no area removed\n";
        if (federated.no_area_left) std::cout << "  warning: " << federated.no_area_left << " faces with no area kept, as all their set had\n";
        if (!federated.rounded) std::cout << "  warning: the model's length unit isn't plain SI, so coordinates weren't rounded\n";
    }
    uint64_t dropped = 0;
    for (const auto& writer : writers) dropped += writer->dropped();
    if (dropped) std::cout << "  warning: " << dropped << " lines couldn't be written without what they refer to\n";
    return 0;
}

}  // namespace bimpass

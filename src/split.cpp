#include "split.h"

#include "breps.h"
#include "combine.h"
#include "compact.h"
#include "extent.h"
#include "signs.h"
#include "simplify.h"
#include "ifc_access.h"
#include "instance.h"
#include "mapping.h"
#include "merge.h"
#include "progress.h"
#include "register.h"
#include "site.h"
#include "spf.h"
#include "system.h"
#include "tidy.h"
#include "trace.h"
#include "upgrade.h"
#include "zip.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifcfilter {
namespace fs = std::filesystem;
namespace {

enum Output : uint8_t { FEDERATION = 0, HIDDEN = 1, ENVELOPES = 2 };
constexpr std::array<const char*, 3> OUTPUT_NAMES = {"federate", "hidden", "envelopes"};
constexpr std::array<const char*, 3> SUFFIXES = {".federate.ifc", ".hidden.ifc", ".envelopes.ifc"};
constexpr std::array<const char*, 3> FOLDERS = {"federate", "non-federate", "non-federate"};
constexpr const char* ZIPPED = "zip";  // added to an .ifc, makes .ifczip

// Elements with this value of this property go to this output
struct PropertyRule {
    std::string set, property, value;
    Output output = FEDERATION;
};

std::string trimmed(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

// A family matches a rule if its name holds the rule's text
struct Rules {
    std::vector<std::string> keep_largest;  // the largest solid is the thing; the rest is detail inside it
    std::vector<std::string> physical;      // transparent, but really there (gratings)
    std::vector<std::string> envelope;      // opaque, but not really there (clearance envelopes)
    std::vector<std::string> hidden;        // really there, but out of sight (buried piles)
    std::vector<PropertyRule> by_property;  // for models where the family doesn't say what a thing is
    std::vector<ForwardRule> forward;       // assemblies moved forward, such as sign faces off their posts
    std::vector<PropertyRule> small_parts;  // assemblies whose small parts are hidden, such as a sign's badly exported text
    double cull_at_origin = 0;              // metres: a body this close to the world's 0,0, in a model that's mostly far from it, is left out

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
            if (schema_rule(action, family)) continue;
            else if (action == "keep-largest") rules.keep_largest.push_back(family);
            else if (action == "physical") rules.physical.push_back(family);
            else if (action == "envelope") rules.envelope.push_back(family);
            else if (action == "hidden") rules.hidden.push_back(family);
            else if (action == "cull-at-origin") rules.cull_at_origin = std::stod(family);
            else if (action == "forward-if") {
                // SET.PROPERTY=VALUE MM, the distance last
                const auto last = family.rfind(' ');
                const auto dot = family.find('.');
                const auto equals = family.find('=');
                if (dot == std::string::npos || equals == std::string::npos || equals < dot || last == std::string::npos || last < equals) {
                    throw std::runtime_error(path + ": forward-if needs SET.PROPERTY=VALUE MM: " + family);
                }
                rules.forward.push_back({family.substr(0, dot), family.substr(dot + 1, equals - dot - 1), trimmed(family.substr(equals + 1, last - equals - 1)),
                                         std::stod(family.substr(last + 1))});
            }
            else if (action == "hidden-small-parts-if") {
                const auto dot = family.find('.');
                const auto equals = family.find('=');
                if (dot == std::string::npos || equals == std::string::npos || equals < dot) throw std::runtime_error(path + ": " + action + " needs SET.PROPERTY=VALUE: " + family);
                rules.small_parts.push_back({family.substr(0, dot), family.substr(dot + 1, equals - dot - 1), trimmed(family.substr(equals + 1)), HIDDEN});
            }
            else if (action == "hidden-if" || action == "envelope-if") {
                const auto dot = family.find('.');
                const auto equals = family.find('=');
                if (dot == std::string::npos || equals == std::string::npos || equals < dot) throw std::runtime_error(path + ": " + action + " needs SET.PROPERTY=VALUE: " + family);
                rules.by_property.push_back({family.substr(0, dot), family.substr(dot + 1, equals - dot - 1), trimmed(family.substr(equals + 1)), action == "hidden-if" ? HIDDEN : ENVELOPES});
            }
            else throw std::runtime_error(path + ": unknown rule " + action);
        }
        return rules;
    }
};

// A line's first attribute, if it's a string: for a root, that's its GlobalId
std::string_view guid_in(std::string_view line) {
    auto open = line.find('(');
    if (open == std::string_view::npos) return {};
    open = line.find_first_not_of(' ', open + 1);
    if (open == std::string_view::npos || line[open] != '\'') return {};
    const auto close = line.find('\'', open + 1);
    return close == std::string_view::npos ? std::string_view() : line.substr(open + 1, close - open - 1);
}

struct Element {
    uint32_t id = 0;
    std::string guid, type, family;
    std::vector<uint32_t> leaves;       // body geometry items, through any mapping
    bool has_body = false;
    bool feature = false;               // an opening, which stays with what it cuts
    bool by_property = false;           // a property rule says where it goes
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
    std::vector<bool> is_product, is_type;  // by id
    std::vector<bool> is_root;
    std::unordered_map<uint32_t, std::string> renamed;  // roots whose GUID isn't unique, and what they have instead
    uint64_t small_parts = 0, small_part_assemblies = 0;  // hidden by hidden-small-parts-if, and the assemblies they're of

    // A root's GUID in an output. What's in one output only is the thing itself, and keeps its GUID.
    // What's in several keeps it in the federation file, and elsewhere has one made from it.
    std::string guid_in_output(uint32_t id, std::string_view guid, Output output, bool whole) const {
        const auto at = renamed.find(id);
        const std::string unique = at == renamed.end() ? std::string(guid) : at->second;
        return output == FEDERATION || whole ? unique : derived_guid(unique + "/" + OUTPUT_NAMES[output]);
    }
};

// GUIDs have to be unique across everything a run writes. One that's in other models too changes in
// all of them, so that no model's result depends on the order they're given in. One that's repeated
// inside a model stays on its first root.
void rename_repeats(const SpfFile& spf, const std::string& stem, const std::unordered_set<std::string>& shared, Model& model) {
    std::unordered_set<std::string_view> seen;
    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        if (!model.is_root[id]) continue;
        const auto guid = guid_in(spf.line(id));
        if (guid.empty()) continue;
        const bool repeated = !seen.insert(guid).second;
        const bool elsewhere = shared.count(std::string(guid)) > 0;
        if (!repeated && !elsewhere) continue;
        std::string name(guid);
        if (elsewhere) name += "@" + stem;
        if (repeated) name += "#" + std::to_string(id);
        model.renamed[id] = derived_guid(name);
    }
}

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

void classify(ifcopenshell::file& file, const SpfFile& spf, const Rules& rules, double threshold, uint32_t max_id, Model& model) {
    model.is_product.assign(max_id, false);
    model.is_type.assign(max_id, false);
    model.is_root.assign(max_id, false);
    for (const auto& i : all_of(file, "IfcRoot")) model.is_root[i.id()] = true;
    for (const auto& i : all_of(file, "IfcTypeObject")) model.is_type[i.id()] = true;

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

    std::unordered_map<uint32_t, Output> by_property;
    if (!rules.by_property.empty()) {
        for (const auto& i : all_of(file, "IfcRelDefinesByProperties")) {
            const auto relationship = i.as<express::entity>();
            const auto set = link(relationship, "RelatingPropertyDefinition");
            if (!set || !set.declaration().is("IfcPropertySet")) continue;
            const std::string name = text(set, "Name");
            for (const PropertyRule& rule : rules.by_property) {
                if (rule.set != name) continue;
                for (const auto& p : links(set, "HasProperties")) {
                    const auto property = p.as<express::entity>();
                    if (text(property, "Name") != rule.property || trimmed(nominal_text(property)) != rule.value) continue;
                    for (const auto& object : links(relationship, "RelatedObjects")) by_property[object.id()] = rule.output;
                }
            }
        }
    }

    // An assembly a rule picks out keeps its big parts, such as a sign's plate, border and symbols, and its parts
    // under a quarter of its size, such as the letters of text exported as broken meshes, are hidden
    if (!rules.small_parts.empty()) {
        std::unordered_set<uint32_t> picked;
        for (const auto& i : all_of(file, "IfcRelDefinesByProperties")) {
            const auto relationship = i.as<express::entity>();
            const auto set = link(relationship, "RelatingPropertyDefinition");
            if (!set || !set.declaration().is("IfcPropertySet")) continue;
            const std::string name = text(set, "Name");
            for (const PropertyRule& rule : rules.small_parts) {
                if (rule.set != name) continue;
                for (const auto& p : links(set, "HasProperties")) {
                    const auto property = p.as<express::entity>();
                    if (text(property, "Name") != rule.property || trimmed(nominal_text(property)) != rule.value) continue;
                    for (const auto& object : links(relationship, "RelatedObjects")) picked.insert(object.id());
                }
            }
        }
        Extents extents(spf);
        const auto size_of = [](const Box& box) {
            double size = 0;
            for (int axis = 0; axis < 3; ++axis) size = std::max(size, box.high[axis] - box.low[axis]);
            return size;
        };
        for (const auto& i : all_of(file, "IfcRelAggregates")) {
            const auto relationship = i.as<express::entity>();
            const auto whole = link(relationship, "RelatingObject");
            if (!whole || !picked.count(whole.id())) continue;
            std::vector<std::pair<uint32_t, double>> parts;
            Box all;
            for (const auto& p : links(relationship, "RelatedObjects")) {
                const auto box = extents.product(p.id());
                if (!box || box->empty()) continue;
                all.add(*box);
                parts.emplace_back(p.id(), size_of(*box));
            }
            if (all.empty()) continue;
            const double whole_size = size_of(all);
            uint64_t hidden = 0;
            for (const auto& [part, size] : parts) {
                if (size >= whole_size / 4 || by_property.count(part)) continue;
                by_property[part] = HIDDEN;
                ++hidden;
            }
            model.small_parts += hidden;
            if (hidden) ++model.small_part_assemblies;
        }
    }

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
        element.feature = !sorted;
        const bool physical = Rules::matches(rules.physical, element.family);
        const auto ruled = by_property.find(product.id());
        element.by_property = ruled != by_property.end();
        const bool envelope = Rules::matches(rules.envelope, element.family) || (element.by_property && ruled->second == ENVELOPES);
        const bool hidden = Rules::matches(rules.hidden, element.family) || (element.by_property && ruled->second == HIDDEN);
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
            else if (hidden) want(id, HIDDEN);
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

    void add_lines(std::vector<std::string> lines) { added_ = std::move(lines); }
    void replace_lines(std::unordered_map<uint32_t, std::string> lines) { replaced_ = std::move(lines); }  // an empty line leaves one out

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
        // A follower can only be reached from what it points at; it needs that here. A style or colour map goes
        // with the shape it's on, not with its colours: a merged asset's moved shapes share those with the shapes
        // they were moved from, which aren't here.
        for (int pass = 0; pass < 2; ++pass) {
            for (const uint32_t id : followers) {
                if (included_[id] || !rewrite(spf_.line(id), is_gone, WhenEmpty::Drop, text)) continue;
                const auto type = spf_.type(id);
                const size_t on = type == "IFCSTYLEDITEM" || type == "IFCINDEXEDCOLOURMAP" ? 0
                                  : type == "IFCINDEXEDTRIANGLETEXTUREMAP" || type == "IFCINDEXEDPOLYGONALTEXTUREMAP" ? 1
                                                                                                                        : SIZE_MAX;
                if (on != SIZE_MAX) {
                    const auto attributes = attributes_of(spf_.line(id));
                    const auto shape = attributes.size() > on ? refs_in(attributes[on]) : std::vector<uint32_t>{};
                    if (shape.size() == 1) {
                        if (included(shape[0])) include(id);
                        continue;
                    }
                }
                bool applies = false;
                for_each_ref(text, [&](uint32_t next) { applies = applies || included(next); });
                if (applies && type != "IFCSHAPEASPECT") include(id);
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
            std::string_view line = spf_.line(id);
            const auto replaced = replaced_.find(id);
            if (replaced != replaced_.end()) {
                if (replaced->second.empty()) continue;
                line = replaced->second;
            }
            if (!rewrite(line, is_gone, policy(id), text)) {
                ++dropped_;
                continue;
            }
            if (model_.is_root[id]) {
                const auto guid = guid_in(text);
                if (!guid.empty()) {
                    const size_t at = static_cast<size_t>(guid.data() - text.data());
                    text.replace(at, guid.size(), model_.guid_in_output(id, guid, output_, whole_.count(id) > 0));
                }
            }
            out << text << '\n';
        }
        for (const std::string& line : added_) out << line << '\n';
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
    std::vector<std::string> added_;  // new instances, for elements that have been merged
    std::unordered_map<uint32_t, std::string> replaced_;  // lines written differently
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

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The models to work on: files as given, and the .ifc files directly in any folder given
std::vector<fs::path> models_in(const std::vector<std::string>& inputs) {
    std::vector<fs::path> models;
    for (const std::string& input : inputs) {
        if (!fs::is_directory(input)) {
            models.emplace_back(input);
            continue;
        }
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(input)) {
            if (entry.is_regular_file() && lowercase(entry.path().extension().string()) == ".ifc") found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end());
        models.insert(models.end(), found.begin(), found.end());
    }
    return models;
}

// GUIDs in more than one model. This goes by text, before anything is parsed, so it also picks up other
// strings the length of a GUID; that's harmless, as only a root's GUID is ever looked up in the result.
struct SharedGuids {
    std::unordered_set<std::string> guids;
    std::vector<std::string> by_model;  // a hash of each model's GUIDs that are in another model too
};

SharedGuids guids_in_several(const std::vector<fs::path>& models, unsigned jobs) {
    std::vector<std::vector<std::string>> found(models.size());
    std::vector<std::string> errors(models.size());
    std::atomic<size_t> next{0};
    const auto read_models = [&]() {
        for (size_t model = next++; model < models.size(); model = next++) {
            try {
                const SpfFile spf(models[model].string());
                for (uint32_t id = 0; id < spf.max_id(); ++id) {
                    const auto guid = guid_in(spf.line(id));
                    if (guid.size() == 22) found[model].emplace_back(guid);
                }
            } catch (const std::exception& e) {
                errors[model] = e.what();
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned i = 1; i < jobs; ++i) threads.emplace_back(read_models);
    read_models();
    for (auto& thread : threads) thread.join();
    for (const std::string& error : errors) {
        if (!error.empty()) throw std::runtime_error(error);
    }

    std::unordered_map<std::string, size_t> first_model;
    SharedGuids out;
    for (size_t model = 0; model < models.size(); ++model) {
        for (const std::string& guid : found[model]) {
            const auto at = first_model.find(guid);
            if (at == first_model.end()) first_model.emplace(guid, model);
            else if (at->second != model) out.guids.insert(guid);
        }
    }
    for (size_t model = 0; model < models.size(); ++model) {
        std::vector<std::string> mine;
        for (std::string& guid : found[model]) {
            if (out.guids.count(guid)) mine.push_back(std::move(guid));
        }
        found[model] = {};
        std::sort(mine.begin(), mine.end());
        mine.erase(std::unique(mine.begin(), mine.end()), mine.end());
        std::string joined;
        for (const std::string& guid : mine) joined.append(guid).append(1, '\n');
        out.by_model.push_back(sha256_of(joined));
    }
    return out;
}

// A model that's combined with others, by the register, has its federation file here, as a part of what's in
// federate, not in federate itself, so that it's only federated once
constexpr const char* PARTS = "parts";

// The register's groups, read once for a process
const std::map<std::string, std::string>& groups_of(const SplitOptions& options) {
    static const std::map<std::string, std::string> groups = options.combine_path.empty() ? std::map<std::string, std::string>{} : combine_groups(options.combine_path);
    return groups;
}

// Where a model's federation file goes, and where it would go otherwise
std::pair<fs::path, fs::path> federation_file(const fs::path& model, const SplitOptions& options) {
    const std::string stem = model.stem().string();
    const std::string discipline = fs::absolute(model).parent_path().filename().string();
    const auto federate = fs::path(options.out_dir) / FOLDERS[FEDERATION] / discipline / (stem + SUFFIXES[FEDERATION]);
    const auto part = fs::path(options.out_dir) / PARTS / discipline / (stem + SUFFIXES[FEDERATION]);
    return groups_of(options).count(stem) ? std::make_pair(part, federate) : std::make_pair(federate, part);
}

// Where a model's outputs go
std::vector<fs::path> outputs_of(const fs::path& model, const SplitOptions& options) {
    const std::string stem = model.stem().string();
    const auto out = fs::path(options.out_dir);
    const std::string zipped = options.zip ? ZIPPED : "";
    return {federation_file(model, options).first, out / FOLDERS[HIDDEN] / (stem + SUFFIXES[HIDDEN] + zipped),
            out / FOLDERS[ENVELOPES] / (stem + SUFFIXES[ENVELOPES] + zipped), out / "manifest" / (stem + ".manifest.csv")};
}

// What a model's outputs depend on, apart from the model and the others in the run: this program, its rules
// and mapping, and the options that change what it writes
std::string build_of(const SplitOptions& options) {
    std::ostringstream key;
    key.precision(17);
    key << sha256_of_file(program_path()) << '|' << (options.rules_path.empty() ? "" : sha256_of_file(options.rules_path)) << '|'
        << (options.mapping_path.empty() ? "" : sha256_of_file(options.mapping_path)) << '|' << options.transparent << '|' << options.merge << '|' << options.instance << '|'
        << options.compact << '|' << options.precision_mm << '|' << options.merge_gap_m << '|' << options.zip << '|' << options.simplify_mm;
    return sha256_of(key.str());
}

int split_model(const fs::path& input, const SplitOptions& options, const Rules& rules, const std::unordered_set<std::string>& shared) {
    const std::string stem = input.stem().string();
    // Federation files go in a folder for their discipline, named after the folder the input is in, as there
    // are hundreds of them
    const std::string discipline = fs::absolute(input).parent_path().filename().string();
    const auto output_path = [&](Output output) {
        if (output == FEDERATION && !discipline.empty()) return federation_file(input, options).first;
        return fs::path(options.out_dir) / FOLDERS[output] / (stem + SUFFIXES[output]);
    };
    // Where a run before discipline folders put the federation file
    const auto flat = fs::path(options.out_dir) / FOLDERS[FEDERATION] / (stem + SUFFIXES[FEDERATION]);
    for (const Output output : {FEDERATION, HIDDEN, ENVELOPES}) {
        const auto target = output_path(output);
        fs::create_directories(target.parent_path());
        if (fs::exists(target) && fs::equivalent(target, input)) {
            std::cerr << target << " is the input: choose another output folder\n";
            return 2;
        }
    }

    // An IFC2X3 model goes through as an IFC4 copy, which goes when this is done: it's before the
    // file below, as Windows can't remove a file that's still open
    struct Temporary {
        fs::path path;
        ~Temporary() {
            if (path.empty()) return;
            std::error_code ignored;
            fs::remove(path, ignored);
            fs::remove(path.parent_path(), ignored);  // only goes if nothing else is in it
        }
    } upgraded;
    fs::path source = input;
    Upgrade upgrade;
    std::string why_not_ifc4 = "only IFC2X3 can be converted";
    if (schema_in_header(input).rfind("IFC2X3", 0) == 0) {
        upgraded.path = fs::path(options.out_dir) / ".work" / (stem + ".ifc4.ifc");
        fs::create_directories(upgraded.path.parent_path());
        upgrade = upgrade_to_ifc4(SpfFile(input.string()), upgraded.path);
        if (upgrade.refused.empty()) {
            source = upgraded.path;
        } else {
            why_not_ifc4 = "";
            for (const std::string& reason : upgrade.refused) why_not_ifc4 += (why_not_ifc4.empty() ? "" : "; ") + reason;
        }
    }

    // Sign faces and the like moved forward, in a copy that's read from here on
    Forward forward;
    struct Moved {
        fs::path path;
        ~Moved() {
            if (path.empty()) return;
            std::error_code ignored;
            fs::remove(path, ignored);
            fs::remove(path.parent_path(), ignored);
        }
    } moved;
    if (!rules.forward.empty()) {
        const fs::path path = fs::path(options.out_dir) / ".work" / (stem + ".forward.ifc");
        fs::create_directories(path.parent_path());
        forward = move_forward(SpfFile(source.string()), rules.forward, path.string());
        if (forward.faces) {
            moved.path = path;
            source = path;
        }
    }

    const SpfFile spf(source.string());
    const uint32_t max_id = spf.max_id();
    Model model;
    std::vector<uint32_t> always;
    {
        // The parser is only for working out what goes where, and is the biggest user of memory
        auto file = std::make_unique<ifcopenshell::file>(source.string());
        if (!file->good()) {
            std::cerr << input.string() << ": can't be read as IFC\n";
            return 1;
        }
        // IFC4 and what came after it go through as they are; only what came before is converted
        if (file->schema()->name().rfind("IFC4", 0) != 0) {
            std::cerr << input.string() << ": warning: schema is " << file->schema()->name() << ", and is written out as it is: " << why_not_ifc4 << "\n";
        }
        classify(*file, spf, rules, options.transparent, max_id, model);
        for (const char* type : {"IfcProject", "IfcSpatialStructureElement"}) {
            for (const auto& i : all_of(*file, type)) always.push_back(i.id());
        }
    }
    rename_repeats(spf, stem, shared, model);

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
    // A port has no body of its own: it goes where what it's on goes, and isn't left behind without it. One
    // that's on nothing, as some exporters leave, is left out.
    for (const auto& element : model.elements) {
        if (element.type != "IfcDistributionPort") continue;
        for (auto& v : in_output) v[element.id] = false;
    }
    for (uint32_t id = 0; id < max_id; ++id) {
        const auto type = spf.type(id);
        if (type != "IFCRELNESTS" && type != "IFCRELCONNECTSPORTTOELEMENT") continue;
        const auto attributes = attributes_of(spf.line(id));
        if (attributes.size() != 6) continue;
        const bool nests = type == "IFCRELNESTS";
        const auto host = refs_in(attributes[nests ? 4 : 5]);
        if (host.size() != 1 || host[0] >= max_id) continue;
        for (const uint32_t port : refs_in(attributes[nests ? 5 : 4])) {
            if (port >= max_id || spf.type(port) != "IFCDISTRIBUTIONPORT") continue;
            for (auto& v : in_output) v[port] = v[host[0]];
        }
    }

    // A dummy an exporter left at the world's 0,0, such as an empty surface, far from all else: it stretches the
    // model's box over kilometres. Only when nearly everything else is far away, so a model drawn about 0,0 keeps all.
    uint64_t at_origin = 0;
    if (rules.cull_at_origin > 0 && length_unit(spf) > 0) {
        Extents extents(spf);
        const double reach = rules.cull_at_origin / length_unit(spf);
        std::vector<uint32_t> near;
        uint64_t bodies = 0;
        for (const auto& element : model.elements) {
            if (!element.has_body) continue;
            const auto box = extents.product(element.id);
            if (!box || box->empty()) continue;
            ++bodies;
            bool close = true;
            for (int a = 0; a < 2; ++a) close = close && std::abs(box->low[a]) < reach && std::abs(box->high[a]) < reach;
            if (close) near.push_back(element.id);
        }
        if (!near.empty() && near.size() * 100 < bodies) {
            for (const uint32_t id : near) {
                for (auto& v : in_output) v[id] = false;
            }
            at_origin = near.size();
        }
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
    MergePlan merge;
    // The federation file goes last, as it also takes whatever nothing else reached
    for (const Output output : {HIDDEN, ENVELOPES, FEDERATION}) {
        auto gone = gone_from(model, output, max_id, in_output[output]);
        if (output == FEDERATION && options.merge) {
            std::vector<uint32_t> candidates;
            for (const auto& element : model.elements) {
                const bool spatial = std::find(always.begin(), always.end(), element.id) != always.end();
                if (in_output[FEDERATION][element.id] && !element.feature && !spatial) candidates.push_back(element.id);
            }
            const std::function<bool(uint32_t)> is_gone = [&](uint32_t id) { return id < gone.size() && gone[id]; };
            merge = plan_merge(spf, candidates, is_gone, stem, options.merge_gap_m);
            for (const auto& [element, asset] : merge.merged_into) gone[element] = true;
            for (const uint32_t level : merge.dropped) {
                gone[level] = true;
                in_output[FEDERATION][level] = false;
            }
            // A type whose elements were all merged goes with them, and its shapes, which the asset has its own
            // copies of: otherwise the project's declaration of its types would keep it
            std::unordered_map<uint32_t, bool> type_used;
            for (const uint32_t id : relationships) {
                if (spf.type(id) != "IFCRELDEFINESBYTYPE") continue;
                const auto attributes = attributes_of(spf.line(id));
                if (attributes.size() != 6) continue;
                const auto type = refs_in(attributes[5]);
                if (type.size() != 1) continue;
                bool used = false, merged = false;
                for (const uint32_t object : refs_in(attributes[4])) {
                    if (merge.merged_into.count(object)) merged = true;
                    else if (!gone[object]) used = true;
                }
                auto& known = type_used.emplace(type[0], false).first->second;
                known = known || used || !merged;
            }
            for (const auto& [type, used] : type_used) {
                if (!used) gone[type] = true;
            }
        }
        writers[output] = std::make_unique<Writer>(spf, model, output, std::move(gone));
        Writer& writer = *writers[output];
        for (const uint32_t id : always) writer.include(id);
        for (const auto& element : model.elements) {
            if (!in_output[output][element.id]) continue;
            writer.include(element.id);
            const uint32_t others = in_output[(output + 1) % 3][element.id] + in_output[(output + 2) % 3][element.id];
            if (!others) writer.whole().insert(element.id);
        }
        if (output == FEDERATION) {
            for (const uint32_t id : merge.keep) writer.include(id);
            writer.add_lines(merge.lines);
            writer.replace_lines(merge.replace);
        }
        writer.close_over(relationships, followers);
    }
    Writer& federation = *writers[FEDERATION];

    // What only merged elements used: their types, materials and sets of properties. The asset has what
    // it needs, and the manifest says what went into it, so these don't count as unused and stay out.
    std::vector<bool> left_behind(max_id, false);
    {
        std::vector<uint32_t> stack;
        for (const auto& [element, asset] : merge.merged_into) stack.push_back(element);
        for (const uint32_t id : relationships) {
            if (federation.included(id)) continue;
            bool merged = false;
            for_each_ref(spf.line(id), [&](uint32_t next) { merged = merged || merge.merged_into.count(next) > 0; });
            if (merged) stack.push_back(id);
        }
        while (!stack.empty()) {
            const uint32_t id = stack.back();
            stack.pop_back();
            if (id >= max_id || left_behind[id] || federation.included(id)) continue;
            left_behind[id] = true;
            for_each_ref(spf.line(id), [&](uint32_t next) { stack.push_back(next); });
        }
    }

    uint64_t unreached = 0;
    const std::unordered_set<uint32_t> layer_set(layers.begin(), layers.end());
    for (uint32_t id = 0; id < max_id; ++id) {
        if (spf.line(id).empty() || layer_set.count(id) || left_behind[id]) continue;
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
    std::array<BrepStats, 3> converted{};
    std::array<CompactStats, 3> compacted{};
    SimplifyStats simplified;
    std::array<InstanceStats, 3> instanced{};
    std::array<TidyStats, 3> tidied{};
    const Mapping mapping = options.mapping_path.empty() ? Mapping{} : load_mapping(options.mapping_path);
    MappingStats mapped;
    TraceStats trace_stats;
    for (const Output output : {FEDERATION, HIDDEN, ENVELOPES}) {
        const auto path = output_path(output);
        writers[output]->write(path.string(), layers);
        tidied[output] = tidy(path.string(), input.string());
        if (options.compact) converted[output] = breps_to_face_sets(path.string());
        if (options.instance) instanced[output] = instance(path.string(), options.precision_mm);
        if (options.compact) compacted[output] = compact(path.string(), options.precision_mm);
        if (output == FEDERATION && options.compact && options.simplify_mm > 0) simplified = simplify(path.string(), options.simplify_mm);
        if (output == FEDERATION) mapped = apply_mapping(path.string(), mapping);
        // After the mapping, so that what it does can't take this away
        if (output == FEDERATION) {
            std::vector<Traced> elements;
            std::map<size_t, Traced> assets;
            for (const auto& element : model.elements) {
                if (!in_output[FEDERATION][element.id] || element.feature) continue;
                if (std::find(always.begin(), always.end(), element.id) != always.end()) continue;
                if (element.type == "IfcDistributionPort" || element.type == "IfcAnnotation" || element.type == "IfcGrid") continue;
                const auto asset = merge.merged_into.find(element.id);
                if (asset != merge.merged_into.end()) {
                    Traced& traced = assets[asset->second];
                    traced.guid = merge.assets[asset->second].guid;
                    traced.sources.push_back(element.id);
                    continue;
                }
                const bool whole = !in_output[HIDDEN][element.id] && !in_output[ENVELOPES][element.id];
                elements.push_back({model.guid_in_output(element.id, element.guid, FEDERATION, whole), {element.id}});
            }
            for (auto& [index, traced] : assets) elements.push_back(std::move(traced));
            trace_stats = trace(path.string(), spf, input.filename().string(), elements);
        }
        const auto zipped = fs::path(path.string() + ZIPPED);
        // The federation file stays plain, as that's the format Bentley's list names
        if (!options.zip || output == FEDERATION) {
            fs::remove(zipped);  // from a run that did zip it
            if (output == FEDERATION && flat != path) {
                fs::remove(flat);  // from a run before discipline folders
                fs::remove(fs::path(flat.string() + ZIPPED));
            }
            // and from a run when it was or wasn't combined with others, so it's only federated once
            if (output == FEDERATION) fs::remove(federation_file(input, options).second);
            sizes[output] = fs::file_size(path);
            continue;
        }
        zip_file(path.string(), zipped.string(), path.filename().string());
        fs::remove(path);
        sizes[output] = fs::file_size(zipped);
    }

    const auto manifest_path = fs::path(options.out_dir) / "manifest" / (stem + ".manifest.csv");
    fs::create_directories(manifest_path.parent_path());
    std::ofstream manifest(manifest_path);
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
            const bool whole = places == 1;
            const bool copied = spatial && output != FEDERATION;
            const auto asset = merge.merged_into.find(element.id);
            const bool merged = output == FEDERATION && asset != merge.merged_into.end();
            const std::string guid = merged ? merge.assets[asset->second].guid : model.guid_in_output(element.id, element.guid, output, whole);
            const char* action = merged ? "merged" : copied ? "copied" : !whole ? "split" : output == FEDERATION ? "kept" : "moved";
            const auto apart = merge.apart.find(element.id);
            const std::string reason = merged ? merge.assets[asset->second].label + merge.assets[asset->second].note
                : output == FEDERATION && apart != merge.apart.end() ? apart->second + ", far from the rest of it"
                : copied ? "spatial structure"
                : output == HIDDEN ? (element.by_property || Rules::matches(rules.hidden, element.family) ? "hidden rule" : "keep-largest rule")
                : output == ENVELOPES ? (element.by_property || Rules::matches(rules.envelope, element.family) ? "envelope rule" : "transparent")
                : model.renamed.count(element.id) ? "GUID not unique" : "";
            manifest << element.guid << ',' << element.type << ',' << quoted(element.family) << ',' << OUTPUT_NAMES[output] << ',' << guid << ','
                     << element.items[output] << ',' << element.faces[output] << ',' << action << ',' << quoted(reason) << '\n';
        }
    }

    element_counts[FEDERATION] += merge.assets.size();
    element_counts[FEDERATION] -= merge.merged_into.size();

    const auto mb = [](uint64_t bytes) { return bytes / 1048576.0; };
    std::cout << std::fixed << std::setprecision(1) << input.filename().string() << ": " << mb(fs::file_size(input)) << " MB\n";
    for (const Output output : {FEDERATION, HIDDEN, ENVELOPES}) {
        std::cout << std::setw(12) << OUTPUT_NAMES[output] << std::setw(9) << mb(sizes[output]) << " MB " << std::setw(7) << element_counts[output] << " elements "
                  << std::setw(8) << item_counts[output] << " items " << std::setw(9) << face_counts[output] << " faces\n";
    }
    std::cout << "  " << split_elements << " elements split, " << moved_elements << " moved whole, " << model.conflicts.size()
              << " shared items wanted in two places (left in the federation file), " << unreached << " unused instances kept in the federation file\n";
    {
        uint64_t loose = 0;
        for (const auto& element : model.elements) {
            if (element.type == "IfcDistributionPort" && !in_output[FEDERATION][element.id] && !in_output[HIDDEN][element.id] && !in_output[ENVELOPES][element.id]) ++loose;
        }
        if (loose) std::cout << "  " << loose << " ports on nothing left out\n";
        if (at_origin) std::cout << "  " << at_origin << " bodies at the world's 0,0 left out by cull-at-origin\n";
    }
    if (forward.faces || forward.unsure) std::cout << "  " << forward.faces << " assemblies moved forward by forward-if, " << forward.unsure << " left as which way is forward couldn't be told\n";
    if (model.small_parts) std::cout << "  " << model.small_parts << " small parts of " << model.small_part_assemblies << " assemblies hidden by hidden-small-parts-if\n";
    if (!model.renamed.empty()) std::cout << "  " << model.renamed.size() << " GUIDs changed, as they weren't unique\n";
    if (source != input) {
        std::cout << "  written as IFC4: " << upgrade.lengthened << " instances given the attributes IFC4 added, " << upgrade.ports << " port relationships now nesting, "
                  << upgrade.retyped << " predefined types now user defined, " << upgrade.dropped << " with attributes IFC4 took away, " << upgrade.still_missing
                  << " values IFC4 needs left missing, as they were\n";
    }
    if (options.merge) {
        std::cout << "  " << merge.merged_into.size() << " elements merged into " << merge.assets.size() << " by site ID or design tag, " << merge.openings_moved
                  << " openings now cutting the asset, " << merge.ports_moved << " ports now on it, " << merge.left_out_other << " left out as they're placed in a way that can't be repeated, "
                  << merge.left_out_extent << " left out as where they reach couldn't be worked out\n";
        if (!merge.assets.empty()) {
            uint64_t left_mapped = 0;
            std::string why;
            for (const auto& [type, count] : merge.assets_mapped) {
                left_mapped += count;
                why += (why.empty() ? "" : ", ") + std::to_string(count) + " for " + type;
            }
            std::cout << "  " << merge.assets_plain << " assets with their parts' shapes moved into them, " << left_mapped << " left as mapped items of their parts"
                      << (why.empty() ? "" : " (" + why + ")") << "\n";
        }
        if (merge.quantities_combined) std::cout << "  " << merge.quantities_combined << " " << schema().quantity_set << " values worked out for assets as a whole\n";
        if (merge.placed_on_levels) std::cout << "  " << merge.placed_on_levels << " elements and assets placed from the level they're on\n";
        if (merge.type_levels || merge.unmanaged) {
            std::cout << "  " << merge.type_levels << " asset type levels, holding " << merge.on_building << " sites of one element or asset, named after their sites, and "
                      << merge.levels << " sites still in pieces as levels of their own (" << merge.on_levels << " pieces); " << merge.unmanaged << " elements with no site ID on "
                      << UNMANAGED_LEVEL << "; " << merge.dropped.size() << " levels left empty, so left out\n";
        }
        if (merge.by_design || merge.tags_not_ids) {
            std::cout << "  " << merge.by_design << " elements with no site ID merged by design tag, " << merge.joined_by_design << " of them joining the one site their tag is on; "
                      << merge.by_mark << " with a mark, so only going with others of the same mark; "
                      << merge.tags_not_ids << " design tags on most of the model, so not taken as IDs\n";
        }
        if (merge.in_runs) std::cout << "  " << merge.in_runs << " elements with a merge-runs placeholder site ID, in " << merge.runs << " runs of what touches\n";
        if (merge.sites_apart || !merge.apart.empty()) {
            std::cout << "  " << merge.sites_apart << " sites with parts over " << options.merge_gap_m << " m apart, so not merged as one; " << merge.apart.size()
                      << " elements left as they are, as nothing of their site is near them\n";
        }
        if (!merge.gap_checked) std::cout << "  warning: the model's length unit isn't plain SI, so parts of a site weren't checked for being far apart\n";
    }
    {
        const TidyStats& federated = tidied[FEDERATION];
        std::cout << "  federate tidied: " << federated.sets_dropped << " culled and duplicate sets left out, " << federated.repeats_dropped
                  << " properties left out of Data as other sets have them, " << federated.named << " elements renamed, " << federated.described
                  << " described as their asset component, " << federated.types_corrected << " " << schema().type_property << " values corrected to their site ID ("
                  << federated.types_mixed << " sets on elements of different types left), " << federated.histories << " owner histories marked as changed by IFCFilter\n";
    }
    if (options.compact) {
        const BrepStats& federated = converted[FEDERATION];
        if (federated.converted || federated.left)
            std::cout << "  federate breps as face sets: " << federated.converted << " (" << federated.faces << " faces, " << federated.faces_dropped << " with no area left out), "
                      << federated.left << " left as breps (" << federated.left_cut << " as openings reach them), " << mb(federated.bytes_before) << " -> " << mb(federated.bytes_after) << " MB\n";
        if (!federated.schema) std::cout << "  warning: the model isn't IFC4, which face sets need, so breps were left as they are\n";
    }
    if (options.instance) {
        const InstanceStats& federated = instanced[FEDERATION];
        std::cout << "  federate instanced: " << federated.sets << " face sets, " << federated.copies << " now placed from " << federated.shapes << " shapes, "
                  << federated.body_copies << " bodies of several placed from " << federated.bodies << " bodies, " << mb(federated.bytes_before) << " -> "
                  << mb(federated.bytes_after) << " MB\n";
        if (!federated.checked) std::cout << "  warning: the model's length unit isn't plain SI, so nothing was instanced\n";
    }
    if (options.compact) {
        const CompactStats& federated = compacted[FEDERATION];
        std::cout << "  federate written compactly: " << mb(federated.bytes_before) << " -> " << mb(federated.bytes_after) << " MB, "
                  << federated.instances_before << " -> " << federated.instances_after << " instances, " << federated.sets_triangulated << " face sets as triangles, "
                  << federated.sets_left << " left as polygons (" << federated.faces_left << " faces), " << federated.sets_cut << " left exactly as they were, as openings touch them, "
                  << federated.no_area_removed << " faces with no area removed\n";
        if (federated.no_area_left) std::cout << "  warning: " << federated.no_area_left << " faces with no area kept, as all their set had\n";
        if (!federated.rounded) std::cout << "  warning: the model's length unit isn't plain SI, so coordinates weren't rounded\n";
    }
    if (options.compact && options.simplify_mm > 0) {
        std::cout << "  federate simplified within " << options.simplify_mm << " mm: " << simplified.simplified << " of " << simplified.sets << " triangle sets, "
                  << simplified.triangles_before << " -> " << simplified.triangles_after << " triangles, " << simplified.left_cut << " left as openings cut them, "
                  << simplified.left_other << " left for colours, normals or shape, " << mb(simplified.bytes_before) << " -> " << mb(simplified.bytes_after) << " MB\n";
        if (!simplified.checked) std::cout << "  warning: the model's length unit isn't plain SI, so nothing was simplified\n";
    }
    if (!mapping.empty()) {
        std::cout << "  federate mapped: " << mapped.sets_deleted << " sets deleted, " << mapped.sets_renamed << " renamed, " << mapped.sets_emptied << " left empty so gone; "
                  << mapped.properties_deleted << " properties deleted, " << mapped.properties_renamed << " renamed; " << mapped.blanked << " attributes left empty, "
                  << mapped.retyped << " elements given another class\n";
        for (const std::string& warning : mapped.warnings) std::cout << "  warning: " << warning << "\n";
    }
    std::cout << "  " << trace_stats.elements << " elements given an " << TRACE_SET << " set of where they came from\n";
    if (trace_stats.missing) std::cout << "  warning: " << trace_stats.missing << " elements weren't found in the federation file by their GUID, so have no " << TRACE_SET << " set\n";
    uint64_t dropped = 0;
    for (const auto& writer : writers) dropped += writer->dropped();
    if (dropped) std::cout << "  warning: " << dropped << " lines couldn't be written without what they refer to\n";
    std::cout << std::flush;
    return 0;
}

// A model at most takes about 8 times its size in memory, measured on models from 24 to 260 MB
constexpr uint64_t MEMORY_PER_BYTE = 10;

std::string worker_command(const fs::path& model, const SplitOptions& options, const fs::path& shared, const fs::path& log) {
    const auto quote = [](const std::string& s) { return "\"" + s + "\""; };
    std::ostringstream command;
    command.precision(17);
    command << quote(program_path()) << " split " << quote(model.string()) << " --out " << quote(options.out_dir) << " --worker-shared " << quote(shared.string());
    if (!options.rules_path.empty()) command << " --rules " << quote(options.rules_path);
    command << " --transparent " << options.transparent << " --precision-mm " << options.precision_mm << " --simplify-mm " << options.simplify_mm;
    if (!options.combine_path.empty()) command << " --combine " << quote(options.combine_path);
    if (!options.mapping_path.empty()) command << " --mapping " << quote(options.mapping_path);
    if (!options.merge) command << " --no-merge";
    command << " --merge-gap " << options.merge_gap_m;
    if (!options.instance) command << " --no-instance";
    if (!options.compact) command << " --no-compact";
    if (!options.zip) command << " --no-zip";
    command << " > " << quote(log.string()) << " 2>&1";
#ifdef _WIN32
    return "\"" + command.str() + "\"";  // cmd takes off the outside quotes, and would otherwise take the program's
#else
    return command.str();
#endif
}

// Each model goes in a process of its own: all they share is the list of GUIDs. They start biggest first,
// and only when what's running leaves room in memory. One that fails gets another go on its own.
void run_in_parallel(const std::vector<fs::path>& models, const SplitOptions& options, unsigned jobs, const std::unordered_set<std::string>& shared, Progress& progress,
                     const std::function<void(const fs::path&, const Source&, int)>& finished) {
    const fs::path work = fs::path(options.out_dir) / ".work";
    fs::create_directories(work);
    const fs::path shared_path = work / "shared-guids.txt";
    {
        std::ofstream out(shared_path, std::ios::binary);
        for (const std::string& guid : shared) out << guid << '\n';
    }

    struct Job {
        fs::path model;
        uint64_t memory = 0;
        bool started = false;
    };
    std::vector<Job> queue;
    for (const fs::path& model : models) queue.push_back({model, fs::file_size(model) * MEMORY_PER_BYTE, false});
    std::sort(queue.begin(), queue.end(), [](const Job& a, const Job& b) { return a.memory > b.memory; });
    const uint64_t budget = available_memory() / 5 * 4;
    std::cout << models.size() << " models, up to " << jobs << " at once, in " << budget / (1ULL << 30) << " GB of memory\n" << std::flush;

    std::mutex lock;
    std::condition_variable room;
    uint64_t in_use = 0;
    size_t running = 0, done = 0;
    std::vector<fs::path> failed;

    const auto run_one = [&](const fs::path& model, bool retry) {
        const fs::path log = work / (model.stem().string() + ".log");
        Source source;
        int code = 1;
        std::string error;
        try {
            source = read_source(model);
            code = std::system(worker_command(model, options, shared_path, log).c_str());
        } catch (const std::exception& e) {
            error = e.what();
        }
        const std::lock_guard<std::mutex> guard(lock);
        if (code && !retry) {
            failed.push_back(model);
            return;
        }
        ++done;
        std::ifstream text(log);
        std::cout << "[" << done << "/" << models.size() << "] " << text.rdbuf();
        std::cout.clear();  // an empty log counts as a failed write
        if (!error.empty()) std::cerr << model.string() << ": " << error << "\n";
        std::cout << "  " << progress.finished(model, code, outputs_of(model, options)[0]) << "\n" << std::flush;
        // An exception that left this thread would end the whole run
        try {
            finished(model, source, code);
        } catch (const std::exception& e) {
            std::cerr << model.string() << ": " << e.what() << "\n";
        }
    };

    const auto worker = [&]() {
        while (true) {
            Job* job = nullptr;
            {
                std::unique_lock<std::mutex> guard(lock);
                while (true) {
                    bool waiting = false;
                    for (Job& candidate : queue) {
                        if (candidate.started) continue;
                        waiting = true;
                        if (running == 0 || !budget || in_use + candidate.memory <= budget) {
                            job = &candidate;
                            break;
                        }
                    }
                    if (job || !waiting) break;
                    room.wait(guard);
                }
                if (!job) return;
                job->started = true;
                in_use += job->memory;
                ++running;
            }
            progress.started(job->model);
            run_one(job->model, false);
            {
                const std::lock_guard<std::mutex> guard(lock);
                in_use -= job->memory;
                --running;
            }
            room.notify_all();
        }
    };
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < jobs; ++i) threads.emplace_back(worker);
    for (auto& thread : threads) thread.join();

    for (const fs::path& model : failed) {
        std::cout << "trying " << model.filename().string() << " again, on its own\n" << std::flush;
        progress.started(model);
        run_one(model, true);
    }
    std::error_code ignored;
    fs::remove_all(work, ignored);
}

}  // namespace

// Writes each group of the register as one federation file, in federate, from its models' files in parts:
// one that isn't there, or is older than any of them or than the register. Returns false if one couldn't be.
bool combine_all(const std::vector<fs::path>& models, const SplitOptions& options) {
    const auto& groups = groups_of(options);
    if (groups.empty()) return true;
    std::map<std::string, std::vector<fs::path>> parts;
    std::map<std::string, std::string> discipline_of;
    for (const fs::path& model : models) {
        const auto group = groups.find(model.stem().string());
        if (group == groups.end()) continue;
        parts[group->second].push_back(federation_file(model, options).first);
        discipline_of.emplace(group->second, fs::absolute(model).parent_path().filename().string());
    }
    const auto register_time = fs::last_write_time(options.combine_path);
    bool ok = true;
    for (const auto& [name, files] : parts) {
        const auto out = fs::path(options.out_dir) / FOLDERS[FEDERATION] / discipline_of[name] /
                         (name.size() > 4 && name.substr(name.size() - 4) == ".ifc" ? name : name + SUFFIXES[FEDERATION]);
        const bool missing = std::any_of(files.begin(), files.end(), [](const fs::path& p) { return !fs::exists(p); });
        if (missing) {
            std::cerr << "warning: " << out.filename().string() << " isn't written, as one of its models has no federation file yet\n";
            ok = false;
            continue;
        }
        bool current = fs::exists(out) && fs::last_write_time(out) > register_time;
        for (const fs::path& file : files) current = current && fs::last_write_time(out) > fs::last_write_time(file);
        if (current) continue;
        std::string summary;
        if (!combine_files(files, out, summary)) {
            ok = false;
            continue;
        }
        std::cout << "combined " << out.filename().string() << ": " << summary << "\n" << std::flush;
    }
    return ok;
}

int split(const SplitOptions& options) {
    // One of the processes that run_in_parallel starts
    if (!options.worker_shared.empty()) {
        std::unordered_set<std::string> shared;
        std::ifstream in(options.worker_shared);
        for (std::string guid; std::getline(in, guid);) shared.insert(guid);
        return split_model(options.inputs.at(0), options, Rules::load(options.rules_path), shared);
    }

    const std::vector<fs::path> models = models_in(options.inputs);
    if (models.empty()) {
        std::cerr << "no .ifc files to work on\n";
        return 2;
    }
    // Outputs are named after the input, so these would overwrite each other
    std::unordered_map<std::string, fs::path> stems;
    for (const fs::path& model : models) {
        const auto [at, added] = stems.emplace(lowercase(model.stem().string()), model);
        if (!added) {
            std::cerr << at->second.string() << " and " << model.string() << " would have the same output names\n";
            return 2;
        }
    }

    const Rules rules = Rules::load(options.rules_path);
    if (!options.mapping_path.empty()) load_mapping(options.mapping_path);  // so a mistake in it stops the run before it starts
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    const unsigned jobs = static_cast<unsigned>(std::min<size_t>(options.jobs > 0 ? static_cast<unsigned>(options.jobs) : threads, models.size()));
    const SharedGuids found = models.size() > 1 ? guids_in_several(models, jobs) : SharedGuids{{}, std::vector<std::string>(models.size(), sha256_of(""))};
    const auto& shared = found.guids;
    const auto register_path = fs::path(options.out_dir) / "register.csv";
    Register sources = load_register(register_path);

    // A model whose input, build and shared GUIDs are as they were when its outputs were written, and whose
    // outputs are all there, would come out the same, so it's left as it is
    const std::string build = build_of(options);
    std::unordered_map<std::string, std::string> shared_of;
    for (size_t i = 0; i < models.size(); ++i) shared_of[models[i].string()] = found.by_model[i];
    std::vector<fs::path> to_do;
    {
        std::vector<std::string> hashes(models.size());
        std::atomic<size_t> next{0};
        const auto hash_models = [&]() {
            for (size_t i = next++; i < models.size(); i = next++) {
                const auto known = sources.find(models[i].stem().string());
                if (options.all || known == sources.end() || known->second.build != build || known->second.shared != found.by_model[i]) continue;
                hashes[i] = sha256_of_file(models[i]);
            }
        };
        std::vector<std::thread> hashing;
        for (unsigned i = 1; i < jobs; ++i) hashing.emplace_back(hash_models);
        hash_models();
        for (auto& thread : hashing) thread.join();
        for (size_t i = 0; i < models.size(); ++i) {
            const auto known = sources.find(models[i].stem().string());
            const auto outputs = outputs_of(models[i], options);
            const bool same = !hashes[i].empty() && known != sources.end() && known->second.sha256 == hashes[i] &&
                              std::all_of(outputs.begin(), outputs.end(), [](const fs::path& p) { return fs::exists(p); });
            if (!same) to_do.push_back(models[i]);
        }
    }
    std::cout << to_do.size() << " of " << models.size() << " models to do; the rest have the same input, build and shared GUIDs as their outputs"
              << (options.all ? "" : " (--all does them all)") << "\n" << std::flush;
    if (to_do.empty()) return combine_all(models, options) ? 0 : 1;
    // From here, what's printed also goes to the run's log, and progress.html says how it's going
    Progress progress(options.out_dir, to_do, models.size(), jobs);
    const Tee tee_out(progress, std::cout), tee_err(progress, std::cerr);
    std::cout << "progress: " << (fs::path(options.out_dir) / "progress.html").string() << ", log: " << progress.log_path().string() << "\n" << std::flush;
    int result = 0;
    size_t failed = 0;
    std::array<size_t, 3> states{};
    bool register_saved = true;
    // The source is read first, so that the register says what was there when the model was read
    const auto finished = [&](const fs::path& model, const Source& source, int code) {
        if (code) {
            result = code;
            ++failed;
            return;
        }
        ++states[static_cast<size_t>(state_of(sources, source))];
        Source done = source;
        done.build = build;
        done.shared = shared_of[model.string()];
        record(sources, std::move(done));
        // As it goes, so that a stopped run still counts. The next save has everything, so this can't lose the run.
        try {
            save_register(register_path, sources);
        } catch (const std::exception& e) {
            std::cerr << "warning: the register wasn't saved: " << e.what() << "\n";
            register_saved = false;
        }
    };
    if (jobs > 1) {
        run_in_parallel(to_do, options, jobs, shared, progress, finished);
    } else {
        for (const fs::path& model : to_do) {
            // One bad model shouldn't lose the rest of a long run
            int code = 1;
            Source source;
            progress.started(model);
            try {
                source = read_source(model);
                code = split_model(model, options, rules, shared);
            } catch (const std::exception& e) {
                std::cerr << model.string() << ": " << e.what() << "\n";
            }
            std::cout << "  " << progress.finished(model, code, outputs_of(model, options)[0]) << "\n" << std::flush;
            finished(model, source, code);
        }
    }
    if (!register_saved) {
        save_register(register_path, sources);  // a last go; if this fails too, it's reported as an error
    }
    std::cout << "register: " << states[static_cast<size_t>(SourceState::New)] << " new, " << states[static_cast<size_t>(SourceState::Changed)] << " changed, "
              << states[static_cast<size_t>(SourceState::Unchanged)] << " unchanged\n";
    if (failed) std::cerr << failed << " of " << to_do.size() << " models failed\n";
    if (!combine_all(models, options) && !result) result = 1;
    progress.done();
    return result;
}

int check(const std::vector<std::string>& inputs, const std::string& out_dir) {
    const auto register_path = fs::path(out_dir) / "register.csv";
    if (!fs::exists(register_path)) std::cout << "no register yet at " << register_path.string() << ", so everything is new\n";
    const Register sources = load_register(register_path);
    size_t added = 0, changed = 0, unchanged = 0, missing = 0;
    for (const fs::path& model : models_in(inputs)) {
        const Source source = read_source(model);
        switch (state_of(sources, source)) {
            case SourceState::New:
                ++added;
                std::cout << "new        " << source.model << "  modified " << source.modified << "\n";
                break;
            case SourceState::Changed: {
                const Source& known = sources.at(source.model);
                ++changed;
                std::cout << "changed    " << source.model << "  modified " << known.modified << " -> " << source.modified << ", exported " << known.ifc_timestamp << " -> "
                          << source.ifc_timestamp << ", " << known.size << " -> " << source.size << " bytes\n";
                break;
            }
            case SourceState::Unchanged:
                ++unchanged;
                break;
        }
    }
    for (const auto& [model, source] : sources) {
        if (fs::exists(source.path)) continue;
        ++missing;
        std::cout << "missing    " << model << "  was at " << source.path << "\n";
    }
    std::cout << added << " new, " << changed << " changed, " << missing << " missing, " << unchanged << " unchanged\n";
    return added || changed || missing ? 1 : 0;
}

}  // namespace ifcfilter

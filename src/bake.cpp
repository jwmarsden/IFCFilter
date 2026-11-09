#include "bake.h"

#include <array>
#include <charconv>
#include <cstdlib>
#include <set>

namespace bimpass {
namespace {

// Solids placed by a Position, which everything else in them is relative to, and where it is
const std::map<std::string_view, size_t> POSITIONED = {
    {"IFCEXTRUDEDAREASOLID", 1},          {"IFCEXTRUDEDAREASOLIDTAPERED", 1}, {"IFCREVOLVEDAREASOLID", 1},
    {"IFCREVOLVEDAREASOLIDTAPERED", 1},   {"IFCSURFACECURVESWEPTAREASOLID", 1}, {"IFCFIXEDREFERENCESWEPTAREASOLID", 1},
    {"IFCBLOCK", 0},                      {"IFCRECTANGULARPYRAMID", 0},       {"IFCRIGHTCIRCULARCONE", 0},
    {"IFCRIGHTCIRCULARCYLINDER", 0},      {"IFCSPHERE", 0},
};

// A faceted brep or surface model, and everything under it: only its points are where they are
const std::set<std::string_view> FACETED = {"IFCFACETEDBREP",   "IFCFACETEDBREPWITHVOIDS", "IFCSHELLBASEDSURFACEMODEL", "IFCFACEBASEDSURFACEMODEL",
                                            "IFCCLOSEDSHELL",   "IFCOPENSHELL",            "IFCCONNECTEDFACESET",       "IFCFACE",
                                            "IFCFACEOUTERBOUND", "IFCFACEBOUND",           "IFCPOLYLOOP",               "IFCCARTESIANPOINT"};

// A number as STEP writes a real: the shortest text that reads back as the same double
std::string real_text(double value) {
    char buffer[40];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    std::string out(buffer, result.ptr);
    const auto e = out.find_first_of("eE");
    if (out.find('.') == std::string::npos) out.insert(e == std::string::npos ? out.size() : e, ".");
    for (char& c : out) {
        if (c == 'e') c = 'E';
    }
    return out;
}

std::string vector_text(const Vector& v) { return "(" + real_text(v[0]) + "," + real_text(v[1]) + "," + real_text(v[2]) + ")"; }

// The (x,y,z) groups in a list of them
std::vector<Vector> triples_in(std::string_view list, bool& all_3d) {
    std::vector<Vector> out;
    all_3d = true;
    int axis = 0;
    Vector v{};
    for (const char* p = list.data(), *end = list.data() + list.size(); p < end;) {
        if (*p == '-' || *p == '+' || *p == '.' || (*p >= '0' && *p <= '9')) {
            char* after = nullptr;
            const double value = std::strtod(p, &after);
            if (after == p) {
                ++p;
                continue;
            }
            if (axis < 3) v[axis] = value;
            ++axis;
            p = after;
        } else {
            if (*p == ')' && axis > 0) {
                if (axis != 3) all_3d = false;
                out.push_back(v);
                axis = 0;
            }
            ++p;
        }
    }
    return out;
}

std::string rebuilt(std::string_view type, const std::vector<std::string>& attributes) {
    std::string out(type);
    out += '(';
    for (size_t i = 0; i < attributes.size(); ++i) {
        if (i) out += ',';
        out += attributes[i];
    }
    return out + ");";
}

std::vector<std::string> strings_of(const std::vector<std::string_view>& attributes) { return {attributes.begin(), attributes.end()}; }

// A line's text with its references changed, leaving strings alone
std::string with_refs(std::string_view text, const std::function<uint32_t(uint32_t)>& change) {
    std::string out;
    out.reserve(text.size());
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\'') quoted = !quoted;
        if (!quoted && c == '#' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') {
            uint32_t id = 0;
            size_t j = i + 1;
            for (; j < text.size() && text[j] >= '0' && text[j] <= '9'; ++j) id = id * 10 + static_cast<uint32_t>(text[j] - '0');
            out += reference(change(id));
            i = j - 1;
            continue;
        }
        out += c;
    }
    return out;
}

// The text after "#id=", which emit numbers again
std::string_view body_of(std::string_view line) {
    const auto equals = line.find('=');
    return equals == std::string_view::npos ? line : line.substr(equals + 1);
}

}  // namespace

bool Baker::can_move(uint32_t representation) {
    why_not_.clear();
    const auto attributes = attributes_of(spf_.line(representation));
    if (attributes.size() != 4) {
        why_not_ = std::string(spf_.type(representation));
        return false;
    }
    for (const uint32_t item : refs_in(attributes[3])) {
        if (!gone_(item) && !can_move_item(item, 0)) return false;
    }
    return true;
}

bool Baker::can_move_item(uint32_t item, int depth) {
    const auto known = movable_.find(item);
    if (known != movable_.end()) {
        if (!known->second.empty()) why_not_ = known->second;
        return known->second.empty();
    }
    why_not_.clear();
    bool ok = false;
    const auto type = spf_.type(item);
    const auto attributes = attributes_of(spf_.line(item));
    if (depth > 8 || item >= spf_.max_id() || attributes.empty()) {
        ok = false;
    } else if (type == "IFCTRIANGULATEDFACESET" || type == "IFCPOLYGONALFACESET") {
        const auto list = refs_in(attributes[0]);
        ok = list.size() == 1 && spf_.type(list[0]) == "IFCCARTESIANPOINTLIST3D";
    } else if (const auto at = POSITIONED.find(type); at != POSITIONED.end()) {
        const auto position = attributes.size() > at->second ? refs_in(attributes[at->second]) : std::vector<uint32_t>{};
        ok = attributes.size() > at->second && (attributes[at->second] == "$" || (position.size() == 1 && axis_placement(spf_, position[0])));
    } else if (type == "IFCCSGSOLID") {
        const auto root = refs_in(attributes[0]);
        ok = root.size() == 1 && can_move_item(root[0], depth + 1);
    } else if (type == "IFCBOOLEANRESULT" || type == "IFCBOOLEANCLIPPINGRESULT") {
        const auto first = refs_in(attributes.size() == 3 ? attributes[1] : "");
        const auto second = refs_in(attributes.size() == 3 ? attributes[2] : "");
        ok = first.size() == 1 && second.size() == 1 && can_move_item(first[0], depth + 1) && can_move_item(second[0], depth + 1);
    } else if (type == "IFCHALFSPACESOLID" || type == "IFCPOLYGONALBOUNDEDHALFSPACE" || type == "IFCBOXEDHALFSPACE") {
        const auto plane = refs_in(attributes[0]);
        ok = plane.size() == 1 && spf_.type(plane[0]) == "IFCPLANE";
        if (ok) {
            const auto placed = refs_in(attributes_of(spf_.line(plane[0]))[0]);
            ok = placed.size() == 1 && axis_placement(spf_, placed[0]).has_value();
        }
        if (ok && type == "IFCPOLYGONALBOUNDEDHALFSPACE") {
            const auto position = refs_in(attributes.size() == 4 ? attributes[2] : "");
            ok = position.size() == 1 && axis_placement(spf_, position[0]).has_value();
        }
        if (ok && type == "IFCBOXEDHALFSPACE") ok = false;  // its box is in the plane's frame too; not worth it
    } else if (FACETED.count(type)) {
        // Everything under it must be faces and 3D points
        ok = true;
        std::vector<uint32_t> stack{item};
        std::set<uint32_t> seen;
        while (ok && !stack.empty()) {
            const uint32_t id = stack.back();
            stack.pop_back();
            if (!seen.insert(id).second) continue;
            const auto kind = spf_.type(id);
            if (!FACETED.count(kind)) {
                ok = false;
                break;
            }
            if (kind == "IFCCARTESIANPOINT") {
                bool all_3d = false;
                const auto points = triples_in(attributes_of(spf_.line(id))[0], all_3d);
                ok = points.size() == 1 && all_3d;
                continue;
            }
            for_each_ref(spf_.line(id), [&](uint32_t next) { stack.push_back(next); });
        }
    } else if (type == "IFCMAPPEDITEM") {
        const auto map = attributes.size() == 2 ? refs_in(attributes[0]) : std::vector<uint32_t>{};
        const auto target = attributes.size() == 2 ? refs_in(attributes[1]) : std::vector<uint32_t>{};
        ok = map.size() == 1 && target.size() == 1 && operator_of(spf_, target[0]).has_value();
        if (ok) {
            const auto parts = attributes_of(spf_.line(map[0]));
            const auto origin = parts.size() == 2 ? refs_in(parts[0]) : std::vector<uint32_t>{};
            const auto held = parts.size() == 2 ? refs_in(parts[1]) : std::vector<uint32_t>{};
            ok = origin.size() == 1 && axis_placement(spf_, origin[0]) && held.size() == 1;
            if (ok) {
                const auto representation = attributes_of(spf_.line(held[0]));
                ok = representation.size() == 4;
                for (const uint32_t next : ok ? refs_in(representation[3]) : std::vector<uint32_t>{}) ok = ok && can_move_item(next, depth + 1);
            }
        }
    }
    // What's under it that couldn't be, if that's why, or else its own type
    if (!ok && why_not_.empty()) why_not_ = std::string(type);
    movable_[item] = ok ? std::string() : why_not_;
    return ok;
}

void Baker::move(uint32_t representation, const Rigid& to, std::vector<uint32_t>& items, std::vector<uint32_t>& kept) {
    for (const uint32_t item : refs_in(attributes_of(spf_.line(representation))[3])) {
        if (!gone_(item)) move_item(item, to, 0, items, kept);
    }
}

uint32_t Baker::emit(const std::string& text, std::vector<uint32_t>& kept) {
    for_each_ref("#0=" + text, [&](uint32_t next) {
        if (next < spf_.max_id()) kept.push_back(next);
    });
    return emitter_.emit(text);
}

uint32_t Baker::moved_placement(std::string_view reference_text, const Rigid& to) {
    const auto ids = refs_in(reference_text);
    const Rigid here = ids.size() == 1 ? *axis_placement(spf_, ids[0]) : Rigid{};
    return emitter_.emit_placement(compose(to, here));
}

void Baker::move_item(uint32_t item, const Rigid& to, int depth, std::vector<uint32_t>& out, std::vector<uint32_t>& kept) {
    const auto type = spf_.type(item);
    const auto attributes = attributes_of(spf_.line(item));
    auto changed = strings_of(attributes);
    uint32_t moved = 0;
    if (type == "IFCTRIANGULATEDFACESET" || type == "IFCPOLYGONALFACESET") {
        const uint32_t list = refs_in(attributes[0])[0];
        auto list_attributes = strings_of(attributes_of(spf_.line(list)));
        bool all_3d = true;
        std::string points = "(";
        for (const Vector& p : triples_in(list_attributes[0], all_3d)) {
            if (points.size() > 1) points += ',';
            points += vector_text(applied(to, p));
        }
        list_attributes[0] = points + ")";
        changed[0] = reference(emit(rebuilt("IFCCARTESIANPOINTLIST3D", list_attributes), kept));
        // Normals turn with it
        if (type == "IFCTRIANGULATEDFACESET" && changed.size() > 1 && changed[1] != "$") {
            Rigid turn = to;
            turn.origin = {0, 0, 0};
            std::string normals = "(";
            for (const Vector& n : triples_in(changed[1], all_3d)) {
                if (normals.size() > 1) normals += ',';
                normals += vector_text(applied(turn, n));
            }
            changed[1] = normals + ")";
        }
        moved = emit(rebuilt(type, changed), kept);
    } else if (const auto at = POSITIONED.find(type); at != POSITIONED.end()) {
        changed[at->second] = reference(moved_placement(attributes[at->second], to));
        moved = emit(rebuilt(type, changed), kept);
    } else if (type == "IFCCSGSOLID") {
        std::vector<uint32_t> root;
        move_item(refs_in(attributes[0])[0], to, depth + 1, root, kept);
        changed[0] = reference(root[0]);
        moved = emit(rebuilt(type, changed), kept);
    } else if (type == "IFCBOOLEANRESULT" || type == "IFCBOOLEANCLIPPINGRESULT") {
        std::vector<uint32_t> operands;
        move_item(refs_in(attributes[1])[0], to, depth + 1, operands, kept);
        move_item(refs_in(attributes[2])[0], to, depth + 1, operands, kept);
        changed[1] = reference(operands[0]);
        changed[2] = reference(operands[1]);
        moved = emit(rebuilt(type, changed), kept);
    } else if (type == "IFCHALFSPACESOLID" || type == "IFCPOLYGONALBOUNDEDHALFSPACE") {
        const uint32_t plane = refs_in(attributes[0])[0];
        const uint32_t placed = moved_placement(attributes_of(spf_.line(plane))[0], to);
        changed[0] = reference(emitter_.emit("IFCPLANE(" + reference(placed) + ");"));
        if (type == "IFCPOLYGONALBOUNDEDHALFSPACE") changed[2] = reference(moved_placement(attributes[2], to));
        moved = emit(rebuilt(type, changed), kept);
    } else if (FACETED.count(type)) {
        std::unordered_map<uint32_t, uint32_t> done;
        moved = moved_faces(item, to, done, kept);
    } else if (type == "IFCMAPPEDITEM") {
        const auto parts = attributes_of(spf_.line(refs_in(attributes[0])[0]));
        const Rigid origin = *axis_placement(spf_, refs_in(parts[0])[0]);
        const Rigid placed = *operator_of(spf_, refs_in(attributes[1])[0]);
        const Rigid inner = compose(to, compose(placed, inverse(origin)));
        for (const uint32_t next : refs_in(attributes_of(spf_.line(refs_in(parts[1])[0]))[3])) move_item(next, inner, depth + 1, out, kept);
        return;
    }
    copy_followers(item, moved, kept);
    out.push_back(moved);
}

uint32_t Baker::moved_faces(uint32_t id, const Rigid& to, std::unordered_map<uint32_t, uint32_t>& done, std::vector<uint32_t>& kept) {
    const auto seen = done.find(id);
    if (seen != done.end()) return seen->second;
    uint32_t moved = 0;
    if (spf_.type(id) == "IFCCARTESIANPOINT") {
        bool all_3d = true;
        const auto points = triples_in(attributes_of(spf_.line(id))[0], all_3d);
        moved = emitter_.emit("IFCCARTESIANPOINT(" + vector_text(applied(to, points[0])) + ");");
    } else {
        std::vector<uint32_t> under;
        for_each_ref(spf_.line(id), [&](uint32_t next) { under.push_back(next); });
        for (const uint32_t next : under) moved_faces(next, to, done, kept);
        moved = emit(with_refs(body_of(spf_.line(id)), [&](uint32_t next) { return done.at(next); }), kept);
    }
    done[id] = moved;
    return moved;
}

void Baker::read_followers() {
    followers_read_ = true;
    for (uint32_t id = 0; id < spf_.max_id(); ++id) {
        const auto type = spf_.type(id);
        size_t at = 0;
        if (type == "IFCSTYLEDITEM" || type == "IFCINDEXEDCOLOURMAP") at = 0;
        else if (type == "IFCINDEXEDTRIANGLETEXTUREMAP" || type == "IFCINDEXEDPOLYGONALTEXTUREMAP") at = 1;
        else continue;
        const auto attributes = attributes_of(spf_.line(id));
        const auto item = attributes.size() > at ? refs_in(attributes[at]) : std::vector<uint32_t>{};
        if (item.size() == 1) followers_[item[0]].push_back(id);
    }
}

void Baker::copy_followers(uint32_t from, uint32_t to, std::vector<uint32_t>& kept) {
    if (!followers_read_) read_followers();
    const auto found = followers_.find(from);
    if (found == followers_.end()) return;
    for (const uint32_t follower : found->second) {
        const auto type = spf_.type(follower);
        auto attributes = strings_of(attributes_of(spf_.line(follower)));
        attributes[type == "IFCSTYLEDITEM" || type == "IFCINDEXEDCOLOURMAP" ? 0 : 1] = reference(to);
        emit(rebuilt(type, attributes), kept);
    }
}

std::string Baker::representation_type(const std::vector<uint32_t>& items) const {
    bool meshes = true, breps = true, swept = true, solids = true, csg = true;
    for (const uint32_t item : items) {
        const std::string* line = emitter_.line_of(item);
        const std::string_view text = line ? body_of(*line) : std::string_view();
        const std::string_view type = text.substr(0, text.find('('));
        const bool mesh = type == "IFCTRIANGULATEDFACESET" || type == "IFCPOLYGONALFACESET";
        const bool brep = type == "IFCFACETEDBREP" || type == "IFCFACETEDBREPWITHVOIDS";
        const bool sweep = type.find("SWEPTAREASOLID") != std::string_view::npos || type.find("AREASOLID") != std::string_view::npos;
        const bool primitive = type == "IFCBLOCK" || type == "IFCRECTANGULARPYRAMID" || type == "IFCRIGHTCIRCULARCONE" || type == "IFCRIGHTCIRCULARCYLINDER" || type == "IFCSPHERE";
        const bool boolean = type == "IFCBOOLEANRESULT" || type == "IFCBOOLEANCLIPPINGRESULT";
        meshes = meshes && mesh;
        breps = breps && brep;
        swept = swept && sweep;
        solids = solids && (brep || sweep || type == "IFCCSGSOLID");
        csg = csg && (boolean || primitive || type == "IFCCSGSOLID" || brep || sweep);
    }
    if (meshes) return "Tessellation";
    if (breps) return "Brep";
    if (swept) return "SweptSolid";
    if (solids) return "SolidModel";
    if (csg) return "CSG";
    return "SolidModel";
}

}  // namespace bimpass

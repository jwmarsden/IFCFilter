#include "simplify.h"

#include "extent.h"
#include "spf.h"
#include "system.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Core/System/omstream.hh>
#include <OpenMesh/Tools/Decimater/McDecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModNormalFlippingT.hh>
#include <OpenMesh/Tools/Decimater/ModBaseT.hh>
#include <OpenMesh/Core/Geometry/QuadricT.hh>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifcfilter {
namespace {

// Sets smaller than this aren't worth it
constexpr size_t MIN_TRIANGLES = 48;

struct Traits : public OpenMesh::DefaultTraits {
    using Point = OpenMesh::Vec3d;
    using Normal = OpenMesh::Vec3d;
    VertexAttributes(OpenMesh::Attributes::Status);
    FaceAttributes(OpenMesh::Attributes::Status | OpenMesh::Attributes::Normal);
    EdgeAttributes(OpenMesh::Attributes::Status);
};
using Mesh = OpenMesh::TriMesh_ArrayKernelT<Traits>;

// How far a point that's kept is from the planes of the faces it stands for: the sum of the squares of its distance
// from each. OpenMesh's ModQuadricT weights each plane by its face's area, so its error is only a distance squared
// in units where faces are about 1 across: in metres, small faces let points move by hundreds of millimetres, as on
// RW's road meshes. Here each plane counts the same, so under the tolerance squared it's within the tolerance of each.
template <class MeshT>
class ModPlanesT : public OpenMesh::Decimater::ModBaseT<MeshT> {
  public:
    DECIMATING_MODULE(ModPlanesT, MeshT, Planes);
    using Quadric = OpenMesh::Geometry::QuadricT<double>;

    explicit ModPlanesT(MeshT& mesh) : Base(mesh, false) { Base::mesh().add_property(quadrics_); }
    ~ModPlanesT() override { Base::mesh().remove_property(quadrics_); }

    void set_max_err(double error) { max_error_ = error; }

    void initialize() override {
        auto& mesh = Base::mesh();
        for (const auto vertex : mesh.vertices()) mesh.property(quadrics_, vertex).clear();
        for (const auto face : mesh.faces()) {
            auto corner = mesh.cfv_iter(face);
            const auto v0 = *corner, v1 = *++corner, v2 = *++corner;
            const auto p0 = mesh.point(v0), p1 = mesh.point(v1), p2 = mesh.point(v2);
            auto normal = (p1 - p0) % (p2 - p0);
            const double length = normal.norm();
            if (!(length > 0)) continue;  // no area, so no plane
            normal /= length;
            const Quadric plane(normal[0], normal[1], normal[2], -(normal | p0));
            mesh.property(quadrics_, v0) += plane;
            mesh.property(quadrics_, v1) += plane;
            mesh.property(quadrics_, v2) += plane;
        }
    }

    float collapse_priority(const CollapseInfo& collapse) override {
        Quadric both = Base::mesh().property(quadrics_, collapse.v0);
        both += Base::mesh().property(quadrics_, collapse.v1);
        const double error = both(collapse.p1);
        if (!(error <= max_error_)) return static_cast<float>(Base::ILLEGAL_COLLAPSE);
        // The planes go on for ever, so a point can be near all of them and far from where the faces were, at a
        // fold or a thin fin, by up to 100 mm on RW's walls. So the point that goes has to be within the tolerance
        // of the triangles that take its place, as well.
        if (!near_what_replaces(collapse)) return static_cast<float>(Base::ILLEGAL_COLLAPSE);
        return static_cast<float>(error);
    }

    void postprocess_collapse(const CollapseInfo& collapse) override {
        Base::mesh().property(quadrics_, collapse.v1) += Base::mesh().property(quadrics_, collapse.v0);
    }

  private:
    using Point = typename MeshT::Point;

    // The squared distance from a point to a triangle
    static double distance_squared(const Point& p, const Point& a, const Point& b, const Point& c) {
        // Ericson, Real-Time Collision Detection, 5.1.5
        const Point ab = b - a, ac = c - a, ap = p - a;
        const double d1 = ab | ap, d2 = ac | ap;
        if (d1 <= 0 && d2 <= 0) return ap.sqrnorm();
        const Point bp = p - b;
        const double d3 = ab | bp, d4 = ac | bp;
        if (d3 >= 0 && d4 <= d3) return bp.sqrnorm();
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) return (ap - ab * (d1 / (d1 - d3))).sqrnorm();
        const Point cp = p - c;
        const double d5 = ab | cp, d6 = ac | cp;
        if (d6 >= 0 && d5 <= d6) return cp.sqrnorm();
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) return (ap - ac * (d2 / (d2 - d6))).sqrnorm();
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return (bp - (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))).sqrnorm();
        const double denominator = 1.0 / (va + vb + vc);
        return (ap - ab * (vb * denominator) - ac * (vc * denominator)).sqrnorm();
    }

    // Whether the point that goes is within the tolerance of a triangle that's left around where it was: each of
    // its faces that stays, with it moved to where it goes
    bool near_what_replaces(const CollapseInfo& collapse) {
        const auto& mesh = Base::mesh();
        bool any = false;
        for (const auto face : mesh.vf_range(collapse.v0)) {
            if (face == collapse.fl || face == collapse.fr) continue;
            Point corners[3];
            int i = 0;
            for (const auto vertex : mesh.fv_range(face)) corners[i++] = vertex == collapse.v0 ? collapse.p1 : mesh.point(vertex);
            any = true;
            if (distance_squared(collapse.p0, corners[0], corners[1], corners[2]) <= max_error_) return true;
        }
        return !any;
    }

    OpenMesh::VPropHandleT<Quadric> quadrics_;
    double max_error_ = 0;
};

// The (x,y,z) groups in a list, as their text and as numbers
bool groups_in(std::string_view list, std::vector<std::string_view>& texts, std::vector<std::array<double, 3>>& points) {
    size_t depth = 0, start = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i] == '(') {
            if (++depth == 2) start = i;
        } else if (list[i] == ')') {
            if (depth-- == 2) {
                const std::string_view group = list.substr(start, i - start + 1);
                std::array<double, 3> p{};
                const std::string copy(group.substr(1, group.size() - 2));
                const char* at = copy.c_str();
                for (int axis = 0; axis < 3; ++axis) {
                    char* end = nullptr;
                    p[axis] = std::strtod(at, &end);
                    if (end == at) return false;
                    at = end;
                    while (*at == ',' || *at == ' ') ++at;
                }
                texts.push_back(group);
                points.push_back(p);
            }
        }
    }
    return depth == 0;
}

// The integers in a list of triangles, three at a time, from 1
bool triangles_in(std::string_view list, std::vector<std::array<uint32_t, 3>>& out) {
    std::vector<uint32_t> numbers;
    for (size_t i = 0; i < list.size();) {
        if (list[i] >= '0' && list[i] <= '9') {
            uint32_t value = 0;
            while (i < list.size() && list[i] >= '0' && list[i] <= '9') value = value * 10 + static_cast<uint32_t>(list[i++] - '0');
            numbers.push_back(value);
        } else {
            ++i;
        }
    }
    if (numbers.size() % 3) return false;
    for (size_t i = 0; i < numbers.size(); i += 3) out.push_back({numbers[i], numbers[i + 1], numbers[i + 2]});
    return true;
}

std::string rebuilt(std::string_view id_and_type, const std::vector<std::string>& attributes) {
    std::string out(id_and_type);
    out += '(';
    for (size_t i = 0; i < attributes.size(); ++i) {
        if (i) out += ',';
        out += attributes[i];
    }
    return out + ");";
}

}  // namespace

SimplifyStats simplify(const std::string& path, double tolerance_mm) {
    namespace fs = std::filesystem;
    SimplifyStats stats;
    stats.bytes_before = stats.bytes_after = fs::file_size(path);
    const std::string temporary = path + ".simplified";
    std::unordered_map<uint32_t, std::string> replaced;
    {
        const SpfFile spf(path);
        const double unit = length_unit(spf);
        if (unit <= 0 || tolerance_mm <= 0) return stats;
        stats.checked = true;
        const double tolerance = tolerance_mm * 1e-3 / unit;
        omlog().disable();  // the decimater says so each time it runs out of collapses
        omerr().disable();  // and the mesh, each time it can't take a triangle, which is expected
        const uint32_t max_id = spf.max_id();

        // Who uses each set and point list: a set anything but a representation, style, colour map or layer uses,
        // such as a boolean, has to stay as it is, as does one with a texture by triangle
        std::vector<uint32_t> list_users(max_id, 0);
        std::unordered_set<uint32_t> held;
        std::unordered_map<uint32_t, uint32_t> colour_map_of;
        std::vector<uint32_t> sets;
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto line = spf.line(id);
            if (line.empty()) continue;
            const auto type = spf.type(id);
            if (type == "IFCTRIANGULATEDFACESET") sets.push_back(id);
            const bool follower = type == "IFCSHAPEREPRESENTATION" || type == "IFCSTYLEDITEM" || type == "IFCPRESENTATIONLAYERASSIGNMENT" || type == "IFCPRESENTATIONLAYERWITHSTYLE";
            if (type == "IFCINDEXEDCOLOURMAP") {
                const auto attributes = attributes_of(line);
                const auto target = attributes.empty() ? std::vector<uint32_t>{} : refs_in(attributes[0]);
                if (target.size() == 1) colour_map_of[target[0]] = id;
                continue;
            }
            for_each_ref(line, [&](uint32_t next) {
                if (next >= max_id) return;
                const auto target = spf.type(next);
                if (target == "IFCCARTESIANPOINTLIST3D") ++list_users[next];
                else if (target == "IFCTRIANGULATEDFACESET" && !follower) held.insert(next);
            });
        }
        stats.sets = sets.size();
        const auto cut = sets_openings_touch(spf);

        for (const uint32_t id : sets) {
            if (cut.count(id)) {
                ++stats.left_cut;
                continue;
            }
            const auto line = spf.line(id);
            const auto attributes = attributes_of(line);
            const auto lists = attributes.empty() ? std::vector<uint32_t>{} : refs_in(attributes[0]);
            // Normals of its own would have to be worked out again, and a point index adds a step
            const bool plain = attributes.size() >= 4 && lists.size() == 1 && spf.type(lists[0]) == "IFCCARTESIANPOINTLIST3D" && list_users[lists[0]] == 1 &&
                               attributes[1] == "$" && (attributes.size() < 5 || attributes[4] == "$") && !held.count(id);
            if (!plain) {
                ++stats.left_other;
                continue;
            }
            std::vector<std::array<uint32_t, 3>> triangles;
            if (!triangles_in(attributes[3], triangles) || triangles.size() < MIN_TRIANGLES) continue;

            // A colour map is fine if it gives every triangle the same colour. Revit's jet fans in TV have more of
            // them than triangles, which doesn't matter when they're all the same, and is put right here
            std::string colour_index;
            const auto colour_map = colour_map_of.find(id);
            if (colour_map != colour_map_of.end()) {
                const auto colour = attributes_of(spf.line(colour_map->second));
                std::vector<std::array<uint32_t, 3>> unused;
                std::vector<uint32_t> indices;
                bool same = colour.size() == 4;
                if (same) {
                    for (size_t i = 0; i < colour[3].size();) {
                        if (colour[3][i] >= '0' && colour[3][i] <= '9') {
                            uint32_t value = 0;
                            while (i < colour[3].size() && colour[3][i] >= '0' && colour[3][i] <= '9') value = value * 10 + static_cast<uint32_t>(colour[3][i++] - '0');
                            indices.push_back(value);
                        } else {
                            ++i;
                        }
                    }
                    same = !indices.empty() && std::all_of(indices.begin(), indices.end(), [&](uint32_t c) { return c == indices[0]; });
                }
                if (!same) {
                    ++stats.left_other;
                    continue;
                }
                colour_index = std::to_string(indices[0]);
            }

            const auto list_attributes = attributes_of(spf.line(lists[0]));
            std::vector<std::string_view> texts;
            std::vector<std::array<double, 3>> points;
            if (list_attributes.empty() || !groups_in(list_attributes[0], texts, points) || points.empty()) {
                ++stats.left_other;
                continue;
            }

            // Points written the same are one point, so that pieces that meet are joined: Revit's jet fans in TV
            // repeat each point three times over
            std::unordered_map<std::string_view, uint32_t> first_of;
            std::vector<uint32_t> same_as(points.size());
            for (size_t i = 0; i < points.size(); ++i) same_as[i] = first_of.emplace(texts[i], static_cast<uint32_t>(i)).first->second;

            // From the first point, as world coordinates lose more than the tolerance in what the quadrics square
            Mesh mesh;
            const auto origin = points[0];
            std::vector<Mesh::VertexHandle> vertex_of(points.size());
            OpenMesh::VPropHandleT<uint32_t> original;
            mesh.add_property(original);
            for (size_t i = 0; i < points.size(); ++i) {
                if (same_as[i] != i) continue;
                const auto& p = points[i];
                vertex_of[i] = mesh.add_vertex(Mesh::Point(p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]));
                mesh.property(original, vertex_of[i]) = static_cast<uint32_t>(i);
            }
            // A triangle the mesh can't take, as an edge three or more share, stays as it is, and its corners stay
            // where they are so that it still fits
            std::vector<std::array<uint32_t, 3>> kept_as_is;
            bool fits = true;
            for (const auto& t : triangles) {
                if (t[0] < 1 || t[1] < 1 || t[2] < 1 || t[0] > points.size() || t[1] > points.size() || t[2] > points.size()) {
                    fits = false;
                    break;
                }
                const uint32_t a = same_as[t[0] - 1], b = same_as[t[1] - 1], c = same_as[t[2] - 1];
                if (a == b || b == c || a == c) continue;  // no area
                if (!mesh.add_face(vertex_of[a], vertex_of[b], vertex_of[c]).is_valid()) kept_as_is.push_back({a, b, c});
            }
            if (!fits || mesh.n_faces() == 0) {
                ++stats.left_other;
                continue;
            }
            for (const auto& t : kept_as_is) {
                for (const uint32_t corner : t) mesh.status(vertex_of[corner]).set_locked(true);
            }
            // An open edge stays where it is: the quadrics only measure how far a point is off a face, not along
            // it, so an edge could be pulled in for nothing, and on RW's road meshes was, by up to 913 mm
            for (const auto vertex : mesh.vertices()) {
                if (mesh.is_boundary(vertex)) mesh.status(vertex).set_locked(true);
            }
            const size_t before = triangles.size();
            mesh.update_face_normals();

            OpenMesh::Decimater::McDecimaterT<Mesh> decimater(mesh);
            ModPlanesT<Mesh>::Handle quadric;
            OpenMesh::Decimater::ModNormalFlippingT<Mesh>::Handle flipping;
            decimater.add(quadric);
            decimater.add(flipping);
            // Collapses are chosen from random samples rather than kept in order, as Revit's fans of thousands of
            // triangles around a point made keeping them in order take up to a minute a set. A bound on the distance
            // from what's taken away (ModHausdorffT) was tried first, and took 491 s on one of DU's sets.
            decimater.module(quadric).set_max_err(tolerance * tolerance);
            if (!decimater.initialize()) {
                ++stats.left_other;
                continue;
            }
            // The same choices on every run
            std::srand(1);
            decimater.decimate(mesh.n_vertices());
            mesh.garbage_collection();
            const size_t after = mesh.n_faces() + kept_as_is.size();
            // Not worth a rewrite, and a set can't be empty
            if (after == 0 || after * 20 > before * 19) continue;

            // What's left, as triangles of the points they use, written as they were written
            std::vector<std::array<uint32_t, 3>> left;  // by the points' places in the input
            left.reserve(after);
            for (const auto face : mesh.faces()) {
                std::array<uint32_t, 3> t{};
                int corner = 0;
                for (const auto vertex : mesh.fv_range(face)) t[corner++] = mesh.property(original, vertex);
                left.push_back(t);
            }
            left.insert(left.end(), kept_as_is.begin(), kept_as_is.end());
            std::unordered_map<uint32_t, uint32_t> place;  // a point's place in the input, and in what's written, from 1
            std::string point_text = "(";
            std::string triangle_text = "(";
            for (const auto& t : left) {
                if (triangle_text.size() > 1) triangle_text += ',';
                triangle_text += '(';
                for (int corner = 0; corner < 3; ++corner) {
                    const auto [at, added] = place.emplace(t[corner], static_cast<uint32_t>(place.size() + 1));
                    if (added) {
                        if (point_text.size() > 1) point_text += ',';
                        point_text += texts[t[corner]];
                    }
                    if (corner) triangle_text += ',';
                    triangle_text += std::to_string(at->second);
                }
                triangle_text += ')';
            }
            point_text += ')';
            triangle_text += ')';

            auto new_list = std::vector<std::string>(list_attributes.begin(), list_attributes.end());
            new_list[0] = point_text;
            replaced[lists[0]] = rebuilt(spf.line(lists[0]).substr(0, spf.line(lists[0]).find('(')), new_list);
            auto new_set = std::vector<std::string>(attributes.begin(), attributes.end());
            new_set[3] = triangle_text;
            replaced[id] = rebuilt(line.substr(0, line.find('(')), new_set);
            if (!colour_index.empty()) {
                const auto colour_line = spf.line(colour_map->second);
                auto colour = std::vector<std::string>();
                for (const auto part : attributes_of(colour_line)) colour.emplace_back(part);
                std::string indices = "(";
                for (size_t i = 0; i < after; ++i) indices += (i ? "," : "") + colour_index;
                colour[3] = indices + ")";
                replaced[colour_map->second] = rebuilt(colour_line.substr(0, colour_line.find('(')), colour);
            }
            ++stats.simplified;
            stats.triangles_before += before;
            stats.triangles_after += after;
        }
        if (replaced.empty()) return stats;

        std::ofstream out(temporary, std::ios::binary);
        out << spf.header();
        for (uint32_t id = 0; id < max_id; ++id) {
            const auto line = spf.line(id);
            if (line.empty()) continue;
            const auto change = replaced.find(id);
            out << (change == replaced.end() ? std::string(line) : change->second) << '\n';
        }
        out << "ENDSEC;\nEND-ISO-10303-21;\n";
    }
    replace_file(temporary, path);
    stats.bytes_after = fs::file_size(path);
    return stats;
}

}  // namespace ifcfilter

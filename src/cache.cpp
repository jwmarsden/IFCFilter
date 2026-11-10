#include "cache.h"

#include "compact.h"
#include "extent.h"
#include "register.h"
#include "rigid.h"
#include "spf.h"
#include "system.h"
#include "upgrade.h"

#include <ifcparse/schema.h>
#include <sqlite3.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
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

namespace fs = std::filesystem;

constexpr int FORMAT_VERSION = 1;
constexpr uint32_t NO_MATERIAL = 0xFFFFFFFF;  // a part's: the instance's; an instance's: the default
constexpr double QUANTUM_M = 0.0005;          // positions are 16 bits on the mesh's box, unless that's coarser than this
constexpr double FEATURE_COSINE = 0.8660254;  // edges where faces meet at more than 30 degrees are drawn
constexpr size_t TILE_INSTANCES = 2000, TILE_TRIANGLES = 200000;  // a tile is split past either
constexpr int TILE_DEPTH = 12;
constexpr int ZSTD_LEVEL = 12;
constexpr int MAX_DEPTH = 8;  // of mapped items and booleans
// Simplified meshes for far away: points snapped together on grids this many metres across, one level each
constexpr std::array<double, 3> LOD_CELLS_M{0.05, 0.25, 1.0};
constexpr size_t LOD_MIN_TRIANGLES = 48;  // smaller meshes are drawn as they are at any distance
constexpr double LOD_WORTH = 0.7;         // a level is kept if it has at most this share of the triangles of the one before

enum Flags : uint32_t { HAS_GEOMETRY = 1, HIDDEN = 2, ENVELOPE = 4, MERGED = 8, APPROXIMATE = 16 };

// Text

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

void append_utf8(uint32_t c, std::string& out) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

uint32_t hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

// A STEP string as UTF-8, its escapes undone; empty if the value isn't a string
std::string text_of(std::string_view value) {
    const auto open = value.find('\'');
    const auto close = value.rfind('\'');
    if (open == std::string_view::npos || close <= open) return {};
    std::string out;
    for (size_t i = open + 1; i < close; ++i) {
        const char c = value[i];
        if (c == '\'' && i + 1 < close && value[i + 1] == '\'') {
            out += '\'';
            ++i;
        } else if (c == '\\' && value.compare(i, 4, "\\X2\\") == 0) {
            size_t j = i + 4;
            for (; j + 4 <= close && value[j] != '\\'; j += 4) {
                uint32_t code = 0;
                for (size_t k = 0; k < 4; ++k) code = code * 16 + hex_value(value[j + k]);
                append_utf8(code, out);
            }
            i = j + 3;  // past the closing X0 escape
        } else if (c == '\\' && value.compare(i, 3, "\\X\\") == 0 && i + 4 < close) {
            append_utf8(hex_value(value[i + 3]) * 16 + hex_value(value[i + 4]), out);
            i += 4;
        } else if (c == '\\' && value.compare(i, 3, "\\S\\") == 0 && i + 3 < close) {
            append_utf8(static_cast<unsigned char>(value[i + 3]) + 128, out);
            i += 3;
        } else if (c == '\\' && i + 1 < close && value[i + 1] == '\\') {
            out += '\\';
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// A property's value as the cache stores it
struct Value {
    int type = 0;  // 0 text, 1 real, 2 integer, 3 boolean, 4 enumeration
    std::string text;
    double number = 0;
    bool numeric = false;
};

std::optional<Value> value_in(std::string_view written) {
    written = trimmed(written);
    if (written.empty() || written == "$" || written == "*") return std::nullopt;
    std::string wrapper;
    std::string_view inner = written;
    const auto open = written.find('(');
    if (written[0] != '\'' && open != std::string_view::npos && written.back() == ')') {
        wrapper = upper(trimmed(written.substr(0, open)));
        inner = trimmed(written.substr(open + 1, written.size() - open - 2));
    }
    Value value;
    if (inner.empty() || inner == "$") return std::nullopt;
    if (inner[0] == '\'') {
        value.text = text_of(inner);
    } else if (inner == ".T." || inner == ".F." || inner == ".U.") {
        value.type = 3;
        value.text = inner == ".T." ? "true" : inner == ".F." ? "false" : "unknown";
        value.number = inner == ".T." ? 1 : 0;
        value.numeric = inner != ".U.";
    } else if (inner[0] == '.') {
        value.type = 4;
        value.text = std::string(inner.substr(1, inner.size() >= 2 ? inner.size() - 2 : 0));
    } else {
        const std::string number(inner);
        char* end = nullptr;
        value.number = std::strtod(number.c_str(), &end);
        if (end == number.c_str()) return std::nullopt;
        value.numeric = true;
        value.type = wrapper == "IFCINTEGER" || wrapper == "IFCCOUNTMEASURE" || wrapper == "IFCTIMESTAMP" ? 2 : 1;
    }
    return value;
}

// Binary

template <typename T>
void put(std::string& out, T value) {
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));  // little-endian hosts only, as the format is
    out.append(bytes, sizeof(T));
}

std::string compressed(const std::string& raw) {
    std::string out(ZSTD_compressBound(raw.size()), '\0');
    const size_t size = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), ZSTD_LEVEL);
    if (ZSTD_isError(size)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(size));
    out.resize(size);
    return out;
}

// SQLite

class Database {
  public:
    explicit Database(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error(path + ": " + sqlite3_errmsg(db_));
    }
    ~Database() { sqlite3_close(db_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool try_exec(const char* sql, std::string* error = nullptr) {
        char* message = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) == SQLITE_OK) return true;
        if (error) *error = message ? message : "unknown";
        sqlite3_free(message);
        return false;
    }
    void exec(const char* sql) {
        std::string error;
        if (!try_exec(sql, &error)) throw std::runtime_error("sqlite: " + error + ": " + sql);
    }
    sqlite3* get() { return db_; }

  private:
    sqlite3* db_ = nullptr;
};

class Statement {
  public:
    Statement(Database& db, const char* sql) {
        if (sqlite3_prepare_v2(db.get(), sql, -1, &statement_, nullptr) != SQLITE_OK) throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db.get()) + ": " + sql);
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& integer(int column, int64_t value) {
        sqlite3_bind_int64(statement_, column, value);
        return *this;
    }
    Statement& real(int column, double value) {
        sqlite3_bind_double(statement_, column, value);
        return *this;
    }
    Statement& text(int column, std::string_view value) {
        sqlite3_bind_text(statement_, column, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& blob(int column, std::string_view value) {
        sqlite3_bind_blob(statement_, column, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& null(int column) {
        sqlite3_bind_null(statement_, column);
        return *this;
    }
    // A string id, or NULL for none
    Statement& string_id(int column, int64_t id) { return id < 0 ? null(column) : integer(column, id); }
    void run() {
        const int result = sqlite3_step(statement_);
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
        if (result != SQLITE_DONE) throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(sqlite3_db_handle(statement_)));
    }

  private:
    sqlite3_stmt* statement_ = nullptr;
};

const char* const TABLES = R"(
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE strings(id INTEGER PRIMARY KEY, text TEXT UNIQUE);
CREATE TABLE elements(id INTEGER PRIMARY KEY, guid TEXT, class INTEGER, name INTEGER, description INTEGER, object_type INTEGER,
    tag INTEGER, parent INTEGER, sort INTEGER, type_name INTEGER, minx REAL, miny REAL, minz REAL, maxx REAL, maxy REAL, maxz REAL,
    tile INTEGER, flags INTEGER);
CREATE TABLE psets(id INTEGER PRIMARY KEY, name INTEGER, kind INTEGER);
CREATE TABLE pset_values(pset INTEGER, name INTEGER, value INTEGER, PRIMARY KEY(pset, name, value)) WITHOUT ROWID;
CREATE TABLE "values"(id INTEGER PRIMARY KEY, type INTEGER, text INTEGER, number REAL, unit INTEGER);
CREATE TABLE element_psets(element INTEGER, pset INTEGER, PRIMARY KEY(element, pset)) WITHOUT ROWID;
CREATE TABLE materials(id INTEGER PRIMARY KEY, r REAL, g REAL, b REAL, a REAL, name TEXT);
CREATE TABLE meshes(id INTEGER PRIMARY KEY, vertices INTEGER, triangles INTEGER, minx REAL, miny REAL, minz REAL, maxx REAL, maxy REAL, maxz REAL,
    lod INTEGER, blob BLOB);
CREATE TABLE tiles(id INTEGER PRIMARY KEY, parent INTEGER, level INTEGER, minx REAL, miny REAL, minz REAL, maxx REAL, maxy REAL, maxz REAL,
    size REAL, instances INTEGER, triangles INTEGER, meshes BLOB, blob BLOB);
CREATE TABLE mesh_lods(mesh INTEGER, lod INTEGER, triangles INTEGER, blob BLOB, PRIMARY KEY(mesh, lod)) WITHOUT ROWID;
CREATE TABLE tile_lods(tile INTEGER, lod INTEGER, triangles INTEGER, PRIMARY KEY(tile, lod)) WITHOUT ROWID;
)";

const char* const INDEXES = R"(
CREATE INDEX elements_by_parent ON elements(parent, sort);
CREATE INDEX elements_by_class ON elements(class);
CREATE UNIQUE INDEX elements_by_guid ON elements(guid);
CREATE INDEX element_psets_by_pset ON element_psets(pset);
CREATE INDEX pset_values_by_name ON pset_values(name, value);
CREATE INDEX values_by_text ON "values"(text);
CREATE INDEX values_by_number ON "values"(number);
)";

// Each text once
class Strings {
  public:
    explicit Strings(Database& db) : insert_(db, "INSERT INTO strings(id, text) VALUES(?, ?)") {}
    int64_t id(const std::string& text) {
        if (text.empty()) return -1;
        const auto seen = ids_.find(text);
        if (seen != ids_.end()) return seen->second;
        const int64_t id = static_cast<int64_t>(ids_.size());
        insert_.integer(1, id).text(2, text).run();
        ids_.emplace(text, id);
        return id;
    }

  private:
    Statement insert_;
    std::unordered_map<std::string, int64_t> ids_;
};

// Geometry

struct Part {
    uint32_t first = 0, count = 0, material = NO_MATERIAL;
};

struct Mesh {
    std::vector<Vector> points;      // metres, centred on the box
    std::vector<uint32_t> indices;   // three a triangle, from 0
    std::vector<uint32_t> materials; // one a triangle, while it's built
    std::vector<Part> parts;
    Vector centre{};  // what was taken off the points, in metres in the item's coordinates
    Box box;          // of the points, centred
    bool approximate = false;
};

// Puts a mesh's points into metres, centred on its box, and its triangles into runs of one material
void finish(Mesh& mesh, double unit) {
    Box box;
    for (Vector& p : mesh.points) {
        for (double& c : p) c *= unit;
        box.add(p);
    }
    for (int axis = 0; axis < 3; ++axis) mesh.centre[axis] = box.empty() ? 0 : (box.low[axis] + box.high[axis]) / 2;
    for (Vector& p : mesh.points) {
        for (int axis = 0; axis < 3; ++axis) p[axis] -= mesh.centre[axis];
        mesh.box.add(p);
    }
    const size_t triangles = mesh.indices.size() / 3;
    mesh.materials.resize(triangles, NO_MATERIAL);
    std::vector<uint32_t> order(triangles);
    for (uint32_t t = 0; t < triangles; ++t) order[t] = t;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return mesh.materials[a] < mesh.materials[b]; });
    std::vector<uint32_t> indices;
    indices.reserve(mesh.indices.size());
    for (const uint32_t t : order) {
        const uint32_t material = mesh.materials[t];
        if (mesh.parts.empty() || mesh.parts.back().material != material) mesh.parts.push_back({static_cast<uint32_t>(indices.size()), 0, material});
        for (int k = 0; k < 3; ++k) indices.push_back(mesh.indices[t * 3 + k]);
        mesh.parts.back().count += 3;
    }
    mesh.indices.swap(indices);
    mesh.materials.clear();
}

// Edges to outline: where faces meet at more than 30 degrees, and where a surface ends. Points in the same
// place count as one, as a face set often repeats them face by face.
std::vector<uint32_t> feature_edges(const Mesh& mesh) {
    std::vector<uint32_t> canonical(mesh.points.size());
    {
        std::map<std::array<int64_t, 3>, uint32_t> at;
        for (uint32_t v = 0; v < mesh.points.size(); ++v) {
            const std::array<int64_t, 3> key{std::llround(mesh.points[v][0] * 1e5), std::llround(mesh.points[v][1] * 1e5), std::llround(mesh.points[v][2] * 1e5)};
            canonical[v] = at.emplace(key, v).first->second;
        }
    }
    struct Edge {
        uint32_t a, b;
        Vector normal;
        uint32_t faces;
        bool sharp;
    };
    std::unordered_map<uint64_t, Edge> edges;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const uint32_t v[3] = {mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
        const auto normal = normalised(cross(minus(mesh.points[v[1]], mesh.points[v[0]]), minus(mesh.points[v[2]], mesh.points[v[0]])));
        if (!normal) continue;
        for (int k = 0; k < 3; ++k) {
            uint32_t a = canonical[v[k]], b = canonical[v[(k + 1) % 3]];
            if (a == b) continue;
            if (a > b) std::swap(a, b);
            const uint64_t key = (static_cast<uint64_t>(a) << 32) | b;
            auto [edge, added] = edges.try_emplace(key, Edge{a, b, *normal, 1, false});
            if (added) continue;
            ++edge->second.faces;
            if (edge->second.faces == 2 && dot(edge->second.normal, *normal) < FEATURE_COSINE) edge->second.sharp = true;
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> kept;
    for (const auto& [key, edge] : edges) {
        if (edge.faces != 2 || edge.sharp) kept.emplace_back(edge.a, edge.b);
    }
    std::sort(kept.begin(), kept.end());  // the same mesh makes the same bytes
    std::vector<uint32_t> out;
    out.reserve(kept.size() * 2);
    for (const auto& [a, b] : kept) {
        out.push_back(a);
        out.push_back(b);
    }
    return out;
}

std::string encoded(const Mesh& mesh, const std::vector<uint32_t>& edges) {
    const bool wide = mesh.points.size() > 0xFFFF;
    bool floats = false;
    for (int axis = 0; axis < 3; ++axis) floats = floats || (mesh.box.high[axis] - mesh.box.low[axis]) / 65535.0 > QUANTUM_M;
    const uint16_t flags = (wide ? 1 : 0) | (floats ? 2 : 0) | (edges.empty() ? 0 : 8);
    std::string out;
    out.append("IFCM", 4);
    put<uint16_t>(out, 1);
    put<uint16_t>(out, flags);
    put<uint32_t>(out, static_cast<uint32_t>(mesh.points.size()));
    put<uint32_t>(out, static_cast<uint32_t>(mesh.indices.size()));
    put<uint16_t>(out, static_cast<uint16_t>(mesh.parts.size()));
    put<uint16_t>(out, 0);
    for (int axis = 0; axis < 3; ++axis) put<float>(out, static_cast<float>(mesh.box.empty() ? 0 : mesh.box.low[axis]));
    for (int axis = 0; axis < 3; ++axis) put<float>(out, static_cast<float>(mesh.box.empty() ? 0 : mesh.box.high[axis]));
    for (const Part& part : mesh.parts) {
        put<uint32_t>(out, part.first);
        put<uint32_t>(out, part.count);
        put<uint32_t>(out, part.material);
    }
    // Quantized against the box as it's written, in float, so the viewer's sums come out the same
    std::array<float, 3> low{}, span{};
    for (int axis = 0; axis < 3; ++axis) {
        low[axis] = static_cast<float>(mesh.box.empty() ? 0 : mesh.box.low[axis]);
        span[axis] = static_cast<float>(mesh.box.empty() ? 0 : mesh.box.high[axis]) - low[axis];
    }
    for (const Vector& p : mesh.points) {
        for (int axis = 0; axis < 3; ++axis) {
            if (floats) {
                put<float>(out, static_cast<float>(p[axis]));
            } else {
                const double t = span[axis] > 0 ? (p[axis] - low[axis]) / span[axis] : 0;
                put<uint16_t>(out, static_cast<uint16_t>(std::clamp<long>(std::lround(t * 65535.0), 0, 65535)));
            }
        }
    }
    for (const uint32_t i : mesh.indices) {
        if (wide) put<uint32_t>(out, i);
        else put<uint16_t>(out, static_cast<uint16_t>(i));
    }
    if (!edges.empty()) {
        put<uint32_t>(out, static_cast<uint32_t>(edges.size() / 2));
        for (const uint32_t i : edges) put<uint32_t>(out, i);
    }
    return out;
}

struct ArrayHash {
    template <class T, size_t N>
    size_t operator()(const std::array<T, N>& a) const {
        size_t h = 0;
        for (const T& v : a) h = h * 1000003u ^ std::hash<T>{}(v);
        return h;
    }
};

// The mesh with its points snapped together on a grid, each cell's points to their average, for far away where a
// cell is a pixel or two: a pipe's 24 sides become a few, and what's smaller than a cell goes. Triangles left with
// no area go, as do ones the same as another, such as a thin plate's two faces. It stays in the mesh's frame, so
// the same instances place it.
Mesh clustered(const Mesh& mesh, double cell) {
    Mesh out;
    std::unordered_map<std::array<int64_t, 3>, uint32_t, ArrayHash> at;
    std::vector<uint32_t> to(mesh.points.size());
    std::vector<uint32_t> counts;
    for (size_t v = 0; v < mesh.points.size(); ++v) {
        const Vector& p = mesh.points[v];
        const std::array<int64_t, 3> key{static_cast<int64_t>(std::floor(p[0] / cell)), static_cast<int64_t>(std::floor(p[1] / cell)),
                                         static_cast<int64_t>(std::floor(p[2] / cell))};
        const auto [found, added] = at.try_emplace(key, static_cast<uint32_t>(out.points.size()));
        if (added) {
            out.points.push_back({0, 0, 0});
            counts.push_back(0);
        }
        for (int axis = 0; axis < 3; ++axis) out.points[found->second][axis] += p[axis];
        ++counts[found->second];
        to[v] = found->second;
    }
    for (size_t v = 0; v < out.points.size(); ++v) {
        for (double& c : out.points[v]) c /= counts[v];
        out.box.add(out.points[v]);
    }
    std::unordered_set<std::array<uint32_t, 3>, ArrayHash> seen;
    for (const Part& part : mesh.parts) {
        Part kept{static_cast<uint32_t>(out.indices.size()), 0, part.material};
        for (uint32_t i = part.first; i + 2 < part.first + part.count; i += 3) {
            std::array<uint32_t, 3> t{to[mesh.indices[i]], to[mesh.indices[i + 1]], to[mesh.indices[i + 2]]};
            if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
            std::array<uint32_t, 3> sorted = t;
            std::sort(sorted.begin(), sorted.end());
            if (!seen.insert(sorted).second) continue;
            out.indices.insert(out.indices.end(), t.begin(), t.end());
            kept.count += 3;
        }
        if (kept.count) out.parts.push_back(kept);
    }
    return out;
}

// A box from -0.5 to 0.5: what isn't a mesh is drawn as one, scaled to its extent
Mesh unit_box() {
    Mesh mesh;
    for (int i = 0; i < 8; ++i) mesh.points.push_back({i & 1 ? 0.5 : -0.5, i & 2 ? 0.5 : -0.5, i & 4 ? 0.5 : -0.5});
    const uint32_t faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        for (const uint32_t i : {f[0], f[1], f[2], f[0], f[2], f[3]}) mesh.indices.push_back(i);
    }
    finish(mesh, 1);
    return mesh;
}

struct Instance {
    uint32_t element = 0, mesh = 0, material = NO_MATERIAL;
    std::array<Vector, 3> columns{};  // where the mesh's x, y and z go, scale included
    Vector translation{};             // metres, in the world until the origin is known
};

struct MeshInfo {
    Box box;
    uint32_t triangles = 0;
    std::array<uint32_t, LOD_CELLS_M.size() + 1> lods{};  // triangles drawn at each level, the first the full mesh's
};

struct Element {
    uint32_t ifc = 0;
    std::string guid, class_name, name, description, object_type, tag, type_name;
    uint32_t parent = 0;  // the ifc id of what it's in
    bool has_parent = false;
    int64_t sort = 0;
    uint32_t flags = 0;
    Box box;  // metres, in the world until the origin is known
    std::vector<Instance> instances;
    int64_t tile = -1;
};

struct Kind {
    std::string name;  // as the schema has it
    bool product = false, element = false, skipped = false, hidden = false;
};

class Builder {
  public:
    Builder(const std::string& input, Database& db)
        : input_(input), spf_(input), extents_(spf_), db_(db), strings_(db),
          insert_mesh_(db, "INSERT INTO meshes(id, vertices, triangles, minx, miny, minz, maxx, maxy, maxz, lod, blob) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?)"),
          insert_lod_(db, "INSERT INTO mesh_lods(mesh, lod, triangles, blob) VALUES(?, ?, ?, ?)") {}

    void run() {
        const auto start = std::chrono::steady_clock::now();
        schema_name_ = schema_in_header(input_);
        const ifcopenshell::schema_definition* schema = ifcopenshell::schema_by_name(schema_name_.rfind("IFC2X3", 0) == 0 ? "IFC2X3" : "IFC4");
        schema_ = schema;
        product_ = schema->declaration_by_name("IfcProduct")->as_entity();
        element_ = schema->declaration_by_name("IfcElement")->as_entity();
        for (const char* name : {"IfcFeatureElementSubtraction", "IfcVirtualElement", "IfcAnnotation", "IfcGrid"}) {
            try {
                skipped_.push_back(schema->declaration_by_name(name)->as_entity());
            } catch (const std::exception&) {
            }
        }
        unit_ = length_unit(spf_);
        if (unit_ <= 0) {
            std::cerr << "warning: the model's length unit isn't known; taking it as metres\n";
            unit_ = 1;
        }
        read_styles();
        read_elements(*schema);
        read_tree();
        for (Element& element : elements_) shape(element);
        place();
        build_tiles();
        write_materials();
        write_elements();
        write_properties();
        write_tiles();
        db_.exec(INDEXES);
        if (!db_.try_exec("CREATE VIRTUAL TABLE strings_fts USING fts5(text, content='strings', content_rowid='id'); "
                          "INSERT INTO strings_fts(strings_fts) VALUES('rebuild');")) {
            std::cerr << "warning: this SQLite has no fts5, so the cache has no search index\n";
        }
        write_meta();
        seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    void report(std::ostream& out, uintmax_t bytes) const {
        size_t with_geometry = 0, approximate = 0, instances = 0;
        uint64_t triangles = 0;
        for (const Element& element : elements_) {
            if (element.flags & HAS_GEOMETRY) ++with_geometry;
            if (element.flags & APPROXIMATE) ++approximate;
            instances += element.instances.size();
            for (const Instance& instance : element.instances) triangles += meshes_[instance.mesh].triangles;
        }
        out << "  " << elements_.size() << " elements, " << with_geometry << " with geometry, " << approximate << " of them drawn approximately\n";
        out << "  " << meshes_.size() << " meshes, " << instances << " instances, " << triangles << " triangles, " << tile_count_ << " tiles\n";
        if (extruded_) out << "  " << extruded_ << " extrusions made into meshes\n";
        if (lods_stored_) {
            out << "  " << lods_stored_ << " simplified meshes for far away; triangles at each level:";
            for (size_t level = 0; level <= LOD_CELLS_M.size(); ++level) {
                uint64_t at_level = 0;
                for (const Element& element : elements_) {
                    for (const Instance& instance : element.instances) at_level += meshes_[instance.mesh].lods[level];
                }
                out << " " << at_level;
            }
            out << "\n";
        }
        if (empty_) out << "  " << empty_ << " extrusions of no length left out\n";
        if (boxes_) out << "  " << boxes_ << " items drawn as their box, as they aren't meshes:";
        for (const auto& [type, count] : boxed_types_) out << " " << type << " " << count;
        if (boxes_) out << "\n";
        if (unknown_) out << "  " << unknown_ << " items left out, as their extent couldn't be worked out\n";
        if (fanned_) out << "  " << fanned_ << " faces that couldn't be cut into triangles, fanned from their first point\n";
        out << "  " << std::fixed << std::setprecision(1) << bytes / 1e6 << " MB, in " << seconds_ << " s\n";
    }

  private:
    // Colours: each styled item's, and each colour map's

    uint32_t material(double r, double g, double b, double a, const std::string& name) {
        const std::array<long, 4> key{std::lround(r * 1000), std::lround(g * 1000), std::lround(b * 1000), std::lround(a * 1000)};
        const auto seen = material_ids_.find(key);
        if (seen != material_ids_.end()) return seen->second;
        const uint32_t id = static_cast<uint32_t>(materials_.size());
        materials_.push_back({{r, g, b, a}, name});
        material_ids_.emplace(key, id);
        return id;
    }

    std::optional<uint32_t> style_material(uint32_t style, int depth) {
        if (depth > 4 || style >= spf_.max_id()) return std::nullopt;
        const auto type = spf_.type(style);
        const auto attributes = attributes_of(spf_.line(style));
        if (type == "IFCPRESENTATIONSTYLEASSIGNMENT" && !attributes.empty()) {
            for (const uint32_t next : refs_in(attributes[0])) {
                if (const auto found = style_material(next, depth + 1)) return found;
            }
        } else if (type == "IFCSURFACESTYLE" && attributes.size() == 3) {
            const std::string name = text_of(attributes[0]);
            for (const uint32_t next : refs_in(attributes[2])) {
                const auto shading = spf_.type(next);
                if (shading != "IFCSURFACESTYLERENDERING" && shading != "IFCSURFACESTYLESHADING") continue;
                const auto parts = attributes_of(spf_.line(next));
                const auto colour = parts.empty() ? std::vector<uint32_t>{} : refs_in(parts[0]);
                if (colour.size() != 1) continue;
                const auto rgb = attributes_of(spf_.line(colour[0]));
                if (rgb.size() != 4) continue;
                const double transparency = parts.size() > 1 ? std::strtod(std::string(parts[1]).c_str(), nullptr) : 0;
                return material(std::strtod(std::string(rgb[1]).c_str(), nullptr), std::strtod(std::string(rgb[2]).c_str(), nullptr),
                                std::strtod(std::string(rgb[3]).c_str(), nullptr), 1 - std::clamp(transparency, 0.0, 1.0), name);
            }
        }
        return std::nullopt;
    }

    void read_styles() {
        material(0.7, 0.7, 0.7, 1, "Default");
        for (uint32_t id = 0; id < spf_.max_id(); ++id) {
            const auto type = spf_.type(id);
            if (type == "IFCSTYLEDITEM") {
                const auto attributes = attributes_of(spf_.line(id));
                const auto items = attributes.size() == 3 ? refs_in(attributes[0]) : std::vector<uint32_t>{};
                if (items.size() != 1) continue;
                for (const uint32_t style : refs_in(attributes[1])) {
                    if (const auto found = style_material(style, 0)) {
                        item_material_[items[0]] = *found;
                        break;
                    }
                }
            } else if (type == "IFCINDEXEDCOLOURMAP") {
                const auto attributes = attributes_of(spf_.line(id));
                if (attributes.size() != 4) continue;
                const auto target = refs_in(attributes[0]);
                const auto colours = refs_in(attributes[2]);
                if (target.size() != 1 || colours.size() != 1) continue;
                const auto list = attributes_of(spf_.line(colours[0]));
                if (list.empty()) continue;
                const double opacity = attributes[1] == "$" ? 1 : std::strtod(std::string(attributes[1]).c_str(), nullptr);
                std::vector<uint32_t> ids;
                for (const auto& rgb : point_list(list[0])) ids.push_back(material(rgb[0], rgb[1], rgb[2], std::clamp(opacity, 0.0, 1.0), ""));
                std::vector<uint32_t> by_face;
                for (const uint32_t index : indices_in(attributes[3])) by_face.push_back(index >= 1 && index <= ids.size() ? ids[index - 1] : NO_MATERIAL);
                colour_maps_[target[0]] = std::move(by_face);
            }
        }
    }

    // Elements: every product, and the project

    const Kind& kind_of(std::string_view type, const ifcopenshell::schema_definition& schema) {
        const std::string key(type);
        const auto seen = kinds_.find(key);
        if (seen != kinds_.end()) return seen->second;
        Kind kind;
        try {
            const auto* entity = schema.declaration_by_name(key)->as_entity();
            if (entity) {
                kind.name = entity->name();
                kind.product = entity->is(*product_) || key == "IFCPROJECT";
                kind.element = entity->is(*element_);
                kind.skipped = std::any_of(skipped_.begin(), skipped_.end(), [&](const ifcopenshell::entity* s) { return entity->is(*s); });
                kind.hidden = key == "IFCSPACE";
            }
        } catch (const std::exception&) {
        }
        return kinds_.emplace(key, kind).first->second;
    }

    void read_elements(const ifcopenshell::schema_definition& schema) {
        for (uint32_t id = 0; id < spf_.max_id(); ++id) {
            const auto type = spf_.type(id);
            if (type.empty() || type.rfind("IFCREL", 0) == 0) continue;
            const Kind& kind = kind_of(type, schema);
            if (!kind.product || kind.skipped) continue;
            const auto attributes = attributes_of(spf_.line(id));
            if (attributes.size() < 4) continue;
            Element element;
            element.ifc = id;
            element.guid = text_of(attributes[0]);
            element.class_name = kind.name;
            element.name = text_of(attributes[2]);
            element.description = text_of(attributes[3]);
            if (attributes.size() > 4) element.object_type = text_of(attributes[4]);
            if (kind.element && attributes.size() > 7) element.tag = text_of(attributes[7]);
            if (kind.hidden) element.flags |= HIDDEN;
            index_[id] = static_cast<uint32_t>(elements_.size());
            elements_.push_back(std::move(element));
        }
    }

    Element* element_at(uint32_t ifc) {
        const auto found = index_.find(ifc);
        return found == index_.end() ? nullptr : &elements_[found->second];
    }

    // What each element is in: the whole it's part of, or else its container
    void read_tree() {
        std::unordered_map<uint32_t, int64_t> children;
        std::vector<std::pair<uint32_t, uint32_t>> contained;  // element, structure
        const auto set_parent = [&](uint32_t child, uint32_t parent) {
            Element* element = element_at(child);
            if (!element || element->has_parent || !element_at(parent)) return;
            element->parent = parent;
            element->has_parent = true;
            element->sort = children[parent]++;
        };
        for (uint32_t id = 0; id < spf_.max_id(); ++id) {
            const auto type = spf_.type(id);
            if (type.rfind("IFCREL", 0) != 0) continue;
            const auto attributes = attributes_of(spf_.line(id));
            if (attributes.size() < 6) continue;
            if (type == "IFCRELAGGREGATES" || type == "IFCRELNESTS") {
                const auto whole = refs_in(attributes[4]);
                if (whole.size() != 1) continue;
                for (const uint32_t part : refs_in(attributes[5])) set_parent(part, whole[0]);
            } else if (type == "IFCRELCONTAINEDINSPATIALSTRUCTURE") {
                const auto structure = refs_in(attributes[5]);
                if (structure.size() != 1) continue;
                for (const uint32_t element : refs_in(attributes[4])) contained.emplace_back(element, structure[0]);
            } else if (type == "IFCRELCONNECTSPORTTOELEMENT") {
                const auto port = refs_in(attributes[4]);
                const auto element = refs_in(attributes[5]);
                if (port.size() == 1 && element.size() == 1) set_parent(port[0], element[0]);
            } else if (type == "IFCRELDEFINESBYTYPE") {
                const auto type_object = refs_in(attributes[5]);
                if (type_object.size() != 1) continue;
                const auto type_attributes = attributes_of(spf_.line(type_object[0]));
                const std::string type_name = type_attributes.size() > 2 ? text_of(type_attributes[2]) : std::string();
                for (const uint32_t object : refs_in(attributes[4])) {
                    if (Element* element = element_at(object)) element->type_name = type_name;
                }
            }
        }
        for (const auto& [element, structure] : contained) set_parent(element, structure);
    }

    // Shapes

    const std::pair<int64_t, Vector>& mesh_of(uint32_t item, std::string_view type) {
        const auto seen = item_meshes_.find(item);
        if (seen != item_meshes_.end()) return seen->second;
        Mesh mesh;
        bool built = false;
        if (type == "IFCTRIANGULATEDFACESET") built = triangulated(item, mesh);
        else if (type == "IFCPOLYGONALFACESET") built = polygonal(item, mesh);
        else built = faceted(item, type, mesh);
        if (!built || mesh.indices.empty()) return item_meshes_.emplace(item, std::make_pair(int64_t(-1), Vector{})).first->second;
        finish(mesh, unit_);
        const Vector centre = mesh.centre;
        const bool approximate = mesh.approximate;
        const uint32_t id = stored(mesh);
        if (approximate) approximate_meshes_.insert(id);
        return item_meshes_.emplace(item, std::make_pair(int64_t(id), centre)).first->second;
    }

    // Writes a mesh, unless the same one is there already, and says which it is
    uint32_t stored(const Mesh& mesh) {
        const std::string raw = encoded(mesh, feature_edges(mesh));
        const auto same = mesh_ids_.find(raw);
        if (same != mesh_ids_.end()) return same->second;
        const uint32_t id = static_cast<uint32_t>(meshes_.size());
        insert_mesh_.integer(1, id).integer(2, static_cast<int64_t>(mesh.points.size())).integer(3, static_cast<int64_t>(mesh.indices.size() / 3));
        for (int axis = 0; axis < 3; ++axis) insert_mesh_.real(4 + axis, mesh.box.low[axis]).real(7 + axis, mesh.box.high[axis]);
        insert_mesh_.blob(10, compressed(raw)).run();
        const uint32_t triangles = static_cast<uint32_t>(mesh.indices.size() / 3);
        meshes_.push_back({mesh.box, triangles, {}});
        meshes_.back().lods.fill(triangles);
        mesh_ids_.emplace(raw, id);
        if (triangles >= LOD_MIN_TRIANGLES) store_lods(id, mesh);
        return id;
    }

    // Each level from the full mesh, kept where it's enough fewer triangles than the level before; one with none
    // at all is kept with no blob, as the mesh isn't drawn at that distance. Levels not kept draw the one before.
    void store_lods(uint32_t id, const Mesh& mesh) {
        auto& lods = meshes_[id].lods;
        for (size_t level = 1; level <= LOD_CELLS_M.size(); ++level) {
            const Mesh coarse = clustered(mesh, LOD_CELLS_M[level - 1]);
            const uint32_t triangles = static_cast<uint32_t>(coarse.indices.size() / 3);
            if (triangles > LOD_WORTH * lods[level - 1]) {
                lods[level] = lods[level - 1];
                continue;
            }
            insert_lod_.integer(1, id).integer(2, static_cast<int64_t>(level)).integer(3, triangles);
            if (triangles) insert_lod_.blob(4, compressed(encoded(coarse, {})));
            else insert_lod_.null(4);
            insert_lod_.run();
            for (size_t rest = level; rest < lods.size(); ++rest) lods[rest] = triangles;
            ++lods_stored_;
            if (!triangles) break;  // nothing coarser than nothing
        }
    }

    // Faces that can't be cut into triangles are fanned, and the mesh marked approximate
    void add_polygon(Mesh& mesh, const std::vector<uint32_t>& ring, const std::vector<std::vector<uint32_t>>& holes, uint32_t material) {
        std::vector<std::array<uint32_t, 3>> triangles;
        if (ring.size() < 3) return;
        if (!triangulate_polygon(ring, holes, mesh.points, triangles)) {
            triangles.clear();
            const bool in_range = std::all_of(ring.begin(), ring.end(), [&](uint32_t i) { return i >= 1 && i <= mesh.points.size(); });
            if (!in_range) return;
            for (size_t i = 1; i + 1 < ring.size(); ++i) triangles.push_back({ring[0], ring[i], ring[i + 1]});
            mesh.approximate = true;
            ++fanned_;
        }
        for (const auto& t : triangles) {
            for (const uint32_t i : t) mesh.indices.push_back(i - 1);
            mesh.materials.push_back(material);
        }
    }

    std::vector<Vector> points_of(std::string_view coordinates) {
        const auto ids = refs_in(coordinates);
        if (ids.size() != 1) return {};
        const auto list = attributes_of(spf_.line(ids[0]));
        return list.empty() ? std::vector<Vector>{} : point_list(list[0]);
    }

    bool triangulated(uint32_t item, Mesh& mesh) {
        const auto attributes = attributes_of(spf_.line(item));
        if (attributes.size() < 4) return false;
        mesh.points = points_of(attributes[0]);
        const auto pn = attributes.size() > 4 && attributes[4] != "$" ? indices_in(attributes[4]) : std::vector<uint32_t>{};
        const auto indices = indices_in(attributes[3]);
        const auto colours = colour_maps_.find(item);
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            uint32_t v[3];
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                uint32_t i = indices[t + k];
                if (!pn.empty()) i = i >= 1 && i <= pn.size() ? pn[i - 1] : 0;
                ok = ok && i >= 1 && i <= mesh.points.size();
                v[k] = i - 1;
            }
            if (!ok) continue;
            mesh.indices.insert(mesh.indices.end(), v, v + 3);
            const size_t face = t / 3;
            mesh.materials.push_back(colours != colour_maps_.end() && face < colours->second.size() ? colours->second[face] : NO_MATERIAL);
        }
        return true;
    }

    bool polygonal(uint32_t item, Mesh& mesh) {
        const auto attributes = attributes_of(spf_.line(item));
        if (attributes.size() < 3) return false;
        mesh.points = points_of(attributes[0]);
        const auto pn = attributes.size() > 3 && attributes[3] != "$" ? indices_in(attributes[3]) : std::vector<uint32_t>{};
        const auto mapped = [&](std::vector<uint32_t> loop) {
            if (!pn.empty()) {
                for (uint32_t& i : loop) i = i >= 1 && i <= pn.size() ? pn[i - 1] : 0;
            }
            return loop;
        };
        const auto colours = colour_maps_.find(item);
        const auto faces = refs_in(attributes[2]);
        for (size_t f = 0; f < faces.size(); ++f) {
            const auto parts = attributes_of(spf_.line(faces[f]));
            if (parts.empty()) continue;
            std::vector<std::vector<uint32_t>> holes;
            if (spf_.type(faces[f]) == "IFCINDEXEDPOLYGONALFACEWITHVOIDS" && parts.size() > 1 && parts[1].size() > 2) {
                for (const auto loop : attributes_of(trimmed(parts[1]).substr(1, trimmed(parts[1]).size() - 2))) holes.push_back(mapped(indices_in(loop)));
            }
            const uint32_t material = colours != colour_maps_.end() && f < colours->second.size() ? colours->second[f] : NO_MATERIAL;
            add_polygon(mesh, mapped(indices_in(parts[0])), holes, material);
        }
        return true;
    }

    // Faceted breps, and surface models made of the same faces
    bool faceted(uint32_t item, std::string_view type, Mesh& mesh) {
        const auto attributes = attributes_of(spf_.line(item));
        if (attributes.empty()) return false;
        std::vector<uint32_t> shells;
        if (type == "IFCFACETEDBREP" || type == "IFCFACETEDBREPWITHVOIDS") shells = refs_in(attributes[0]).size() == 1 ? refs_in(attributes[0]) : std::vector<uint32_t>{};
        else if (type == "IFCSHELLBASEDSURFACEMODEL" || type == "IFCFACEBASEDSURFACEMODEL") shells = refs_in(attributes[0]);
        else return false;
        std::unordered_map<uint32_t, uint32_t> point_index;  // ifc id, and its index from 1
        const auto loop_of = [&](uint32_t bound, bool& reversed) {
            std::vector<uint32_t> loop;
            const auto parts = attributes_of(spf_.line(bound));
            if (parts.size() != 2) return loop;
            reversed = trimmed(parts[1]) == ".F.";
            const auto polyloop = refs_in(parts[0]);
            if (polyloop.size() != 1 || spf_.type(polyloop[0]) != "IFCPOLYLOOP") return loop;
            const auto points = attributes_of(spf_.line(polyloop[0]));
            if (points.empty()) return loop;
            for (const uint32_t point : refs_in(points[0])) {
                auto [at, added] = point_index.try_emplace(point, static_cast<uint32_t>(mesh.points.size() + 1));
                if (added) {
                    const auto coordinates = attributes_of(spf_.line(point));
                    const auto read = coordinates.empty() ? std::vector<Vector>{} : point_list(coordinates[0]);
                    mesh.points.push_back(read.empty() ? Vector{} : read[0]);
                }
                loop.push_back(at->second);
            }
            if (reversed) std::reverse(loop.begin(), loop.end());
            return loop;
        };
        for (const uint32_t shell : shells) {
            const auto shell_attributes = attributes_of(spf_.line(shell));
            if (shell_attributes.empty()) continue;
            for (const uint32_t face : refs_in(shell_attributes[0])) {
                const auto face_attributes = attributes_of(spf_.line(face));
                if (face_attributes.empty()) continue;
                // The outer bound is the one that says so, or else the first
                const auto bounds = refs_in(face_attributes[0]);
                size_t outer_at = 0;
                for (size_t b = 0; b < bounds.size(); ++b) {
                    if (spf_.type(bounds[b]) == "IFCFACEOUTERBOUND") {
                        outer_at = b;
                        break;
                    }
                }
                std::vector<uint32_t> outer;
                std::vector<std::vector<uint32_t>> holes;
                for (size_t b = 0; b < bounds.size(); ++b) {
                    bool reversed = false;
                    auto loop = loop_of(bounds[b], reversed);
                    if (loop.size() < 3) continue;
                    if (b == outer_at) outer = std::move(loop);
                    else holes.push_back(std::move(loop));
                }
                if (outer.empty() && !holes.empty()) {
                    outer = std::move(holes.front());
                    holes.erase(holes.begin());
                }
                add_polygon(mesh, outer, holes, NO_MATERIAL);
            }
        }
        return true;
    }

    // Extrusions

    static double number_of(std::string_view text) {
        const std::string written(trimmed(text));
        return std::strtod(written.c_str(), nullptr);
    }

    // A point, or a direction's ratios, from the line a reference leads to
    Vector vector_at(std::string_view reference) {
        const auto ids = refs_in(reference);
        if (ids.size() != 1) return {};
        const auto attributes = attributes_of(spf_.line(ids[0]));
        return attributes.empty() ? Vector{} : numbers_in(attributes[0]);
    }

    // An IfcAxis2Placement2D's move and turn applied to a profile's loops; none leaves them as they are
    void place_2d(std::string_view position, std::vector<std::vector<Vector>>& loops) {
        const auto ids = refs_in(position);
        if (ids.size() != 1 || spf_.type(ids[0]) != "IFCAXIS2PLACEMENT2D") return;
        const auto attributes = attributes_of(spf_.line(ids[0]));
        if (attributes.empty()) return;
        const Vector origin = vector_at(attributes[0]);
        Vector x{1, 0, 0};
        if (attributes.size() > 1 && trimmed(attributes[1]) != "$") {
            if (const auto turned = normalised(vector_at(attributes[1]))) x = *turned;
        }
        for (auto& loop : loops) {
            for (Vector& p : loop) p = {origin[0] + x[0] * p[0] - x[1] * p[1], origin[1] + x[1] * p[0] + x[0] * p[1], 0};
        }
    }

    // A circle or ellipse as a loop, with enough sides that it looks round at its size
    std::vector<Vector> ellipse(double a, double b) {
        const double around = 2 * M_PI * std::max(a, b) * unit_;
        const int sides = std::clamp(static_cast<int>(std::ceil(around / 0.03)), 16, 64);
        std::vector<Vector> loop;
        for (int i = 0; i < sides; ++i) {
            const double t = 2 * M_PI * i / sides;
            loop.push_back({a * std::cos(t), b * std::sin(t), 0});
        }
        return loop;
    }

    static std::vector<Vector> rectangle(double x, double y) {
        return {{-x / 2, -y / 2, 0}, {x / 2, -y / 2, 0}, {x / 2, y / 2, 0}, {-x / 2, y / 2, 0}};
    }

    // The arc from a through b to c, without a, as points
    void arc(const Vector& a, const Vector& b, const Vector& c, std::vector<Vector>& out) {
        // The centre is where the perpendicular bisectors of ab and bc meet
        const double d = 2 * (a[0] * (b[1] - c[1]) + b[0] * (c[1] - a[1]) + c[0] * (a[1] - b[1]));
        if (std::abs(d) < 1e-12) {
            out.push_back(b);
            out.push_back(c);
            return;
        }
        const double aa = a[0] * a[0] + a[1] * a[1], bb = b[0] * b[0] + b[1] * b[1], cc = c[0] * c[0] + c[1] * c[1];
        const double cx = (aa * (b[1] - c[1]) + bb * (c[1] - a[1]) + cc * (a[1] - b[1])) / d;
        const double cy = (aa * (c[0] - b[0]) + bb * (a[0] - c[0]) + cc * (b[0] - a[0])) / d;
        const double radius = std::hypot(a[0] - cx, a[1] - cy);
        const auto angle = [&](const Vector& p) { return std::atan2(p[1] - cy, p[0] - cx); };
        const double start = angle(a);
        double through = angle(b) - start, end = angle(c) - start;
        while (through < 0) through += 2 * M_PI;
        while (end < 0) end += 2 * M_PI;
        if (through > end) end -= 2 * M_PI;  // the other way round
        const int sides = std::clamp(static_cast<int>(std::ceil(std::abs(end) * radius * unit_ / 0.03)), 2, 64);
        for (int i = 1; i <= sides; ++i) {
            const double t = start + end * i / sides;
            out.push_back({cx + radius * std::cos(t), cy + radius * std::sin(t), 0});
        }
    }

    // A closed curve's points, without the last repeating the first; empty for a kind not read yet
    std::vector<Vector> curve_points(std::string_view reference) {
        std::vector<Vector> loop;
        const auto ids = refs_in(reference);
        if (ids.size() != 1) return loop;
        const auto type = spf_.type(ids[0]);
        const auto attributes = attributes_of(spf_.line(ids[0]));
        if (type == "IFCPOLYLINE" && !attributes.empty()) {
            for (const uint32_t point : refs_in(attributes[0])) {
                const auto coordinates = attributes_of(spf_.line(point));
                if (!coordinates.empty()) loop.push_back(numbers_in(coordinates[0]));
            }
        } else if (type == "IFCINDEXEDPOLYCURVE" && !attributes.empty()) {
            const auto list = refs_in(attributes[0]);
            const auto list_attributes = list.size() == 1 ? attributes_of(spf_.line(list[0])) : std::vector<std::string_view>{};
            const auto points = list_attributes.empty() ? std::vector<Vector>{} : point_list(list_attributes[0]);
            const auto at = [&](uint32_t i) { return i >= 1 && i <= points.size() ? points[i - 1] : Vector{}; };
            if (attributes.size() < 2 || trimmed(attributes[1]) == "$") {
                loop = points;
            } else {
                // Each segment is IFCLINEINDEX((i,j,...)) or IFCARCINDEX((i,j,k))
                const std::string_view segments = attributes[1];
                for (size_t from = 0; (from = segments.find("IFC", from)) != std::string_view::npos;) {
                    const bool is_arc = segments.substr(from, 11) == "IFCARCINDEX";
                    const size_t open = segments.find('(', from), close = segments.find(')', open);
                    if (open == std::string_view::npos || close == std::string_view::npos) break;
                    const auto indices = indices_in(segments.substr(open, close - open + 1));
                    if (loop.empty() && !indices.empty()) loop.push_back(at(indices[0]));
                    if (is_arc && indices.size() == 3) arc(at(indices[0]), at(indices[1]), at(indices[2]), loop);
                    else for (size_t i = 1; i < indices.size(); ++i) loop.push_back(at(indices[i]));
                    from = close;
                }
            }
        }
        while (loop.size() > 1 && std::hypot(loop.front()[0] - loop.back()[0], loop.front()[1] - loop.back()[1]) < 1e-9) loop.pop_back();
        for (Vector& p : loop) p[2] = 0;
        return loop;
    }

    // A profile as loops in its plane: the outer first, then any holes; empty for a kind not read yet
    std::vector<std::vector<Vector>> profile_loops(uint32_t profile) {
        const auto type = spf_.type(profile);
        const auto a = attributes_of(spf_.line(profile));
        std::vector<std::vector<Vector>> loops;
        if (type == "IFCCIRCLEPROFILEDEF" && a.size() == 4) {
            loops.push_back(ellipse(number_of(a[3]), number_of(a[3])));
        } else if (type == "IFCCIRCLEHOLLOWPROFILEDEF" && a.size() == 5) {
            const double radius = number_of(a[3]), wall = number_of(a[4]);
            loops.push_back(ellipse(radius, radius));
            if (wall > 0 && wall < radius) loops.push_back(ellipse(radius - wall, radius - wall));
        } else if (type == "IFCELLIPSEPROFILEDEF" && a.size() == 5) {
            loops.push_back(ellipse(number_of(a[3]), number_of(a[4])));
        } else if (type == "IFCRECTANGLEPROFILEDEF" && a.size() == 5) {
            loops.push_back(rectangle(number_of(a[3]), number_of(a[4])));
        } else if (type == "IFCRECTANGLEHOLLOWPROFILEDEF" && a.size() >= 6) {
            const double x = number_of(a[3]), y = number_of(a[4]), wall = number_of(a[5]);
            loops.push_back(rectangle(x, y));
            if (wall > 0 && 2 * wall < std::min(x, y)) loops.push_back(rectangle(x - 2 * wall, y - 2 * wall));
        } else if (type == "IFCARBITRARYCLOSEDPROFILEDEF" && a.size() == 3) {
            loops.push_back(curve_points(a[2]));
        } else if (type == "IFCARBITRARYPROFILEDEFWITHVOIDS" && a.size() == 4) {
            loops.push_back(curve_points(a[2]));
            for (const uint32_t inner : refs_in(a[3])) loops.push_back(curve_points(reference(inner)));
        }
        if (loops.empty() || loops[0].size() < 3) return {};
        // Parameterised profiles have a position of their own, the third attribute
        if (type != "IFCARBITRARYCLOSEDPROFILEDEF" && type != "IFCARBITRARYPROFILEDEFWITHVOIDS") place_2d(a[2], loops);
        return loops;
    }

    // An IfcExtrudedAreaSolid as a mesh in its own position's coordinates, and that position
    bool extruded(uint32_t item, Mesh& mesh, Rigid& position) {
        const auto a = attributes_of(spf_.line(item));
        if (a.size() != 4) return false;
        const auto profile = refs_in(a[0]);
        if (profile.size() != 1) return false;
        auto loops = profile_loops(profile[0]);
        if (loops.empty()) return false;
        const auto placement = refs_in(a[1]);
        if (!placement.empty()) {
            const auto read = axis_placement(spf_, placement[0]);
            if (!read) return false;
            position = *read;
        }
        const auto direction = normalised(vector_at(a[2]));
        const double depth = number_of(a[3]);
        if (!direction || depth <= 0) return false;
        const Vector along{(*direction)[0] * depth, (*direction)[1] * depth, (*direction)[2] * depth};

        // The outer loop anticlockwise and holes clockwise, seen from where the extrusion goes
        const auto area = [](const std::vector<Vector>& loop) {
            double twice = 0;
            for (size_t i = 0; i < loop.size(); ++i) {
                const Vector& p = loop[i];
                const Vector& q = loop[(i + 1) % loop.size()];
                twice += p[0] * q[1] - q[0] * p[1];
            }
            return twice;
        };
        const bool upward = (*direction)[2] >= 0;
        for (size_t l = 0; l < loops.size(); ++l) {
            if (loops[l].size() < 3) continue;
            const bool wrong_way = (area(loops[l]) > 0) != (l == 0);
            if (wrong_way == upward) std::reverse(loops[l].begin(), loops[l].end());
        }

        // Each loop's points at the bottom, then at the top
        std::vector<uint32_t> first;
        for (const auto& loop : loops) {
            first.push_back(static_cast<uint32_t>(mesh.points.size()));
            for (const Vector& p : loop) mesh.points.push_back(p);
            for (const Vector& p : loop) mesh.points.push_back({p[0] + along[0], p[1] + along[1], p[2] + along[2]});
        }
        // The ends, indices from 1 as add_polygon takes them; the bottom faces back down the extrusion
        for (const bool top : {false, true}) {
            std::vector<uint32_t> ring;
            std::vector<std::vector<uint32_t>> holes;
            for (size_t l = 0; l < loops.size(); ++l) {
                if (loops[l].size() < 3) continue;
                const uint32_t n = static_cast<uint32_t>(loops[l].size());
                std::vector<uint32_t> loop;
                for (uint32_t i = 0; i < n; ++i) loop.push_back(first[l] + (top ? n : 0) + i + 1);
                if (!top) std::reverse(loop.begin(), loop.end());
                if (l == 0) ring = std::move(loop);
                else holes.push_back(std::move(loop));
            }
            add_polygon(mesh, ring, holes, NO_MATERIAL);
        }
        // The sides
        for (size_t l = 0; l < loops.size(); ++l) {
            const uint32_t n = static_cast<uint32_t>(loops[l].size());
            if (n < 3) continue;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t b0 = first[l] + i, b1 = first[l] + (i + 1) % n, t0 = b0 + n, t1 = b1 + n;
                for (const uint32_t v : {b0, b1, t1, b0, t1, t0}) mesh.indices.push_back(v);
                mesh.materials.push_back(NO_MATERIAL);
                mesh.materials.push_back(NO_MATERIAL);
            }
        }
        return true;
    }

    struct Swept {
        int64_t mesh = -1;
        Vector centre{};
        Rigid position;
        bool empty = false;  // no length, so nothing to draw
    };

    const Swept& swept_of(uint32_t item) {
        const auto seen = swept_.find(item);
        if (seen != swept_.end()) return seen->second;
        Swept swept;
        const auto attributes = attributes_of(spf_.line(item));
        swept.empty = attributes.size() == 4 && number_of(attributes[3]) <= 0;
        Mesh mesh;
        if (!swept.empty && extruded(item, mesh, swept.position) && !mesh.indices.empty()) {
            finish(mesh, unit_);
            swept.centre = mesh.centre;
            const bool approximate = mesh.approximate;
            swept.mesh = stored(mesh);
            if (approximate) approximate_meshes_.insert(static_cast<uint32_t>(swept.mesh));
            ++extruded_;
        }
        return swept_.emplace(item, swept).first->second;
    }

    void add_instance(Element& element, uint32_t mesh, const Rigid& placed, const Vector& centre, uint32_t material) {
        Instance instance;
        instance.element = index_[element.ifc];
        instance.mesh = mesh;
        instance.material = material;
        for (int axis = 0; axis < 3; ++axis) instance.columns[axis] = placed.axes[axis];
        Rigid rotation = placed;
        rotation.origin = {0, 0, 0};
        const Vector turned = applied(rotation, centre);
        for (int axis = 0; axis < 3; ++axis) instance.translation[axis] = turned[axis] + placed.origin[axis] * unit_;
        element.instances.push_back(instance);
        if (approximate_meshes_.count(mesh)) element.flags |= APPROXIMATE;
    }

    void walk(Element& element, uint32_t item, const Rigid& placed, uint32_t material, int depth) {
        if (depth > MAX_DEPTH || item >= spf_.max_id()) return;
        const auto own = item_material_.find(item);
        if (own != item_material_.end()) material = own->second;
        const auto type = spf_.type(item);
        const auto attributes = attributes_of(spf_.line(item));
        if (type == "IFCMAPPEDITEM" && attributes.size() == 2) {
            const auto map = refs_in(attributes[0]);
            const auto target = refs_in(attributes[1]);
            const auto parts = map.size() == 1 ? attributes_of(spf_.line(map[0])) : std::vector<std::string_view>{};
            const auto origin_ids = parts.size() == 2 ? refs_in(parts[0]) : std::vector<uint32_t>{};
            const auto representation = parts.size() == 2 ? refs_in(parts[1]) : std::vector<uint32_t>{};
            const auto operation = target.size() == 1 ? operator_of(spf_, target[0]) : std::nullopt;
            const auto origin = origin_ids.size() == 1 ? axis_placement(spf_, origin_ids[0]) : std::nullopt;
            if (operation && origin && representation.size() == 1) {
                const Rigid inner = compose(placed, compose(*operation, inverse(*origin)));
                const auto held = attributes_of(spf_.line(representation[0]));
                if (held.size() == 4) {
                    for (const uint32_t next : refs_in(held[3])) walk(element, next, inner, material, depth + 1);
                }
                return;
            }
            box(element, item, placed, material, type);  // one that scales or mirrors
            return;
        }
        if ((type == "IFCBOOLEANRESULT" || type == "IFCBOOLEANCLIPPINGRESULT") && attributes.size() == 3) {
            // What's taken away is left on, for now
            const auto first = refs_in(attributes[1]);
            if (first.size() == 1) walk(element, first[0], placed, material, depth + 1);
            element.flags |= APPROXIMATE;
            return;
        }
        if (type == "IFCTRIANGULATEDFACESET" || type == "IFCPOLYGONALFACESET" || type == "IFCFACETEDBREP" || type == "IFCFACETEDBREPWITHVOIDS" ||
            type == "IFCSHELLBASEDSURFACEMODEL" || type == "IFCFACEBASEDSURFACEMODEL") {
            const auto& [mesh, centre] = mesh_of(item, type);
            if (mesh >= 0) add_instance(element, static_cast<uint32_t>(mesh), placed, centre, material);
            return;
        }
        if (type == "IFCEXTRUDEDAREASOLID") {
            const Swept& swept = swept_of(item);
            if (swept.empty) {
                ++empty_;
                return;
            }
            if (swept.mesh >= 0) {
                add_instance(element, static_cast<uint32_t>(swept.mesh), compose(placed, swept.position), swept.centre, material);
                return;
            }
        }
        box(element, item, placed, material, type);
    }

    // What isn't a mesh is drawn as its box in the world, for now
    void box(Element& element, uint32_t item, const Rigid& placed, uint32_t material, std::string_view type) {
        Box reach;
        if (!extents_.item(item, placed, reach) || reach.empty()) {
            ++unknown_;
            return;
        }
        if (!box_mesh_) box_mesh_ = stored(unit_box());
        Instance instance;
        instance.element = index_[element.ifc];
        instance.mesh = *box_mesh_;
        instance.material = material;
        for (int axis = 0; axis < 3; ++axis) {
            instance.columns[axis] = {0, 0, 0};
            instance.columns[axis][axis] = std::max((reach.high[axis] - reach.low[axis]) * unit_, 0.001);
            instance.translation[axis] = (reach.high[axis] + reach.low[axis]) / 2 * unit_;
        }
        element.instances.push_back(instance);
        element.flags |= APPROXIMATE;
        ++boxes_;
        ++boxed_types_[kind_name(type)];
    }

    std::string kind_name(std::string_view type) {
        try {
            return schema_->declaration_by_name(std::string(type))->name();
        } catch (const std::exception&) {
            return std::string(type);
        }
    }

    void shape(Element& element) {
        const auto attributes = attributes_of(spf_.line(element.ifc));
        if (attributes.size() < 7 || attributes[6] == "$") return;
        const auto placed = extents_.placement(attributes[5]);
        if (!placed) return;
        const auto definition = refs_in(attributes[6]);
        if (definition.size() != 1) return;
        const auto shape_attributes = attributes_of(spf_.line(definition[0]));
        if (shape_attributes.size() != 3) return;
        uint32_t chosen = 0;
        for (const uint32_t representation : refs_in(shape_attributes[2])) {
            const auto parts = attributes_of(spf_.line(representation));
            if (parts.size() != 4) continue;
            const std::string identifier = text_of(parts[1]);
            if (identifier == "Body") {
                chosen = representation;
                break;
            }
            if (identifier == "Facetation" && !chosen) chosen = representation;
        }
        if (!chosen) return;
        for (const uint32_t item : refs_in(attributes_of(spf_.line(chosen))[3])) walk(element, item, *placed, NO_MATERIAL, 0);
    }

    // The origin is the middle of everything, to the metre; then boxes and moves are taken from it

    static Vector moved(const Instance& instance, const Vector& p) {
        Vector out = instance.translation;
        for (int axis = 0; axis < 3; ++axis) {
            for (int k = 0; k < 3; ++k) out[k] += instance.columns[axis][k] * p[axis];
        }
        return out;
    }

    void place() {
        for (Element& element : elements_) {
            for (const Instance& instance : element.instances) {
                const Box& box = meshes_[instance.mesh].box;
                if (box.empty()) continue;
                for (int corner = 0; corner < 8; ++corner) {
                    element.box.add(moved(instance, {corner & 1 ? box.high[0] : box.low[0], corner & 2 ? box.high[1] : box.low[1], corner & 4 ? box.high[2] : box.low[2]}));
                }
            }
            if (!element.instances.empty()) element.flags |= HAS_GEOMETRY;
            if (!element.box.empty()) model_box_.add(element.box);
        }
        if (!model_box_.empty()) {
            for (int axis = 0; axis < 3; ++axis) origin_[axis] = std::round((model_box_.low[axis] + model_box_.high[axis]) / 2);
        }
        const auto shift = [&](Box& box) {
            if (box.empty()) return;
            for (int axis = 0; axis < 3; ++axis) {
                box.low[axis] -= origin_[axis];
                box.high[axis] -= origin_[axis];
            }
        };
        for (Element& element : elements_) {
            shift(element.box);
            for (Instance& instance : element.instances) {
                for (int axis = 0; axis < 3; ++axis) instance.translation[axis] -= origin_[axis];
            }
        }
        shift(model_box_);
    }

    // Tiles: a loose octree. An element goes down while it's no bigger than the next node's cell, into
    // the one its middle is in, and a node is only split when it holds too much.

    struct Tile {
        int64_t parent = -1;
        int level = 0;
        Box box;  // of everything in it and below it
        double size = 0;
        std::vector<uint32_t> elements;
    };

    static double largest_side(const Box& box) {
        return std::max({box.high[0] - box.low[0], box.high[1] - box.low[1], box.high[2] - box.low[2]});
    }

    size_t build_tile(int64_t parent, int level, const Vector& centre, double half, std::vector<uint32_t> members) {
        const size_t id = tiles_.size();
        tiles_.emplace_back();
        tiles_[id].parent = parent;
        tiles_[id].level = level;
        size_t instances = 0;
        uint64_t triangles = 0;
        for (const uint32_t e : members) {
            instances += elements_[e].instances.size();
            for (const Instance& instance : elements_[e].instances) triangles += meshes_[instance.mesh].triangles;
            tiles_[id].box.add(elements_[e].box);
        }
        std::array<std::vector<uint32_t>, 8> children;
        std::vector<uint32_t> kept;
        if ((instances > TILE_INSTANCES || triangles > TILE_TRIANGLES) && level < TILE_DEPTH) {
            for (const uint32_t e : members) {
                const Box& box = elements_[e].box;
                if (largest_side(box) > half) {
                    kept.push_back(e);
                    continue;
                }
                int octant = 0;
                for (int axis = 0; axis < 3; ++axis) {
                    if ((box.low[axis] + box.high[axis]) / 2 >= centre[axis]) octant |= 1 << axis;
                }
                children[octant].push_back(e);
            }
        } else {
            kept = std::move(members);
        }
        for (const uint32_t e : kept) {
            elements_[e].tile = static_cast<int64_t>(id);
            tiles_[id].size = std::max(tiles_[id].size, std::sqrt(std::pow(elements_[e].box.high[0] - elements_[e].box.low[0], 2) +
                                                                  std::pow(elements_[e].box.high[1] - elements_[e].box.low[1], 2) +
                                                                  std::pow(elements_[e].box.high[2] - elements_[e].box.low[2], 2)));
        }
        tiles_[id].elements = std::move(kept);
        for (int octant = 0; octant < 8; ++octant) {
            if (children[octant].empty()) continue;
            Vector child_centre = centre;
            for (int axis = 0; axis < 3; ++axis) child_centre[axis] += (octant & (1 << axis) ? 0.5 : -0.5) * half;
            build_tile(static_cast<int64_t>(id), level + 1, child_centre, half / 2, std::move(children[octant]));
        }
        return id;
    }

    void build_tiles() {
        std::vector<uint32_t> members;
        for (uint32_t e = 0; e < elements_.size(); ++e) {
            if (!elements_[e].instances.empty() && !elements_[e].box.empty()) members.push_back(e);
        }
        if (members.empty()) return;
        Vector centre{};
        for (int axis = 0; axis < 3; ++axis) centre[axis] = (model_box_.low[axis] + model_box_.high[axis]) / 2;
        build_tile(-1, 0, centre, largest_side(model_box_) / 2 * 1.0001 + 0.001, std::move(members));
        tile_count_ = tiles_.size();
    }

    // Writing

    static void bind_box(Statement& statement, int first, const Box& box) {
        for (int axis = 0; axis < 3; ++axis) {
            if (box.empty()) {
                statement.null(first + axis).null(first + 3 + axis);
            } else {
                statement.real(first + axis, box.low[axis]).real(first + 3 + axis, box.high[axis]);
            }
        }
    }

    void write_materials() {
        Statement insert(db_, "INSERT INTO materials(id, r, g, b, a, name) VALUES(?, ?, ?, ?, ?, ?)");
        for (size_t id = 0; id < materials_.size(); ++id) {
            const auto& [rgba, name] = materials_[id];
            insert.integer(1, static_cast<int64_t>(id)).real(2, rgba[0]).real(3, rgba[1]).real(4, rgba[2]).real(5, rgba[3]);
            if (name.empty()) insert.null(6);
            else insert.text(6, name);
            insert.run();
        }
    }

    void write_elements() {
        Statement insert(db_, "INSERT INTO elements(id, guid, class, name, description, object_type, tag, parent, sort, type_name, "
                              "minx, miny, minz, maxx, maxy, maxz, tile, flags) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        std::unordered_set<std::string> guids;
        for (uint32_t id = 0; id < elements_.size(); ++id) {
            const Element& element = elements_[id];
            insert.integer(1, id);
            // A GUID repeated in the model is left off all but the first, so it stays a key
            if (element.guid.empty() || !guids.insert(element.guid).second) insert.null(2);
            else insert.text(2, element.guid);
            insert.string_id(3, strings_.id(element.class_name)).string_id(4, strings_.id(element.name)).string_id(5, strings_.id(element.description));
            insert.string_id(6, strings_.id(element.object_type)).string_id(7, strings_.id(element.tag));
            if (element.has_parent) insert.integer(8, index_[element.parent]);
            else insert.null(8);
            insert.integer(9, element.sort).string_id(10, strings_.id(element.type_name));
            bind_box(insert, 11, element.box);
            if (element.tile >= 0) insert.integer(17, element.tile);
            else insert.null(17);
            insert.integer(18, element.flags).run();
        }
    }

    int64_t value_id(const Value& value) {
        std::ostringstream key;
        key << value.type << '|' << value.text << '|' << std::setprecision(17) << value.number << '|' << value.numeric;
        const auto seen = value_ids_.find(key.str());
        if (seen != value_ids_.end()) return seen->second;
        const int64_t id = static_cast<int64_t>(value_ids_.size());
        insert_value_->integer(1, id).integer(2, value.type).string_id(3, strings_.id(value.text));
        if (value.numeric) insert_value_->real(4, value.number);
        else insert_value_->null(4);
        insert_value_->null(5).run();
        value_ids_.emplace(key.str(), id);
        return id;
    }

    // A property or quantity set, written the first time it's used; false if it isn't one
    bool write_pset(uint32_t pset, int kind) {
        if (written_psets_.count(pset)) return true;
        const auto type = spf_.type(pset);
        const auto attributes = attributes_of(spf_.line(pset));
        std::vector<std::pair<std::string, Value>> values;
        if (type == "IFCPROPERTYSET" && attributes.size() == 5) {
            for (const uint32_t property : refs_in(attributes[4])) {
                const auto parts = attributes_of(spf_.line(property));
                const auto property_type = spf_.type(property);
                if (property_type == "IFCPROPERTYSINGLEVALUE" && parts.size() >= 3) {
                    if (auto value = value_in(parts[2])) values.emplace_back(text_of(parts[0]), std::move(*value));
                } else if (property_type == "IFCPROPERTYENUMERATEDVALUE" && parts.size() >= 3 && parts[2].size() > 2) {
                    Value value;
                    value.type = 4;
                    for (const auto each : attributes_of(trimmed(parts[2]).substr(1, trimmed(parts[2]).size() - 2))) {
                        const auto one = value_in(each);
                        if (one) value.text += (value.text.empty() ? "" : ", ") + (one->text.empty() ? std::string(trimmed(each)) : one->text);
                    }
                    values.emplace_back(text_of(parts[0]), std::move(value));
                }
            }
        } else if (type == "IFCELEMENTQUANTITY" && attributes.size() == 6) {
            kind = 1;
            for (const uint32_t quantity : refs_in(attributes[5])) {
                const auto parts = attributes_of(spf_.line(quantity));
                if (parts.size() < 4 || spf_.type(quantity).rfind("IFCQUANTITY", 0) != 0) continue;
                const std::string number(trimmed(parts[3]));
                char* end = nullptr;
                Value value;
                value.type = 1;
                value.number = std::strtod(number.c_str(), &end);
                value.numeric = end != number.c_str();
                if (value.numeric) values.emplace_back(text_of(parts[0]), value);
            }
        } else {
            return false;
        }
        written_psets_.insert(pset);
        insert_pset_->integer(1, pset).string_id(2, strings_.id(text_of(attributes[2]))).integer(3, kind).run();
        for (const auto& [name, value] : values) insert_pset_value_->integer(1, pset).string_id(2, strings_.id(name)).integer(3, value_id(value)).run();
        return true;
    }

    std::string material_names(uint32_t id, int depth) {
        if (depth > 4 || id >= spf_.max_id()) return {};
        const auto type = spf_.type(id);
        const auto attributes = attributes_of(spf_.line(id));
        if (type == "IFCMATERIAL") return attributes.empty() ? std::string() : text_of(attributes[0]);
        std::string out;
        for (const auto attribute : attributes) {
            for (const uint32_t next : refs_in(attribute)) {
                const std::string names = material_names(next, depth + 1);
                if (!names.empty() && out.find(names) == std::string::npos) out += (out.empty() ? "" : "; ") + names;
            }
        }
        return out;
    }

    void write_properties() {
        Statement insert_pset(db_, "INSERT INTO psets(id, name, kind) VALUES(?, ?, ?)");
        Statement insert_pset_value(db_, "INSERT OR IGNORE INTO pset_values(pset, name, value) VALUES(?, ?, ?)");
        Statement insert_value(db_, "INSERT INTO \"values\"(id, type, text, number, unit) VALUES(?, ?, ?, ?, ?)");
        Statement link(db_, "INSERT OR IGNORE INTO element_psets(element, pset) VALUES(?, ?)");
        insert_pset_ = &insert_pset;
        insert_pset_value_ = &insert_pset_value;
        insert_value_ = &insert_value;

        std::unordered_map<uint32_t, std::vector<uint32_t>> type_psets;  // a type object's own sets
        std::vector<std::pair<uint32_t, uint32_t>> typed;                 // element, type object
        for (uint32_t id = 0; id < spf_.max_id(); ++id) {
            const auto type = spf_.type(id);
            if (type.rfind("IFCREL", 0) != 0) continue;
            const auto attributes = attributes_of(spf_.line(id));
            if (attributes.size() < 6) continue;
            if (type == "IFCRELDEFINESBYPROPERTIES") {
                const auto pset = refs_in(attributes[5]);
                if (pset.size() != 1) continue;
                for (const uint32_t object : refs_in(attributes[4])) {
                    const auto found = index_.find(object);
                    if (found != index_.end() && write_pset(pset[0], 0)) link.integer(1, found->second).integer(2, pset[0]).run();
                }
            } else if (type == "IFCRELDEFINESBYTYPE") {
                const auto type_object = refs_in(attributes[5]);
                if (type_object.size() != 1) continue;
                for (const uint32_t object : refs_in(attributes[4])) typed.emplace_back(object, type_object[0]);
            } else if (type == "IFCRELASSOCIATESMATERIAL") {
                const auto relating = refs_in(attributes[5]);
                if (relating.size() != 1) continue;
                const std::string names = material_names(relating[0], 0);
                if (names.empty()) continue;
                // A set of its own, numbered past the model's ids
                const uint32_t pset = spf_.max_id() + id;
                bool written = false;
                for (const uint32_t object : refs_in(attributes[4])) {
                    const auto found = index_.find(object);
                    if (found == index_.end()) continue;
                    if (!written) {
                        insert_pset.integer(1, pset).string_id(2, strings_.id("Material")).integer(3, 3).run();
                        Value value;
                        value.text = names;
                        insert_pset_value.integer(1, pset).string_id(2, strings_.id("Material")).integer(3, value_id(value)).run();
                        written = true;
                    }
                    link.integer(1, found->second).integer(2, pset).run();
                }
            }
        }
        for (const auto& [object, type_object] : typed) {
            const auto found = index_.find(object);
            if (found == index_.end()) continue;
            const auto attributes = attributes_of(spf_.line(type_object));
            if (attributes.size() < 6 || attributes[5] == "$") continue;
            for (const uint32_t pset : refs_in(attributes[5])) {
                if (write_pset(pset, 2)) link.integer(1, found->second).integer(2, pset).run();
            }
        }
        insert_pset_ = insert_pset_value_ = insert_value_ = nullptr;
    }

    void write_tiles() {
        Statement insert(db_, "INSERT INTO tiles(id, parent, level, minx, miny, minz, maxx, maxy, maxz, size, instances, triangles, meshes, blob) "
                              "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        Statement insert_lod(db_, "INSERT INTO tile_lods(tile, lod, triangles) VALUES(?, ?, ?)");
        // A tile's box takes in what's below it, so the viewer can skip a whole branch out of view
        for (size_t id = tiles_.size(); id-- > 1;) tiles_[static_cast<size_t>(tiles_[id].parent)].box.add(tiles_[id].box);
        for (size_t id = 0; id < tiles_.size(); ++id) {
            const Tile& tile = tiles_[id];
            std::vector<const Instance*> instances;
            for (const uint32_t e : tile.elements) {
                for (const Instance& instance : elements_[e].instances) instances.push_back(&instance);
            }
            std::stable_sort(instances.begin(), instances.end(), [](const Instance* a, const Instance* b) { return a->mesh < b->mesh; });
            std::string blob, meshes;
            blob.append("IFCI", 4);
            put<uint16_t>(blob, 1);
            put<uint16_t>(blob, 0);
            put<uint32_t>(blob, static_cast<uint32_t>(instances.size()));
            uint64_t triangles = 0;
            std::array<uint64_t, LOD_CELLS_M.size() + 1> lod_triangles{};
            int64_t last_mesh = -1;
            for (const Instance* instance : instances) {
                put<uint32_t>(blob, instance->element);
                put<uint32_t>(blob, instance->mesh);
                put<uint32_t>(blob, instance->material);
                put<uint32_t>(blob, 0);
                for (const Vector& column : instance->columns) {
                    for (const double c : column) put<float>(blob, static_cast<float>(c));
                }
                for (const double c : instance->translation) put<float>(blob, static_cast<float>(c));
                triangles += meshes_[instance->mesh].triangles;
                for (size_t level = 0; level < lod_triangles.size(); ++level) lod_triangles[level] += meshes_[instance->mesh].lods[level];
                if (static_cast<int64_t>(instance->mesh) != last_mesh) put<uint32_t>(meshes, instance->mesh);
                last_mesh = instance->mesh;
            }
            insert.integer(1, static_cast<int64_t>(id));
            if (tile.parent >= 0) insert.integer(2, tile.parent);
            else insert.null(2);
            insert.integer(3, tile.level);
            bind_box(insert, 4, tile.box);
            insert.real(10, tile.size).integer(11, static_cast<int64_t>(instances.size())).integer(12, static_cast<int64_t>(triangles));
            insert.blob(13, meshes).blob(14, compressed(blob)).run();
            for (size_t level = 1; level < lod_triangles.size(); ++level) {
                insert_lod.integer(1, static_cast<int64_t>(id)).integer(2, static_cast<int64_t>(level)).integer(3, static_cast<int64_t>(lod_triangles[level])).run();
            }
        }
    }

    void write_meta() {
        Statement insert(db_, "INSERT INTO meta(key, value) VALUES(?, ?)");
        const auto set = [&](const char* key, const std::string& value) { insert.text(1, key).text(2, value).run(); };
        const auto numbers = [](std::initializer_list<double> values) {
            std::ostringstream out;
            out << std::setprecision(15);
            for (const double v : values) out << (out.tellp() > 0 ? " " : "") << v;
            return out.str();
        };
        const std::time_t now = std::time(nullptr);
        std::tm parts{};
#ifdef _WIN32
        gmtime_s(&parts, &now);
#else
        gmtime_r(&now, &parts);
#endif
        char created[32];
        std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &parts);
        uint64_t triangles = 0;
        for (const Element& element : elements_) {
            for (const Instance& instance : element.instances) triangles += meshes_[instance.mesh].triangles;
        }
        set("format", "ifcc");
        set("format_version", std::to_string(FORMAT_VERSION));
        set("generator", std::string("IFCFilter ") + IFCFILTER_VERSION);
        set("created", created);
        set("source_name", fs::path(input_).filename().string());
        set("source_sha256", sha256_of_file(input_));
        set("schema", schema_name_);
        set("origin", numbers({origin_[0], origin_[1], origin_[2]}));
        if (!model_box_.empty()) {
            set("bbox", numbers({model_box_.low[0], model_box_.low[1], model_box_.low[2], model_box_.high[0], model_box_.high[1], model_box_.high[2]}));
        }
        set("element_count", std::to_string(elements_.size()));
        set("mesh_count", std::to_string(meshes_.size()));
        set("tile_count", std::to_string(tiles_.size()));
        set("triangle_count", std::to_string(triangles));
        set("lod_cells", numbers({LOD_CELLS_M[0], LOD_CELLS_M[1], LOD_CELLS_M[2]}));
    }

    const std::string input_;
    SpfFile spf_;
    Extents extents_;
    Database& db_;
    Strings strings_;
    Statement insert_mesh_, insert_lod_;
    Statement* insert_pset_ = nullptr;
    Statement* insert_pset_value_ = nullptr;
    Statement* insert_value_ = nullptr;

    std::string schema_name_;
    const ifcopenshell::schema_definition* schema_ = nullptr;
    const ifcopenshell::entity* product_ = nullptr;
    const ifcopenshell::entity* element_ = nullptr;
    std::vector<const ifcopenshell::entity*> skipped_;
    std::unordered_map<std::string, Kind> kinds_;
    double unit_ = 1;

    std::vector<std::pair<std::array<double, 4>, std::string>> materials_;
    std::map<std::array<long, 4>, uint32_t> material_ids_;
    std::unordered_map<uint32_t, uint32_t> item_material_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> colour_maps_;  // face set, and each face's material

    std::vector<Element> elements_;
    std::unordered_map<uint32_t, uint32_t> index_;  // ifc id, and element id

    std::vector<MeshInfo> meshes_;
    std::unordered_map<std::string, uint32_t> mesh_ids_;  // a mesh's bytes, and its id
    std::unordered_map<uint32_t, std::pair<int64_t, Vector>> item_meshes_;  // item, and its mesh and centre; -1 if it has none
    std::unordered_set<uint32_t> approximate_meshes_;
    std::optional<uint32_t> box_mesh_;

    Vector origin_{};
    Box model_box_;
    std::vector<Tile> tiles_;
    size_t tile_count_ = 0;

    std::unordered_set<uint32_t> written_psets_;
    std::unordered_map<std::string, int64_t> value_ids_;

    size_t boxes_ = 0, unknown_ = 0, fanned_ = 0, extruded_ = 0, empty_ = 0, lods_stored_ = 0;
    std::unordered_map<uint32_t, Swept> swept_;
    std::map<std::string, size_t> boxed_types_;
    double seconds_ = 0;
};

}  // namespace

int cache(const std::string& input, const std::string& output) {
    try {
        const std::string partial = output + ".partial";
        std::error_code ignored;
        fs::remove(partial, ignored);
        uintmax_t bytes = 0;
        {
            Database db(partial);
            db.exec("PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;");
            db.exec(TABLES);
            db.exec("BEGIN");
            Builder builder(input, db);
            builder.run();
            db.exec("COMMIT");
            db.exec("VACUUM");
            bytes = fs::file_size(partial);
            std::cout << output << "\n";
            builder.report(std::cout, bytes);
        }
        replace_file(partial, output);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << input << ": " << e.what() << "\n";
        return 1;
    }
}

}  // namespace ifcfilter

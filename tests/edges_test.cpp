// Checks of the edge rule (src/edges.h): which edges go, on small meshes made here. Run by ctest.

#include "edges.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ifcfilter;
using Edges = std::vector<std::pair<uint32_t, uint32_t>>;

static int failures = 0;

static void check(const char* name, Edges got, Edges want) {
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    if (got == want) return;
    ++failures;
    std::printf("FAIL %s: kept", name);
    for (const auto& [a, b] : got) std::printf(" %u-%u", a, b);
    std::printf(", wanted");
    for (const auto& [a, b] : want) std::printf(" %u-%u", a, b);
    std::printf("\n");
}

// A square from (x, y), size across, at height z, as two triangles; its outline
static Edges square(std::vector<EdgePoint>& p, std::vector<uint32_t>& i, double x, double y, double size, double z) {
    const auto s = static_cast<uint32_t>(p.size());
    p.insert(p.end(), {{x, y, z}, {x + size, y, z}, {x + size, y + size, z}, {x, y + size, z}});
    i.insert(i.end(), {s, s + 1, s + 2, s, s + 2, s + 3});
    return {{s, s + 1}, {s + 1, s + 2}, {s + 2, s + 3}, {s + 3, s}};
}

int main() {
    {
        // A flat diagonal goes, the outline stays
        std::vector<EdgePoint> p;
        std::vector<uint32_t> i;
        const Edges outline = square(p, i, 0, 0, 2, 0);
        Edges all = outline;
        all.push_back({0, 2});
        check("flat diagonal", flat_edges_out(p, i, all), outline);
        // The same faces again facing down: still only the outline
        i.insert(i.end(), {0, 2, 1, 0, 3, 2});
        check("surface both ways up", flat_edges_out(p, i, all), outline);
    }
    {
        // Two faces at 90 degrees stay; at 10 degrees they go
        std::vector<EdgePoint> p{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const std::vector<uint32_t> i{0, 1, 2, 0, 3, 1};
        check("fold", flat_edges_out(p, i, {{0, 1}}), {{0, 1}});
        p[3] = {0, -1, std::tan(10 * 3.14159265358979 / 180)};
        check("shallow fold", flat_edges_out(p, i, {{0, 1}}), {});
        // A long sliver below the edge, its far corner half a millimetre up
        p = {{0, 0, 0}, {10, 0, 0}, {5, 1, 0}, {5, -0.01, 0.0005}};
        check("sliver", flat_edges_out(p, i, {{0, 1}}), {});
    }
    {
        // A 2 x 1 face, and below it two 1 x 1 faces meeting it at a T at x = 1
        const std::vector<EdgePoint> p{{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}, {1, 0, 0}, {0, -1, 0}, {1, -1, 0}, {2, -1, 0}};
        const std::vector<uint32_t> i{0, 1, 2, 0, 2, 3, 5, 6, 4, 5, 4, 0, 6, 7, 1, 6, 1, 4};
        const Edges outline{{1, 2}, {2, 3}, {3, 0}, {0, 5}, {5, 6}, {6, 7}, {7, 1}};
        Edges all{{0, 1}, {0, 4}, {4, 1}};
        all.insert(all.end(), outline.begin(), outline.end());
        check("T-junction", flat_edges_out(p, i, all), outline);
    }
    {
        // A piece lying on a face loses its outline; pieces a centimetre apart keep theirs
        std::vector<EdgePoint> p;
        std::vector<uint32_t> i;
        const Edges big = square(p, i, 0, 0, 10, 0);
        Edges all = big;
        for (const auto& e : square(p, i, 2, 2, 1, 0)) all.push_back(e);
        check("piece lying on a face", flat_edges_out(p, i, all), big);
        p.clear(), i.clear();
        Edges apart = square(p, i, 0, 0, 10, 0);
        for (const auto& e : square(p, i, 10.01, 0, 10, 0)) apart.push_back(e);
        check("pieces apart", flat_edges_out(p, i, apart), apart);
    }
    if (failures) return 1;
    std::printf("edges: all passed\n");
    return 0;
}

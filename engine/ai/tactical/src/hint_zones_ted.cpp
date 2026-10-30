#include "alamo/ai/tactical/hint_zones.hpp"
#include "alamo/terrain/ted.hpp"

#include <utility>

namespace alamo::ai::tactical {

std::vector<HintZone> hint_zones_from_map(const terrain::MapData& map) {
    std::vector<HintZone> out;
    out.reserve(map.zones.size());
    for (const terrain::AiZone& z : map.zones) {
        HintZone h;
        h.id = z.id;
        h.name = z.name;
        for (int i = 0; i < 3; ++i) h.center[i] = z.center[i];
        h.links.reserve(z.links.size());
        for (const terrain::AiZone::Link& l : z.links) h.links.push_back({l.zone, l.kind});
        h.cells.reserve(z.cells.size());
        for (const terrain::AiZone::Cell& c : z.cells)
            h.cells.push_back(static_cast<uint32_t>(c.x) | (static_cast<uint32_t>(c.y) << 16));
        out.push_back(std::move(h));
    }
    return out;
}

bool map_bounds(const terrain::MapData& map, Rect2& out) {
    float mn[2] = {0.0f, 0.0f}, mx[2] = {0.0f, 0.0f};
    if (!map.playable_box(mn, mx)) return false;
    out = {mn[0], mn[1], mx[0], mx[1]};
    return true;
}

}  // namespace alamo::ai::tactical

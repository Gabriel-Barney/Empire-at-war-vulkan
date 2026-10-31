#include "alamo/ai/tactical/hint_zones.hpp"
#include "alamo/terrain/ted.hpp"

#include <algorithm>
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
        // A1_READINGS §22: hint map from header minis 08 (name) + 09 (u32 value) [M: pairing = an 08 followed by its 09].
        std::string pending;
        bool have_name = false;
        for (const terrain::RawMini& m : z.raw) {
            if (m.id == 0x08) {
                pending.assign(m.data.begin(), std::find(m.data.begin(), m.data.end(), uint8_t{0}));
                have_name = true;
            } else if (m.id == 0x09 && have_name && m.data.size() >= 4) {
                const uint32_t v = static_cast<uint32_t>(m.data[0]) | (static_cast<uint32_t>(m.data[1]) << 8) |
                                   (static_cast<uint32_t>(m.data[2]) << 16) | (static_cast<uint32_t>(m.data[3]) << 24);
                h.hints.push_back({pending, v});
                have_name = false;
            }
        }
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

bool space_region_bounds(const terrain::MapData& map, Rect2& out) {
    if (!map.is_space()) return false;
    const terrain::SpaceMapDesc d = map.space.present ? map.space : terrain::SpaceMapDesc{};
    out = {d.camera_min[0], d.camera_min[1], d.camera_max[0], d.camera_max[1]};
    return true;
}

}  // namespace alamo::ai::tactical

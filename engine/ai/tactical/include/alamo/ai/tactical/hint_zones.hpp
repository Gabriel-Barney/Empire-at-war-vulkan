// A1 - AI hint zones of a land map (docs/re/AI_TACTICAL.md 3.2; MOVEMENT.md "LandZonePathFinderClass
// (zones from .ted 0x10B)").
//
// The land mode's hint-zone list (+0x3a0) is filled by the map loader with AIHintZoneClass objects.
// [M] This port takes them from the map's 0x10B AI-zone chunk (engine/terrain AiZone), in chunk
// order: name (01), id (02), centre (05), links (child E: zone id + kind), cells (child 7, packed
// x | y << 16). A map without 0x10B zones has no hint zones and therefore no land Tactical_Location
// targets [H].
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace alamo::terrain {
struct MapData;
}

namespace alamo::ai::tactical {

struct Rect2 {
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    friend bool operator==(const Rect2&, const Rect2&) = default;
};

struct HintZone {
    uint32_t id = 0;
    std::string name;
    float center[3] = {0.0f, 0.0f, 0.0f};
    struct Link {
        uint32_t zone = 0;  // target zone id (not index)
        uint32_t kind = 0;  // meaning not established; kept for the edge-weight function
    };
    std::vector<Link> links;
    std::vector<uint32_t> cells;  // passability cells (20 x 20 world units), packed x | y << 16, chunk order
    // Hints (A1_READINGS §22): the zone's hint map from header minis 08 (name) / 09 (u32 value), in chunk order. No
    // shipped FoC zone has any (36 EaW zones do); a zone without hints has an empty Hints evaluator.
    struct Hint {
        std::string name;
        uint32_t value = 0;
    };
    std::vector<Hint> hints;
};

// Position -> zone (A1_READINGS §4, land mode vf48 0x3b7f50 / vf55 0x3b7c10) [H]: cell = ((int)((x + 10) / 20),
// (int)((y + 10) / 20)) truncated; the first zone in list order whose cell set holds (cx | cy << 16). A cell outside
// the u16 cell range or held by no zone gives -1 [M: the passability-grid bound is read as "no zone holds the cell"].
class ZoneLocator {
public:
    void build(const std::vector<HintZone>& zones);
    int32_t zone_at(float x, float y) const;
    // Passability cell of a world point, -1 outside the u16 range (packed cx | cy << 16 otherwise).
    static int64_t cell_of(float x, float y);

private:
    std::vector<std::pair<uint32_t, int32_t>> first_;  // sorted (packed cell, first zone index)
};

#if defined(ALAMO_AI_TACTICAL_HAVE_TERRAIN)
// Hint zones of a parsed map (parse_ted with ParseOptions::zones = true), in chunk order.
std::vector<HintZone> hint_zones_from_map(const terrain::MapData& map);
// Map bounds used for the space region grid and region rects: MapData::playable_box (land 0x103
// 05/06 or the terrain extent; space 0x103 04/05, default +-5000). [M]: the retail space mode's
// bounds getter (mode vtable +0x208) is assumed to return the playable box. False if none.
bool map_bounds(const terrain::MapData& map, Rect2& out);
// Space region-grid bounds (A1_READINGS §8 [H]): the space camera box (0x103 minis 07/08, SpaceMapDesc camera_min/max;
// loader default = the playable box shrunk by 400 in X and Y). False for a land map.
bool space_region_bounds(const terrain::MapData& map, Rect2& out);
#endif

}  // namespace alamo::ai::tactical

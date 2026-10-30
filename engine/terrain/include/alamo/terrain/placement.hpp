// alamo_terrain - map object placements resolved to models (TERRAIN_WATER_POST.md 1.6).
//
// A placement stores the CRC-32 of the upper-cased GameObjectType name (alamo::name_crc), never
// the name. Resolving it needs the game database, which this library does not depend on: callers
// pass a TypeResolver (engine/data GameDatabase in the test / app; see the test for the
// reference implementation: first definition by CRC, model = Land_Model_Name for land maps,
// Space_Model_Name for space maps, falling back to the other and to Galactic_Model_Name, following
// Variant_Of_Existing_Type when the variant has no model of its own; Scale_Factor likewise).
//
// What a placement needs for drawing:
//   * position: 0x4B0 mini 04 as stored. Its z is the absolute world height (the editor snapped it
//     when the map was saved); the runtime does not re-snap static props at load as far as the doc
//     shows, so the default is "use z as stored". PlacementOptions::snap_to_terrain replaces z with
//     Terrain::sample_height(x, y) (useful for edited maps).
//   * rotation: mini 05, Euler degrees; z = yaw (the only non-zero component in vanilla maps).
//     Matrix R = Rz(z) * Ry(y) * Rx(x) ([L] for the x/y order, irrelevant for vanilla data).
//   * scale: the type's Scale_Factor (XML, default 1). Mini 03 is NOT a scale: it is the starting
//     health fraction (health / max tactical health), exposed as initial_health_fraction for the
//     sim (non-finite -> 1.0 with a warning). Some vanilla props store < 1 (Endor mineral pads 0.08).
//   * owner: mini 02 (player slot; team colour), not part of the transform.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "alamo/assets/math.hpp"
#include "alamo/terrain/ted.hpp"

namespace alamo::terrain {

struct ResolvedType {
    std::string type_name;     // GameObjectType name as written in the XML
    std::string model_name;    // .ALO file name (as written, e.g. "W_Tree_Endor_01.alo"); may be empty
    float scale_factor = 1.0f; // <Scale_Factor>
};
// Returns false when the CRC names no known type.
using TypeResolver = std::function<bool(uint32_t type_crc, bool space_map, ResolvedType& out)>;

struct PlacementOptions {
    bool snap_to_terrain = false;
    bool keep_without_model = false;  // also emit placements whose type has no model (markers etc.)
};

struct ResolvedPlacement {
    uint32_t object_index = 0;   // into MapData::objects
    uint32_t type_crc = 0;
    std::string type_name, model_name;
    assets::Mat3x4 transform;    // model -> world
    float position[3] = {0, 0, 0};
    float scale = 1.0f;                    // type Scale_Factor
    float initial_health_fraction = 1.0f;  // mini 03, validated (sim: health = this * max health)
    uint32_t owner = 0;
};

struct PlacementResult {
    std::vector<ResolvedPlacement> placements;  // object order
    std::vector<uint32_t> unknown_type;         // object indices whose CRC the resolver did not know
    std::vector<uint32_t> no_model;             // object indices whose type has no model
    // Distinct unknown CRCs with their counts, ascending CRC.
    std::vector<std::pair<uint32_t, uint32_t>> unknown_crcs;
};

assets::Mat3x4 placement_transform(const float (&position)[3], const float (&rotation_deg)[3], float scale);
PlacementResult resolve_placements(const MapData& map, const TypeResolver& resolver, const PlacementOptions& options = {});

}  // namespace alamo::terrain

// Data-oriented tables for the galactic (campaign) map. Filled by engine/data from XML,
// consumed by engine/galactic (path finding) and the simulation.
//
// Row order is the campaign's <Locations> order (planets) and <Trade_Routes> order (routes):
// deterministic and identical on every machine, which matters for lockstep.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {

// Also defined (token-identical, same guard) in tools/formats common.hpp, so both headers can share a TU.
#ifndef ALAMO_VEC3_DEFINED
#define ALAMO_VEC3_DEFINED
struct Vec3 { float x = 0, y = 0, z = 0; };
#endif

using PlanetIndex = uint32_t;
inline constexpr PlanetIndex kInvalidPlanet = 0xFFFFFFFFu;

enum PlanetFlags : uint32_t {
    kPlanetInBackground = 1u << 0,   // <In_Background>yes (e.g. Galaxy_Core_Art_Model): scenery
    kPlanetAlwaysInstantiate = 1u << 1,
    // <Behavior> lists PLANET. The original's planet query (both path finders) keeps only objects
    // with a PLANET behaviour slot and an owner id != -1 (docs/re/PHASE2_ANSWERS.md Q1); scenery
    // such as Galaxy_Core_Art_Model (Behavior IDLE) is excluded by this, not by In_Background.
    kPlanetHasPlanetBehavior = 1u << 2,
};

// Structure-of-arrays: one row per planet.
struct PlanetTable {
    std::vector<std::string> name;          // as written in Planets XML
    std::vector<uint32_t> name_crc;         // alamo::name_crc(name)
    std::vector<Vec3> position;             // <Galactic_Position>
    std::vector<uint32_t> flags;            // PlanetFlags
    std::vector<int32_t> credit_value;      // <Planet_Credit_Value>
    std::vector<int32_t> max_space_base;    // <Max_Space_Base>
    std::vector<int32_t> max_ground_base;   // <Max_Ground_Base>

    size_t size() const { return name.size(); }
    PlanetIndex add(std::string_view planet_name);  // appends a default row, returns its index
    PlanetIndex find(std::string_view planet_name) const;  // case-insensitive; kInvalidPlanet if absent
};

struct TradeRouteTable {
    std::vector<std::string> name;
    std::vector<uint32_t> name_crc;
    std::vector<PlanetIndex> point_a;
    std::vector<PlanetIndex> point_b;
    std::vector<float> hs_speed_factor;        // <HS_Speed_Factor>, travel-cost divisor
    std::vector<float> political_control_gain;
    std::vector<float> credit_gain_factor;

    size_t size() const { return name.size(); }
    uint32_t add(std::string_view route_name);
};

// One campaign's galactic map, ready for simulation/pathing.
struct GalaxyMap {
    std::string campaign_name;
    PlanetTable planets;
    TradeRouteTable routes;
    float max_fleet_move_distance = 100.0f;  // GameConstants.MaximumFleetMovementDistance

    // Cheap structural checks (indices in range, no self routes, finite positions). Empty == OK.
    std::vector<std::string> validate() const;
};

}  // namespace alamo

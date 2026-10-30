// A1 - shared constants of the tactical AI (docs/re/AI_TACTICAL.md 3.2, 3.4, 6.3).
#pragma once
#include <cstdint>

namespace alamo::data {
struct GameConstants;
}

namespace alamo::ai::tactical {

inline constexpr int32_t kFramesPerSecond = 30;
// Reachability "unreachable" cost (ZoneReachabilityClass slot 7 returns 1e18) [H].
inline constexpr double kUnreachable = 1.0e18;

enum class TacticalMode : uint8_t { Land = 1, Space = 2 };  // context mode type (1 land, 2 space) [H]

// GameConstants values the tactical AI reads. Defaults are the shipped FoC values (the engine's
// defaults when a tag is missing were not read [L]).
struct TacticalAiConstants {
    float space_region_size = 2000.0f;             // AI_SpaceEvaluatorRegionSize (+0x10d0)
    float land_region_size = 2000.0f;              // AI_LandEvaluatorRegionSize (+0x10d4)
    float low_threat_tolerance = 1000.0f;          // Low_Threat_Reachability_Tolerance (+0x157c)
    float medium_threat_tolerance = 3000.0f;       // Medium_Threat_Reachability_Tolerance (+0x1580)
    float high_threat_tolerance = 5000.0f;         // High_Threat_Reachability_Tolerance (+0x1584)
    float space_threat_decay_step = 0.05f;         // AI_SpaceThreatDecayStep (+0x10fc), DAMAGE_TRACKING decay

    // Threat grid / perception (docs/re/AI_TACTICAL_PERCEPTION.md 3.3, 4). Defaults = shipped values;
    // the range caps are not in the shipped XML (constructor defaults 0x2c91c0).
    int32_t fog_cells_per_threat_cell = 4;         // AI_FogCellsPerThreatCell (+0x10f8)
    float desired_land_fow_cell_size = 15.0f;      // DesiredLandFOWCellSize (+0x11c0)
    float desired_space_fow_cell_size = 100.0f;    // DesiredSpaceFOWCellSize (+0x11c4)
    float space_threat_look_ahead_time = 0.0f;     // AI_SpaceThreatLookAheadTime (+0x10e8)
    float land_threat_look_ahead_time = 0.0f;      // AI_LandThreatLookAheadTime (+0x10ec)
    float space_area_threat_scale = 1.0f;          // AI_SpaceAreaThreatScaleFactor (+0x10f0)
    float land_area_threat_scale = 1.0f;           // AI_LandAreaThreatScaleFactor (+0x10f4)
    float space_threat_range_cap = 1000.0f;        // AI_SpaceThreatRangeCap (+0x1104)
    float land_threat_range_cap = 500.0f;          // AI_LandThreatRangeCap (+0x1108)
    bool ai_uses_fow_space = false;                // AIUsesFogOfWarSpace (+0x2ed), shipped False
    bool ai_uses_fow_land = false;                 // AIUsesFogOfWarLand (+0x2ee), shipped False
    // A2 tactical (docs/re/AI_GALACTIC_3.md §3.1, §8): free-store eligibility and the tactical population caps.
    float health_low_percent_threshold = 0.33f;    // Health_Low_Percent_Threshold (+0x11B38)
    int32_t max_ground_forces_on_planet = 10;      // Max_Ground_Forces_On_Planet (+0x10C8)
    float allow_reinforcement_percentage = 0.0f;   // Allow_Reinforcement_Percentage_Normalized (+0x10CC)
    // S10 AI land reinforcement (TACTICAL_GAPS §4.2): land drops only near allied reinforcement points.
    bool use_reinforcement_points = true;          // Use_Reinforcement_Points (+0x118a0), True in FoC and TR

    // Reads the tags above (missing tags keep the current value).
    void load(const data::GameConstants& gc);
};

}  // namespace alamo::ai::tactical

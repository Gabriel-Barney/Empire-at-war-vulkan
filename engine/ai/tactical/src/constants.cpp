#include "alamo/ai/tactical/constants.hpp"

#include "alamo/data/game_database.hpp"

namespace alamo::ai::tactical {

void TacticalAiConstants::load(const data::GameConstants& gc) {
    space_region_size = gc.get_float("AI_SpaceEvaluatorRegionSize", space_region_size);
    land_region_size = gc.get_float("AI_LandEvaluatorRegionSize", land_region_size);
    low_threat_tolerance = gc.get_float("Low_Threat_Reachability_Tolerance", low_threat_tolerance);
    medium_threat_tolerance = gc.get_float("Medium_Threat_Reachability_Tolerance", medium_threat_tolerance);
    high_threat_tolerance = gc.get_float("High_Threat_Reachability_Tolerance", high_threat_tolerance);
    space_threat_decay_step = gc.get_float("AI_SpaceThreatDecayStep", space_threat_decay_step);
    fog_cells_per_threat_cell = gc.get_int("AI_FogCellsPerThreatCell", fog_cells_per_threat_cell);
    desired_land_fow_cell_size = gc.get_float("DesiredLandFOWCellSize", desired_land_fow_cell_size);
    desired_space_fow_cell_size = gc.get_float("DesiredSpaceFOWCellSize", desired_space_fow_cell_size);
    space_threat_look_ahead_time = gc.get_float("AI_SpaceThreatLookAheadTime", space_threat_look_ahead_time);
    land_threat_look_ahead_time = gc.get_float("AI_LandThreatLookAheadTime", land_threat_look_ahead_time);
    space_area_threat_scale = gc.get_float("AI_SpaceAreaThreatScaleFactor", space_area_threat_scale);
    land_area_threat_scale = gc.get_float("AI_LandAreaThreatScaleFactor", land_area_threat_scale);
    space_threat_range_cap = gc.get_float("AI_SpaceThreatRangeCap", space_threat_range_cap);
    land_threat_range_cap = gc.get_float("AI_LandThreatRangeCap", land_threat_range_cap);
    ai_uses_fow_space = gc.get_bool("AIUsesFogOfWarSpace", ai_uses_fow_space);
    ai_uses_fow_land = gc.get_bool("AIUsesFogOfWarLand", ai_uses_fow_land);
    // A2 tactical
    health_low_percent_threshold = gc.get_float("Health_Low_Percent_Threshold", health_low_percent_threshold);
    max_ground_forces_on_planet = gc.get_int("Max_Ground_Forces_On_Planet", max_ground_forces_on_planet);
    allow_reinforcement_percentage = gc.get_float("Allow_Reinforcement_Percentage_Normalized", allow_reinforcement_percentage);
    use_reinforcement_points = gc.get_bool("Use_Reinforcement_Points", use_reinforcement_points);  // S10 AI land reinforcement
}

}  // namespace alamo::ai::tactical

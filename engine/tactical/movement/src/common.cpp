#include "alamo/data/game_database.hpp"
#include "alamo/tactical/movement/constants.hpp"

namespace alamo::tactical::movement {

namespace {
uint32_t get_u32(const data::GameConstants& gc, const char* tag, uint32_t fallback) {
    const float v = gc.get_float(tag, static_cast<float>(fallback));
    return v > 0.0f ? static_cast<uint32_t>(v) : 0u;
}
}  // namespace

MovementConstants MovementConstants::from_game_constants(const data::GameConstants& gc) {
    MovementConstants c;
    c.dynamic_land_complexity_quota = get_u32(gc, "DynamicLandComplexityQuota", c.dynamic_land_complexity_quota);
    c.dynamic_land_quota_reset_interval = get_u32(gc, "DynamicLandQuotaResetInterval", c.dynamic_land_quota_reset_interval);
    c.dynamic_avoidance_rectangle_bound = get_u32(gc, "DynamicAvoidanceRectangleBound", c.dynamic_avoidance_rectangle_bound);
    c.dynamic_obstacle_overlap_penalty = gc.get_float("DynamicObstacleOverlapPenalty", c.dynamic_obstacle_overlap_penalty);
    c.land_destination_proximity = gc.get_float("LandDestinationProximity", c.land_destination_proximity);
    c.land_temporary_destination_proximity =
        gc.get_float("LandTemporaryDestinationProximity", c.land_temporary_destination_proximity);
    c.max_move_frame_delay = static_cast<int32_t>(get_u32(gc, "Max_Move_Frame_Delay", 6));
    c.close_enough_angle_for_move_start = gc.get_float("CloseEnoughAngleForMoveStart", c.close_enough_angle_for_move_start);
    c.frames_per_collision_check = get_u32(gc, "FramesPerCollisionCheck", c.frames_per_collision_check);
    c.moving_vs_moving_look_ahead_time = gc.get_float("MovingVsMovingLookAheadTime", c.moving_vs_moving_look_ahead_time);
    c.minimum_stopped_vs_stopped_overlap =
        gc.get_float("MinimumStoppedVsStoppedOverlapCoefficient", c.minimum_stopped_vs_stopped_overlap);
    c.space_pathfind_max_expansions = get_u32(gc, "SpacePathfindMaxExpansions", c.space_pathfind_max_expansions);
    c.space_pathing_tries = get_u32(gc, "SpacePathingTries", c.space_pathing_tries);
    c.space_failure_max_expansions_coefficient =
        gc.get_float("SpacePathFailureMaxExpansionsCoefficient", c.space_failure_max_expansions_coefficient);
    c.space_failure_rotation_increment =
        gc.get_float("SpacePathFailureRotationExpansionIncrement", c.space_failure_rotation_increment);
    c.space_failure_forward_increment =
        gc.get_float("SpacePathFailureForwardExpansionIncrement", c.space_failure_forward_increment);
    c.space_failure_distance_cutoff_coefficient =
        gc.get_float("SpacePathFailureDistanceCutoffCoefficient", c.space_failure_distance_cutoff_coefficient);
    c.current_path_cost_coefficient_space =
        gc.get_float("CurrentPathCostCoefficientSpace", c.current_path_cost_coefficient_space);
    c.min_obstacle_cost_space = gc.get_float("MinObstacleCostSpace", c.min_obstacle_cost_space);
    c.min_obstacle_cost_land = gc.get_float("MinObstacleCostLand", c.min_obstacle_cost_land);
    c.max_rotations_space = get_u32(gc, "MaxRotationsSpace", c.max_rotations_space);
    c.xy_expansion_distance_space = gc.get_float("XYExpansionDistanceSpace", c.xy_expansion_distance_space);
    c.xy_expansion_distance_land = gc.get_float("XYExpansionDistanceLand", c.xy_expansion_distance_land);
    c.match_facing_delta_space = gc.get_float("MatchFacingDeltaSpace", c.match_facing_delta_space);
    c.occupation_radius_coefficient_space =
        gc.get_float("OccupationRadiusCoefficientSpace", c.occupation_radius_coefficient_space);
    c.destination_search_radius_increment_space =
        gc.get_float("DestinationSearchRadiusIncrementSpace", c.destination_search_radius_increment_space);
    c.off_map_cost_penalty = gc.get_float("OffMapCostPenalty", c.off_map_cost_penalty);
    c.wait_operator_cost_coefficient = gc.get_float("WaitOperatorCostCoefficient", c.wait_operator_cost_coefficient);
    c.wait_operator_base_frame_time = gc.get_float("WaitOperatorBaseFrameTime", c.wait_operator_base_frame_time);
    c.wait_operator_speed_coefficient = gc.get_float("WaitOperatorSpeedCoefficient", c.wait_operator_speed_coefficient);
    c.land_wait_operator_speed_coefficient =
        gc.get_float("LandWaitOperatorSpeedCoefficient", c.land_wait_operator_speed_coefficient);
    c.space_pathfind_frame_delay_delta = get_u32(gc, "SpacePathfindFrameDelayDelta", c.space_pathfind_frame_delay_delta);
    c.space_object_tracking_tree_count = get_u32(gc, "SpaceObjectTrackingTreeCount", c.space_object_tracking_tree_count);
    c.space_object_tracking_interval = get_u32(gc, "SpaceObjectTrackingInterval", c.space_object_tracking_interval);
    c.turn_in_place_slowdown_corvette = gc.get_float("TurnInPlaceSlowdownCorvette", c.turn_in_place_slowdown_corvette);
    c.turn_in_place_slowdown_frigate = gc.get_float("TurnInPlaceSlowdownFrigate", c.turn_in_place_slowdown_frigate);
    c.turn_in_place_slowdown_capital = gc.get_float("TurnInPlaceSlowdownCapital", c.turn_in_place_slowdown_capital);
    c.final_facing_180_penalty = gc.get_float("FinalFacing180Penalty", c.final_facing_180_penalty);  // OI tactical
    c.special_aligned_operator_bonus = gc.get_float("SpecialAlignedOperatorBonus", c.special_aligned_operator_bonus);
    c.threat_expansion_distance = gc.get_float("ThreatExpansionDistance", c.threat_expansion_distance);  // §4.2
    c.max_formation_area = gc.get_float("Max_Formation_Area", c.max_formation_area);
    c.spread_out_spacing_coefficient = gc.get_float("Spread_Out_Spacing_Coefficient", c.spread_out_spacing_coefficient);
    c.infantry_formation_recruitment_distance =
        gc.get_float("InfantryFormationRecruitmentDistance", c.infantry_formation_recruitment_distance);
    c.vehicle_formation_recruitment_distance =
        gc.get_float("VehicleFormationRecruitmentDistance", c.vehicle_formation_recruitment_distance);
    c.double_click_move_max_speed_ratio = gc.get_float("DoubleClickMoveMaxSpeedRatio", c.double_click_move_max_speed_ratio);
    c.fleeing_infantry_speed_bonus = gc.get_float("Fleeing_Infantry_Speed_Bonus", c.fleeing_infantry_speed_bonus);  // s78n t14
    c.land_speed_multiplier = gc.get_float("Object_Max_Speed_Multiplier_Land", c.land_speed_multiplier);
    c.space_speed_multiplier = gc.get_float("Object_Max_Speed_Multiplier_Space", c.space_speed_multiplier);
    c.space_guard_range = gc.get_float("Space_Guard_Range", c.space_guard_range);  // S7 t37
    c.land_guard_range = gc.get_float("Land_Guard_Range", c.land_guard_range);
    c.max_waypoints_per_path = get_u32(gc, "MaxWaypointsPerPath", c.max_waypoints_per_path);  // s78f t02
    return c;
}

}  // namespace alamo::tactical::movement

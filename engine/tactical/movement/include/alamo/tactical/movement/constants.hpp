// GameConstants used by movement (vanilla FoC values, docs/re/MOVEMENT.md §1.7, §2.7, §4.5).
// from_game_constants() overrides them from a loaded GameConstants.xml (mods).
#pragma once
#include <cstdint>

namespace alamo::data {
struct GameConstants;
}

namespace alamo::tactical::movement {

struct MovementConstants {
    // Land (§1.5-1.7)
    uint32_t dynamic_land_complexity_quota = 100;
    uint32_t dynamic_land_quota_reset_interval = 5;
    uint32_t dynamic_avoidance_rectangle_bound = 3;
    float dynamic_obstacle_overlap_penalty = 15.0f;
    float land_destination_proximity = 60.0f;
    float land_temporary_destination_proximity = 20.0f;
    int32_t max_move_frame_delay = 6;
    float close_enough_angle_for_move_start = 180.0f;
    uint32_t frames_per_collision_check = 4;
    float moving_vs_moving_look_ahead_time = 1.5f;  // seconds (x 30 frames)
    float minimum_stopped_vs_stopped_overlap = 0.15f;
    // Space (§2.7)
    uint32_t space_pathfind_max_expansions = 3500;
    uint32_t space_pathing_tries = 6;
    float space_failure_max_expansions_coefficient = 1.7f;
    float space_failure_rotation_increment = 0.5f;
    float space_failure_forward_increment = 0.5f;
    float space_failure_distance_cutoff_coefficient = 0.25f;
    float current_path_cost_coefficient_space = 0.66f;
    float min_obstacle_cost_space = 15.0f;
    float min_obstacle_cost_land = 3.0f;
    uint32_t max_rotations_space = 24;
    float xy_expansion_distance_space = 300.0f;
    float xy_expansion_distance_land = 60.0f;
    float match_facing_delta_space = 10.0f;
    float occupation_radius_coefficient_space = 1.2f;
    float destination_search_radius_increment_space = 50.0f;
    float off_map_cost_penalty = 20.0f;
    float wait_operator_cost_coefficient = 0.8f;
    float wait_operator_base_frame_time = 100.0f;
    float wait_operator_speed_coefficient = 0.2f;
    float land_wait_operator_speed_coefficient = 0.65f;
    uint32_t space_pathfind_frame_delay_delta = 2;
    uint32_t space_object_tracking_tree_count = 45;
    uint32_t space_object_tracking_interval = 90;
    float turn_in_place_slowdown_corvette = 2.0f;
    float turn_in_place_slowdown_frigate = 3.0f;
    float turn_in_place_slowdown_capital = 4.0f;
    float final_facing_180_penalty = 1.5f;        // OI tactical: facing-mode heuristic (§2.1)
    float special_aligned_operator_bonus = 0.6f;  // OI tactical: tangent ops 11/12/15/16 + facing heuristic
    float threat_expansion_distance = 600.0f;     // REMAINING_GAPS §4.2: D in threat mode (ThreatExpansionDistance)
    // Formations (§4.5)
    float max_formation_area = 14.0f;
    float spread_out_spacing_coefficient = 1.8f;
    float infantry_formation_recruitment_distance = 60.0f;
    float vehicle_formation_recruitment_distance = 100.0f;
    float double_click_move_max_speed_ratio = 1.25f;
    // s78n t20 (MOVEMENT §4.3.1 item 4): BetweenFormationSpacing (GC +0x14ec), the margin of the destination check's
    // formation and slot rectangles; the destination query extension (GC +0x14f0, Destination_Collision_Query_Extension,
    // 60; s78n t20 batch 2: read from GameConstants).
    float between_formation_spacing = 5.0f;
    float destination_query_extension = 60.0f;
    // s78n t14 part B (FIGHTER_FLYING §7.1, MOVEMENT §3.4): Fleeing_Infantry_Speed_Bonus (GC +0x150c, read as a float; FoC 2),
    // the factor on a fleeing squad's speeds and turn rates (its container's current path is a crush-flee push, kind 7).
    float fleeing_infantry_speed_bonus = 2.0f;
    // Mode speed multipliers (§3.4)
    float land_speed_multiplier = 1.56f;
    float space_speed_multiplier = 1.2f;
    // S7 t37 (A1_READINGS §33.4.1 step 2): the cap of a guard block's follow radius R (GameConstants +0x11EC0 / +0x11EC4).
    float space_guard_range = 750.0f;
    float land_guard_range = 150.0f;
    // s78f t02 (FIGHTER_FLYING §4.11 B 1): the waypoint append's limit of queued (0x08) blocks per formation (GC +0x13f0).
    uint32_t max_waypoints_per_path = 6;

    static MovementConstants from_game_constants(const data::GameConstants& gc);
};

}  // namespace alamo::tactical::movement

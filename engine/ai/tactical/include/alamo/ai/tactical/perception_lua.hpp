// A1 stage 1b - the Lua-facing functions that read the threat grid or combat power
// (docs/re/AI_TACTICAL_PERCEPTION.md 5, AI_TACTICAL.md 6.4). Pure C++; the Lua bindings are stage 2.
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "alamo/ai/tactical/perception_world.hpp"
#include "alamo/ai/tactical/threat_grid.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/auto_resolve.hpp"

namespace alamo::ai::tactical {

// ---- Get_Most_Defended_Position (0x69f3a0 -> 0x655400 space / 0x6556c0 land) [H] -------------------
// Cell score = CellForce(cell, all categories, player, ally = true, A = 0, no exclusion, no faction),
// no area-scale factor. Scan: land = the cells of the position's hint zone (list order); space = the
// cells overlapped by a square of side AI_SpaceEvaluatorRegionSize centred on the position (rows
// outer, columns inner, clamped). The maximum wins; an EXACT tie draws ONE Get_Float01 (synced) and
// switches to the new cell if the draw > 0.5. A1_READINGS §17 [H]: the running best starts at -1.0f, so
// the first cell is taken without a draw and every later tie draws (0 == 0 included); a grid whose category union
// misses the mask returns kNoDefendedPosition with no draws. Result: the chosen cell's centre (z = 0), or
// kNoDefendedPosition (no cells).
inline constexpr float kNoDefendedPosition = 1.0e18f;
struct DefendedPosition {
    Vec3f position{kNoDefendedPosition, kNoDefendedPosition, kNoDefendedPosition};
    int32_t cell = -1;
    uint32_t draws = 0;
    bool found() const { return cell >= 0; }
};
DefendedPosition most_defended_position_cells(const TacticalPerceptionGrid& g, const PerceptionWorld& w,
                                              std::span<const uint32_t> cells, int32_t player, SimRng& rng);
DefendedPosition most_defended_position_space(const TacticalPerceptionGrid& g, const PerceptionWorld& w, const Vec3f& pos,
                                              float region_size, int32_t player, SimRng& rng);

// ---- TaskForce.Get_Self_Threat_Max / Get_Self_Threat_Sum (0x5fcda0 / 0x5fcec0) [H] -----------------
// Over the task force's units (list order; alive, with a type): AI_Combat_Power. Empty -> 0.
struct SelfThreatUnit {
    float combat_power = 0.0f;
    bool alive = true;
    bool has_type = true;
};
float self_threat_max(std::span<const SelfThreatUnit> units);
float self_threat_sum(std::span<const SelfThreatUnit> units);

// ---- Project_By_Unit_Range (0x69f6f0) [H] ------------------------------------------------------------
// R = max(max attack range of the unit (a team: its leader), weapon range of every live hard point of
// type 5..10); d = (pos - unitPos) normalised with the retail fast inverse sqrt (0x5f375a86, two
// Newton steps: dmath::fast_rsqrt_retail of x*x + y*y + z*z); result = pos + d x R, x / y clamped to the
// map extents. nullopt when the positions are identical (the original warns, no result).
float unit_projection_range(const PerceptionWorld& w, const PerceptionObject& unit);
std::optional<Vec3f> project_by_unit_range(const Vec3f& unit_pos, float range, const Vec3f& pos, const Rect2& map);
std::optional<Vec3f> project_by_unit_range(const PerceptionWorld& w, const PerceptionObject& unit, const Vec3f& pos);
// t48: `unit` null = a live object the snapshot does not list (no weapon data): R = 0, from `unit_pos`.
std::optional<Vec3f> project_by_unit_range(const PerceptionWorld& w, const PerceptionObject* unit, const Vec3f& unit_pos, const Vec3f& pos);

// ---- unit.Is_Good_Against(x) (0x57bb60) [H, A2L_T03_RE_ANSWERS Q6.4] ----------------------------------
// For every category bit of x's type (low to high), the AI weight 0x5f1f90 (ContrastTable::ai_weight) of the
// caller's type in the row whose enemy key is exactly that bit (no row: 0); true iff the maximum is > 1.0.
bool is_good_against(const galactic::ContrastTable& contrast, std::string_view caller_type_name, uint64_t caller_categories,
                     uint64_t target_categories);

}  // namespace alamo::ai::tactical

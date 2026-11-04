// A2 tactical - the tactical PotentialPlan: the land / space marshaller lists (docs/re/AI_GALACTIC_2.md §1.1:
// land FreeStore, Plan, Reinforcement, TechTree, Hero, Contrast, TacticalBuild; space FreeStore, TechTree, Plan,
// Reinforcement, Hero, Contrast, TacticalBuild) on the generic solver core (ai_plans.hpp run_pp_solver), with the
// tactical marshaller rules of docs/re/AI_GALACTIC_3.md §1 and the tactical hero attach (§2) / population cap (§3).
//
// The solve runs where the galactic one does: in Find_Plan_For_Goal at proposal time (slot 10, land and space
// 0x6b8850), so its synced draws (Plan Setup C ints, IgnoreTarget contrast floats) happen during proposal, in cursor
// order. A failed solve rejects the candidate. The accept in Resolve_Category solves it again (0x6c4a50, A1_READINGS
// §28.6), drawing again. The solved selection is what Produce_Force gathers.
//
// The input is a plain snapshot (filled by TacticalAiWorld from its PerceptionWorld, free store, the host's
// reinforcement pool and build pads), so the solver is testable without a world. Candidates are object TYPES
// (PerceptionWorld type indices); one selection = one unit, taken from (in marshaller order) a free-store unit, a
// reinforcement-pool entry, a build pad (UC_ types), else "to be produced".
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ai/ai_plans.hpp"
#include "alamo/ai/tactical/perception_world.hpp"

namespace alamo {
class SimRng;
}
namespace alamo::galactic {
class ContrastTable;
}

namespace alamo::ai::tactical {

// One free-store unit (free store list order: ascending object id [port, free_store.hpp]).
struct PpFreeUnit {
    int32_t id = -1;
    int32_t type = -1;        // PerceptionWorld type index (own type)
    Vec3f position;
    float health = 1.0f;      // hull fraction
    bool reserved = false;    // Setup A skip: entry +0x30 set and not the PP's first solve (A1_READINGS §28.5; host fills)
    // FreeStore distance key over the TF stages (AI_GALACTIC_3 §1.2, A1_READINGS §25.3), computed by the host:
    // (Σ finite Low_Threat path cost² from the unit's zone + Σ 2-D distance² to each stage) / stages, when at least one
    // cost was finite; < 0 = none finite (the key is then the 2-D distance² to the goal target).
    double stage_key = -1.0;
};

// One reinforcement-pool slot (pool order = the source order: campaign = the parent's reinforcement record, skirmish
// = the player's tactical reinforcement table; AI_GALACTIC_3 §1.3).
struct PpPoolEntry {
    int32_t key = -1;
    int32_t type = -1;            // entry type E (a company / squadron for multi-unit entries)
    float population = 0.0f;      // pop(E, mode), charged once per entry
    uint32_t units = 1;
    int32_t member_type = -1;     // E's first Company_Units member M (-1: no Company_Units)
    bool member_create_team = false;  // M has Create_Team (+0x67)
    int32_t count = 1;            // count(E): selections one entry serves (§1.3)
    bool reserved = false;        // the free store reserved the slot (+0x88) / a task force holds it: entry left empty
};

// One build pad the player may build on (ascending id) and the UC_ types it accepts for the player's faction.
struct PpPad {
    int32_t id = -1;
    Vec3f position;
    std::vector<int32_t> accepts;  // type indices (Tactical_Buildable_Objects_* rows of the pad type)
    bool reserved = false;         // reserved by another PotentialPlan / task force
};

struct TacticalPpInput {
    TacticalMode mode = TacticalMode::Land;
    const PlanDefinition* def = nullptr;
    uint32_t plan = kAiNone;
    uint32_t goal_type = kAiNone;
    int32_t player = -1;
    std::string faction;
    float time_limit = -1.0f;        // goal Time_Limit (< 0 none)
    // Target (AITarget of the goal): has_target false for global goals.
    bool has_target = false;
    Vec3f target_position;
    int32_t target_object = -1;      // unit targets: the object id
    int32_t target_zone = -1;        // land regions: the hint zone
    // Types (PerceptionWorld order) + category names.
    const std::vector<PerceptionType>* types = nullptr;
    std::function<uint64_t(std::string_view)> category_mask;
    // q6 t03: the space-layer category bits (tactical_space_categories); 0 = the stock names through category_mask.
    uint64_t space_categories = 0;
    // Sources.
    std::vector<PpFreeUnit> free_units;
    std::vector<PpPoolEntry> pool;
    std::vector<PpPad> pads;
    float credits = 0.0f;            // player credits (+0x70; TacticalBuild affordability)
    int32_t player_tech = 0;
    float pop_cap = -1.0f;           // < 0: no cap (AI_GALACTIC_3 §3.3)
    float pop_used = 0.0f;           // §3.2 (fractional shares rounded up by the host)
    bool campaign = false;           // the mode has a galactic parent (Hero Setup C item 3)
    bool reinforcements_enabled = true;  // player +0x39C (story DISABLE/ENABLE_REINFORCEMENTS)
    float health_threshold = 0.33f;  // Health_Low_Percent_Threshold (free-store eligibility)
    // TacticalBuild, types without behaviour 0x2A: 0x288b80 + 0x497140 (a builder can build T now). Absent = none.
    std::function<bool(int32_t type)> can_build_now;
    // Contrast: enemy strength at the target for a category mask (all bits = the layer total).
    const galactic::ContrastTable* contrast = nullptr;
    float contrast_multiplier = 1.0f;  // Land_ / Space_AI_Contrast_Multiplier (difficulty)
    int32_t recent_failures = 0;       // Learning 0x588980 at this target
    std::function<double(uint64_t mask)> enemy_strength;
    SimRng* rng = nullptr;
    uint32_t max_iterations = 200;     // port guard
};

enum class PpSource : uint8_t { FreeStore = 0, Reinforcement = 1, Build = 2, Produce = 3 };
const char* pp_source_name(PpSource s);

struct TacticalPpPick {
    int32_t type = -1;
    PpSource source = PpSource::Produce;
    int32_t ref = -1;   // unit id / pool key / pad id
    int32_t tf = 0;     // task force index of the plan
    int32_t team = -1;  // team index within the task force (-1 = unplaced)
};

struct TacticalPpResult {
    bool ok = false;
    std::vector<TacticalPpPick> picks;
    size_t candidates = 0;
    std::string last_veto;  // marshaller whose Finalize failed the solve (diagnostics), "-" none
};

// Runs the mode's marshaller list. Consumes synced draws (Plan Setup C, IgnoreTarget contrast) from input.rng.
bool solve_tactical_pp(const TacticalPpInput& input, TacticalPpResult& out);

// Team spec lhs (0x66fcc0, AI_GALACTIC_2 §1.3) over PerceptionWorld types: a category mask when every name is a
// GameObjectCategoryType name (types in type order with mask overlap and the TF unit filter, flags +0x67/+0x29/+0x21
// clear), else the '|' list of type names; -Name excluded.
std::vector<int32_t> pp_team_types(const TaskForceDef& tf, const TeamDef& team, const std::vector<PerceptionType>& types,
                                   const std::function<uint64_t(std::string_view)>& category_mask);

}  // namespace alamo::ai::tactical

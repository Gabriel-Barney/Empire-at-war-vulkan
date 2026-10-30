// AI framework: plan definitions, the plan pick and the PotentialPlan greedy solver
// (docs/re/AI_GALACTIC.md §1.9.1-1.9.3). Mode-agnostic; the Lua side (reading Definitions() globals,
// pooled scripts, running plans) is in ai_lua_runtime.hpp, the galactic marshallers in galactic_ai.hpp.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ai/ai_types.hpp"

namespace alamo {
class SimRng;
}

namespace alamo::ai {

class AiDefinitions;

// One team of a TaskForce entry (0x66fcc0, AI_GALACTIC_2 §1.3) [H]: "Frigate | Capital = 0,4",
// "Vehicle | Infantry = 100%", "Rebel_Star_Base_1 = 1". names = the '|' list as written (a category mask when
// every name is a GameObjectCategoryType name, else type names; resolved by the mode).
//   a,b -> min a, max (b != 0 ? b : a), percent 0;   N -> min N, max N;   N% -> min 0, max 0, percent N
// (renormalised over the whole plan by renormalise_percents).
struct TeamDef {
    std::vector<std::string> names;
    bool percent = false;      // "N%": percent-based team (sets the TF's percent flag +0x48)
    float percent_value = 0.0f;
    int32_t min = 0, max = 0;  // TeamDef +0x14 / +0x10
};

struct TaskForceDef {
    std::string name;  // the Lua global, <Name>_Thread and the event-handler prefix
    std::vector<TeamDef> teams;
    bool deny_hero_attach = false, escort = false, required = false, deny_special_weapon_attach = false;
    bool require_unique_stage = false, require_matching_stage = false;
    int32_t minimum_total_size = 0;
    float minimum_total_force = 0.0f;
    std::vector<std::string> excluded_types;  // "-TypeName"
    std::vector<std::string> masks;           // bare names that are not keywords (unit filters) [M]
    bool percent_based() const;
};

// Parses one TaskForce table entry (first string = name). Unknown syntax is reported in *diag and skipped.
bool parse_task_force(const std::vector<std::string>& strings, TaskForceDef& out, std::vector<std::string>* diag = nullptr);
struct PlanDefinition;
// 0x670630 [H]: after every TF of a plan is parsed, each percent team becomes
// (int)(pct / Σpct x 100 + 0.5), Σ over all teams of all TFs of the plan.
void renormalise_percents(PlanDefinition& plan);

struct PlanDefinition {
    std::string name;      // file base name
    std::string file;      // game-relative path
    uint32_t folder = 0;   // load folder index (Data\Scripts\AI = 0, ...)
    std::vector<uint32_t> goal_types;  // Category list resolved (unknown names dropped)
    std::vector<TaskForceDef> task_forces;
    bool ignore_target = false, magic_plan = false, magic_plan_stealing = true;
    bool allow_free_store_units = true, allow_engaged_units = true;
    float additional_cost = 0.0f, minimum_cost = 0.0f;
    float per_failure_contrast_adjust = 0.1f;  // PGAICommands Base_Definitions default
    float min_contrast_scale = 1.1f, max_contrast_scale = 1.5f;
    std::vector<std::string> required_categories;
    int32_t pool_count = 1;  // ScriptPoolCount: missing = 1, 0 = not poolable
    bool serves(uint32_t goal_type) const;
};

// Every plan of a game session, in load order (folder order, then name order within a folder [port: the
// original uses filesystem enumeration order, AI_GALACTIC.md §1.9.1 / open item 11]).
struct PlanLibrary {
    std::vector<PlanDefinition> plans;
    uint32_t find(std::string_view name) const;  // case-insensitive, kAiNone
};

// The plan-script folders in load order (0x4e7e30) [H].
inline constexpr const char* kPlanFolders[] = {"Data\\Scripts\\AI\\", "Data\\Scripts\\AI\\SpaceMode\\",
                                               "Data\\Scripts\\AI\\LandMode\\", "Data\\Scripts\\AI\\StoryArc\\",
                                               "Data\\Scripts\\Interventions\\"};
// [port] Within a folder files are loaded in byte order of their upper-cased names (Vfs::list order).
void sort_plan_files(std::vector<std::string>& paths);

// Find_Plan_For_Goal's pick (0x6bae50, §1.9.2): weights = success ratio + 1.0 accumulated in float in
// candidate order; ONE synced draw r = range_float(0, total) [M: hi = total, open item 2], then the first
// cumulative >= r (binary search). Returns the candidate index; 0 draws and kAiNone when empty.
uint32_t pick_weighted(std::span<const double> success_ratio, SimRng& rng);
// Resolve's random phase over effective desires (same draw shape): cumulative float weights, one draw.
uint32_t pick_cumulative(std::span<const float> weights, SimRng& rng);

// ---- PotentialPlan solver (0x6c3da0, AI_GALACTIC_2 §1.1-1.2) [H] ----
// A marshaller (vtable slots +0x08..+0x38). Every call runs over the mode's list in order (pp_marshaller_order).
class PpMarshaller {
public:
    virtual ~PpMarshaller() = default;
    virtual void setup_a() {}  // +0x28
    virtual void setup_b() {}  // +0x18
    virtual void setup_c() {}  // +0x20
    virtual float score(uint32_t candidate) = 0;  // +0x08; exactly kAiBigFloat = veto
    virtual void commit(uint32_t candidate) { (void)candidate; }  // +0x10
    virtual void finalize(bool& success) { (void)success; }      // +0x30 (may clear success)
    virtual bool permanent_veto() const { return false; }         // +0x38 (Plan, Production: true)
};

enum class PpMarshallerKind : uint8_t {
    FreeStore, Budget, TechTree, Plan, Production, Hero, TerrainType, Contrast, Survival, Reinforcement, TacticalBuild
};
// The lists built by 0x6c0e60 (§1.1 table): galactic FreeStore, Budget, TechTree, Plan, Production, Hero,
// TerrainType, Contrast, Survival; land FreeStore, Plan, Reinforcement, TechTree, Hero, Contrast, TacticalBuild;
// space FreeStore, TechTree, Plan, Reinforcement, Hero, Contrast, TacticalBuild.
std::span<const PpMarshallerKind> pp_marshaller_order(AiMode mode);
const char* pp_marshaller_name(PpMarshallerKind k);

// The solver's view of the candidate list (candidates are object TYPES; one selection = one unit).
struct PpSolverIo {
    std::function<size_t()> candidate_count;             // read after the setups (Setup B may add candidates)
    std::function<uint32_t(uint32_t candidate)> type_of;  // for the 50 % rule
    std::function<void(uint32_t candidate)> on_select;    // step 4 bookkeeping before the marshaller commits
    std::function<void()> after_commit;                   // step 5 (galactic ETA recompute)
    std::function<void(uint32_t candidate, size_t marshaller)> on_veto;  // diagnostics only (optional)
};
// Setup A for each marshaller, then B, then C; then iterate (§1.2):
//   1. per candidate the scores summed in marshaller order (float); exactly 1e18 = veto (-1e18) and the rest
//      is skipped; a veto by a permanent-veto marshaller erases the candidate;
//   2. 50 % rule: a viable candidate that is not the only viable one is vetoed when the selection is
//      non-empty and (same-type entries) / |selection| >= 0.5;
//   3. pick the first index with the strictly greatest score above -1e18 (nothing -> stop);
//   4. on_select, then commit on every marshaller in order; 5. after_commit.
// Finally Finalize on every marshaller. Returns success (starts true; any Finalize may clear it).
// `selection` = committed candidate indices in order. `max_iterations` is a port guard only.
bool run_pp_solver(std::span<PpMarshaller* const> marshallers, const PpSolverIo& io, std::vector<uint32_t>& selection,
                   uint32_t max_iterations = 1000);

}  // namespace alamo::ai

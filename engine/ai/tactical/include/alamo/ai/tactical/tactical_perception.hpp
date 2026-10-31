// A1 stage 1b - tactical perceptual evaluators and token leaves (docs/re/AI_TACTICAL_PERCEPTION.md 1-3).
//
// The equation language, linker, '#' operator, Function_/Script_ references and token ids are S2's
// (alamo/ai/perception_equations.hpp, used unchanged). This file is the tactical TokenResolver: the
// evaluator tree the language resolves paths through, the leaf bodies, the leaf cache and the
// normalised tokens.
//
// Evaluator tree (section 2) [H]:
//   Game                          -> Game evaluator (Land/SpaceGamePerceptualEvaluatorClass)
//   Variable_Self/Enemy/Human     -> the player evaluator of that player INDEX (player list order)
//   Variable_Target / _Location   -> a target HANDLE (register_*): a unit (LandUnit / SpaceShip
//                                    evaluator, every target object, structures included), a land hint
//                                    zone (LandLocation) or a space cell (SpaceLocation)
//   <unit>.Location               -> a location bound to the unit: land = the unit's CURRENT hint zone
//                                    (PerceptionObject::zone; outside every zone the force /
//                                    concentration / time tokens return 0), space = a square of side
//                                    2 x max attack range centred on the unit
//   <unit>.Owner                  -> the player evaluator of the owner AT REGISTRATION [M: not re-pointed]
//   <unit>.Type.IsType            -> 1 iff the object's type name is one of Parameter_Type [L]
//   <land location>.Hints.<name>  -> PerceptionWorld::zone_hint (absent: 0) [L: value rule open item 2]
//
// Duplicate zone names (TACTICAL_GAPS 6.8, A1 t03) [H; M for "evaluation looks the evaluator up by the bound id"]:
// every hint zone gets its own region target (own handle, centre, reachability, list position), but the
// original registers location evaluators under the zone's NAME id and refuses a second registration, so a
// zone whose name repeats an earlier zone's evaluates through the evaluator of the first REGISTERED zone with
// that name (its force sums, cells, centre, hints and leaf-cache entries). Port: evaluator_handle() maps a
// zone handle to that handle; the resolver binds Variable_Target / Variable_Location through it. Name match =
// ASCII case-insensitive [M: the name table's comparison is not traced; shipped repeats have identical case].
// Zone links stay by 02 id; nothing resolves zones by name (the original's post-load relink to the first zone
// by name, 0x6b9580, is not reproduced: N1 saves restore region targets by zone index).
// A token that is not registered on the evaluator it is applied to fails the evaluation.
//
// Leaf cache (1.2): every leaf goes through LeafCache with its lifetime from section 3 (seconds; -1
// forever; 0 none), keyed by (evaluator, leaf, Self/Target/Enemy/Human/Location, parameters).
//
// Normalised tokens (1.4) [H]: numerator = the *Unnormalized leaf on the same evaluator with the
// caller's parameters (the path it names reaches that evaluator), failure -> failure, exactly 0 -> 0;
// denominator = the named leaf (Game / Self / owner) WITHOUT parameters; num <= den ? num / den : 1.
//
// Parameters (1.3): Parameter_Category (GameObjectCategoryType names, default all bits),
// Parameter_Attenuator (0), Parameter_Type (list, default none), Parameter_Faction (case-insensitive
// [M]), Parameter_Time, Parameter_Range, Parameter_Count (1), Parameter_Only_Consider_Complete (0),
// Parameter_Difficulty_Level_Type, Parameter_Hard_Point_Type.
//
// Thread safety: evaluation is const and may run concurrently for different contexts while the world
// and the grid are not mutated (the cache locks internally). '#' needs EvalContext::rng (serial).
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/ai/perception_equations.hpp"
#include "alamo/ai/tactical/perception_cache.hpp"
#include "alamo/ai/tactical/perception_world.hpp"
#include "alamo/ai/tactical/target_list.hpp"
#include "alamo/ai/tactical/threat_grid.hpp"
#include "alamo/ai/tactical/zone_reachability.hpp"

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::ai::tactical {

struct PerceptionTarget {
    enum class Kind : uint8_t { Unit = 0, LandZone = 1, SpaceCell = 2 };
    Kind kind = Kind::Unit;
    int32_t object_id = -1;               // Unit
    int32_t owner_at_registration = -1;   // Unit: .Owner (frozen)
    bool released = false;                // Unit: destroyed (evaluations fail)
    int32_t zone = -1;                    // LandZone
    int32_t cell_x = -1, cell_y = -1;     // SpaceCell
    Rect2 rect;                           // SpaceCell: the cell clipped to the map
    Vec3f center;                         // LandZone: zone centre; SpaceCell: rect centre
};

struct TacticalPerceptionConfig {
    // GameObjectCategoryType name -> mask (0 unknown). Default: the objects module's built-in table.
    std::function<uint64_t(std::string_view)> category_mask;
    // [M] cell-force memory quirk (threat_grid.hpp); true = documented behaviour.
    bool memory_counted_when_not_fogged = true;
};

class TacticalPerception;

class TacticalTokenResolver final : public TokenResolver {
public:
    TacticalTokenResolver(const TacticalPerception& p, const PerceptionWorld& w) : p_(p), w_(w) {}
    bool resolve(const EvalContext& ctx, const TokenRef& token, double& out) const override;
    bool implements(Root root, std::span<const TokenId> path, const TokenTable& tokens) const override;
    // Script_<File>.Evaluate (AI_GALACTIC_2 §4.4, unchanged in tactical): TacticalPerception's script evaluator
    // (fails without one). Serial callers only.
    bool resolve_script(const EvalContext& ctx, std::string_view name, std::span<const Param> params,
                        double& out) const override;

private:
    const TacticalPerception& p_;
    const PerceptionWorld& w_;
};

// Script_<File>.Evaluate hook (the host's Lua evaluator VMs, alamo/ai/ai_lua_runtime.hpp ScriptEvaluators).
using TacticalScriptEvaluator = std::function<bool(const EvalContext&, std::string_view name, std::span<const Param>, double&)>;

class TacticalPerception {
public:
    TacticalPerception();

    // Script_ references (AI_GALACTIC_2 §4.4): null = every Script_ reference fails (S2 default). The hook may run Lua
    // and draw synced RNG, so evaluations that reach it must be serial (the tactical AI evaluates on the sim thread).
    void set_script_evaluator(TacticalScriptEvaluator fn) { script_ = std::move(fn); }

    // Battle start: builds the threat grid over `map` (players = player list size) and the zones'
    // threat-cell lists. Clears targets and the cache.
    void init(TacticalMode mode, const TacticalAiConstants& c, const Rect2& map, uint32_t players,
              std::span<const HintZone> zones = {}, const TacticalPerceptionConfig& cfg = {});
    TacticalMode mode() const { return mode_; }
    TacticalPerceptionGrid& grid() { return grid_; }
    const TacticalPerceptionGrid& grid() const { return grid_; }
    const TacticalAiConstants& constants() const { return c_; }
    const TacticalPerceptionConfig& config() const { return cfg_; }
    LeafCache& cache() const { return cache_; }
    std::span<const uint32_t> zone_threat_cells(int32_t zone) const;
    uint32_t zone_count() const { return static_cast<uint32_t>(zones_.size()); }
    const HintZone& zone(uint32_t i) const { return zones_[i]; }

    // ---- evaluator tree (handles are stable for the battle) ----
    int32_t register_unit(const PerceptionWorld& w, int32_t object_id);  // existing handle if registered
    int32_t register_zone(int32_t zone);
    int32_t register_cell(int32_t x, int32_t y, const Rect2& rect);
    // Every target of the list, in list order; handles[i] for targets()[i].
    void register_targets(const PerceptionWorld& w, const TacticalTargetList& list, std::vector<int32_t>* handles = nullptr);
    int32_t handle_for(const TacticalTarget& t) const;  // -1 if not registered
    // The handle whose evaluator answers for `handle`: for a land zone, the first registered zone with the same name
    // (TACTICAL_GAPS 6.8); every other handle (units, cells, unique zone names, unknown handles) maps to itself.
    int32_t evaluator_handle(int32_t handle) const;
    // First zone index (list order) whose name equals zone `zone`'s (ASCII case-insensitive); `zone` itself if unique,
    // -1 for an index outside the zone list.
    int32_t first_zone_with_name(int32_t zone) const;
    int32_t handle_of_object(int32_t object_id) const;
    void on_object_destroyed(int32_t object_id);         // releases the unit evaluator (handle kept)
    size_t target_count() const { return targets_.size(); }
    const PerceptionTarget& target(int32_t handle) const { return targets_[static_cast<size_t>(handle)]; }
    bool is_target(int32_t handle) const { return handle >= 0 && static_cast<size_t>(handle) < targets_.size(); }

    // ---- evaluation ----
    TacticalTokenResolver resolver(const PerceptionWorld& w) const { return TacticalTokenResolver(*this, w); }
    EvalResult evaluate(const EquationSet& set, uint32_t equation, const EvalContext& ctx, const PerceptionWorld& w) const;
    EvalResult evaluate(const EquationSet& set, const Expression& expr, const EvalContext& ctx, const PerceptionWorld& w) const;

    // Zone threat for AI player `player` (section 4.6 zone danger) as zone_reachability's ZoneThreatFn.
    // Captures `w` and this by reference.
    ZoneThreatFn zone_threat_fn(const PerceptionWorld& w, int32_t player) const;

    // CRC: grid, targets, cache.
    void hash(StateHasher& h) const;

    // N1 save / load (contract: alamo/core/save_file.hpp; part of TacticalAiWorld's chunk). Layout: u32 version (1), u8
    // mode and u32 zone count (checked against init()), the threat grid (TacticalPerceptionGrid::write), every target
    // (all fields), the leaf cache. Not saved: init()'s constants, config, zones and zone cells, the script evaluator
    // hook; the unit / zone / cell handle tables are rebuilt from the targets (one target per registration, so they
    // come back exactly). read() into a perception init()ed like the writer's; false and unchanged on bad data;
    // reads its own fields only.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    friend class TacticalTokenResolver;
    TacticalMode mode_ = TacticalMode::Land;
    TacticalAiConstants c_;
    TacticalPerceptionConfig cfg_;
    TacticalPerceptionGrid grid_;
    std::vector<HintZone> zones_;
    std::vector<std::vector<uint32_t>> zone_cells_;
    std::vector<PerceptionTarget> targets_;
    std::vector<std::pair<int32_t, int32_t>> unit_handles_;  // sorted (object id, handle); lookup only
    std::vector<std::pair<int32_t, int32_t>> zone_handles_;  // (zone, handle)
    std::vector<int32_t> zone_name_first_;                    // zone -> first zone with an equal name (init)
    std::vector<int32_t> zone_name_handle_;                   // first-of-name zone -> first registered handle, -1
    void rebuild_zone_name_handles();                         // zone_name_handle_ from targets_ in handle order
    std::vector<std::pair<int64_t, int32_t>> cell_handles_;  // ((x << 32) | y, handle)
    mutable LeafCache cache_;
    TacticalScriptEvaluator script_;
};

}  // namespace alamo::ai::tactical

// A1 stage 2 hookup (owner: A1 agent): the tactical AI (engine/ai/tactical tactical_ai.hpp) in an S10 tactical
// battle. BattleAi is the TacticalAiHost of one TacticalBattle: it fills the PerceptionWorld snapshot from the
// mode's primary store, the battle's units / participants, the type database, the galaxy's players and the
// battle's CombatSystem (hard points = the S9 callback), and executes the AI's orders on the battle's own
// movement / combat / abilities paths:
//   MoveToPositionFacing        -> MovementSystem::move (the unit list, clamped to the mode bounds)
//   Attack                      -> CombatSystem::order_attack (ref2 = 1 + HardPointType -> the target's first
//                                  targetable hard point of that type) + a move into 80 % of the attack range
//   PortExtension LuaOrder      -> MoveTo / AttackMove / Guard: a move to the target object / point; Stop
//   Reinforce                   -> A2 tactical: TacticalBattle::order_reinforce (the human path: undeployed reserve,
//                                  side not retreating, space unit cap, rows of 6 around the clamped point [port])
//   Retreat                     -> TacticalBattle::request_retreat
//   40 / 43 / 57 / 58           -> TacticalBattle::handle_command (abilities)
//   TacticalBuild               -> A2 tactical: the player build event 0x42edf0 refusals (UC / built present, pad
//                                  fogged, not allied-owned, cooldown, type without TACTICAL_UNDER_CONSTRUCTION),
//                                  then TacticalBattle::pad_build without charge (AI players build at once, §3.6)
// Only units owned by the issuing player are ordered (legal orders: the human rules of the same handlers).
// [port] The battle's stand-in orders are switched off by the host when the AI is on; BattleAi keeps the
// stand-in (attack-move toward the nearest enemy every 60 frames) for sides without a tactical AI, except the
// human players' units under BattleConfig::skip_human_units (they take commands only; TacticalBattle::order_*).
//
// Why not SimHost::execute_direct: SimHost::apply handles MoveToPosition / MoveToObject / Attack with the G0
// stub locomotor of the TacticalMode, not the battle's MovementSystem / CombatSystem (integrator ask in
// docs/PHASE4.md); the AI uses its own CommandPipeline::execute_direct whose handler is BattleAi::execute.
#pragma once
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/ai/tactical/tactical_ai.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/lua/fog_bridge.hpp"        // refixf t03: Find_Nearest's fog test in the tactical AI VMs
#include "alamo/lua/script_gaps.hpp"       // battle host: the tactical AI VMs' TacticalScriptHooks
#include "alamo/lua/script_remaining.hpp"  // L1 bindings: the tactical AI VMs' RemainingScriptHooks
#include "alamo/tactical/movement/common.hpp"  // kThreatOff

namespace alamo::galactic {
class GalacticSim;
}

namespace alamo::sim {

class SimHost;
class TacticalBattle;
class TacticalMode;

// What the tactical AI needs from the session (loaded once by the host).
struct TacticalAiSetup {
    const ai::AiDefinitions* defs = nullptr;
    const ai::EquationSet* equations = nullptr;
    const FileProvider* files = nullptr;
    std::vector<ai::tactical::TacticalAiDifficulty> difficulties;
    std::vector<std::string> land_plans, space_plans;  // tactical_plan_files per mode
    // AIPlayerType of a battle player ("" = no tactical AI). Default: default_tactical_ai_type.
    std::function<std::string(int32_t player, const galactic::GalacticSim& galaxy)> ai_type;
    int32_t difficulty = 1;  // 0 Easy, 1 Normal, 2 Hard
    ai::tactical::TacticalAiConstants constants;
    bool log_decisions = true;
};

// [port] faction -> AIPlayerType for non-human playable players: Empire -> BasicEmpire, Rebel -> BasicRebel,
// Underworld -> AI_Player_Underworld (A2's mapping); "" otherwise.
std::string default_tactical_ai_type(int32_t player, const galactic::GalacticSim& galaxy);

class BattleAi final : public ai::tactical::TacticalAiHost {
public:
    BattleAi(TacticalBattle& battle, TacticalMode& mode, SimHost& host, const TacticalAiSetup& setup);
    ~BattleAi() override;
    BattleAi(const BattleAi&) = delete;
    BattleAi& operator=(const BattleAi&) = delete;

    // Loads the mode's plans and creates the AI players. False when no player got a tactical AI.
    bool start(std::vector<std::string>* diagnostics = nullptr);
    // One AI frame, after the battle tick (T7 order, see tactical_ai.hpp).
    void update();
    void end();  // battle end: every plan released
    ai::tactical::TacticalAiWorld& world() { return *world_; }
    const ai::tactical::TacticalAiWorld& world() const { return *world_; }
    bool controls(int32_t player) const;
    std::string summary() const;
    void hash(StateHasher& h) const;
    // N1 stage 2 (owner: n2-host; CampaignBattles chunk "battle.ai"): u32 version, a section with the TacticalAiWorld
    // (TacticalAiWorld::write / read, then relink() - battle.ai is the last battle chunk read), the AI sides
    // (checked: the loader's start() rebuilt the same AI players), stale order count, executed-event counters, the
    // opportunity-target snapshot. False (unchanged) on bad data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

    // ---- TacticalAiHost ----
    void fill(ai::tactical::PerceptionWorld& w, std::vector<ai::tactical::TacticalAiObject>& objects) override;
    commands::RejectReason execute(uint8_t player, const commands::EventData& ev) override;
    std::vector<ai::tactical::TacticalReinforcement> reinforcement_pool(int32_t player) override;
    bool can_reinforce(int32_t player) override;
    float population_cap(int32_t player) override;
    float population_used(int32_t player) override;
    const ai::tactical::DamageTrackingStore* damage_tracking() override;
    const galactic::ContrastTable* contrast() override;
    // --- A2 tactical: build pads (L1_S4_REMAINING §3.6 on TacticalBattle's pads) ---
    std::vector<ai::tactical::TacticalPad> build_pads(int32_t player) override;
    ai::tactical::PadCheck build_pad_check(int32_t player, int32_t pad, int32_t type) override;
    bool can_build_now(int32_t player, int32_t type) override;
    std::vector<int32_t> tactical_builders(int32_t player, int32_t type) override;  // skirmish station production
    // --- end A2 tactical ---
    // --- battle host follow-up ---
    // Hard points whose opportunity target changed to a new live target since the previous call (ascending unit id, then
    // hard-point index [port: no in-tick order]); owner = the unit's team container when it has one.
    std::vector<ai::tactical::OpportunityTargetEvent> opportunity_targets() override;
    // HIJACK_UNIT (AI_GALACTIC_3 §4.3): GalacticSim::magic_spawn at the battle planet, a surface unit launched into the
    // orbit fleet (FAILED when that fails, e.g. blockaded), TacticalBattle::add_reserve; returns the new pool entry.
    // Refused before spawning when the type is not in the battle tables. [port] the galactic AI free store is not touched.
    ai::tactical::TacticalReinforcement hijack_unit(int32_t player, int32_t type, bool steal) override;
    // Active_By_Default re-apply (TACTICAL_GAPS §1.2): AbilitySystem::apply_active_by_default for the player's live
    // battle object. [port] the hook does not tell the free-store path from the task-force path: a unit with HUNT
    // active is skipped on both; the un-immobilise step (0x3a8e50(obj, 0)) has no AbilitySystem entry point (not done).
    void unit_released(int32_t player, int32_t object_id) override;
    // --- end battle host ---

private:
    float type_population(int32_t type) const;  // A2 tactical: pop(T, mode) 0x373500
    float allow_reinforcement_fraction_ = 0.0f;  // A2 tactical: Allow_Reinforcement_Percentage_Normalized
    void standin_orders();
    commands::RejectReason execute_order(uint8_t player, const commands::EventData& ev);
    bool move_unit(objects::ObjectId id, float x, float y, float threat_threshold = ::alamo::tactical::movement::kThreatOff);

    TacticalBattle* b_;
    TacticalMode* mode_;
    SimHost* host_;
    std::vector<ai::tactical::HintZone> zones_;
    ai::tactical::ZoneLocator zone_locator_;  // position -> zone (A1_READINGS §4)
    std::unique_ptr<ai::tactical::TacticalAiWorld> world_;
    std::vector<uint8_t> ai_side_;  // per player: has a tactical AI
    uint32_t stale_ = 0;  // object orders whose target died before execution (no-ops)
    // refixf t02: this battle frame's per-unit Lua move orders (PortExtension MoveTo / AttackMove / Guard) with one key (player,
    // order, target, position bits, threat bits) -> one group move (execute_order). Transient: only the current frame's entries
    // are read (a frame change clears them), so it is not saved.
    struct OrderGroup {
        uint32_t frame = 0;
        uint8_t player = 0;
        uint32_t subtype = 0, target = 0, x = 0, y = 0, threat = 0;
        std::vector<objects::ObjectId> units;
    };
    std::vector<OrderGroup> order_groups_;
    std::map<std::string, std::pair<uint32_t, uint32_t>> executed_;  // event name -> (applied, rejected) [diagnostic]
    std::vector<std::pair<uint64_t, int32_t>> opp_prev_;  // battle host: (unit id << 8 | hp, target), ascending key
    // UnitAI data-pack re-projection stamps (A1_READINGS §14) of the listed UNIT_AI objects, drawn once per object
    // (draw_unit_ai_stamps, two synced draws) when fill() first lists it; ascending id.
    std::map<objects::ObjectId, int32_t> unit_ai_stamps_;
    std::vector<objects::ObjectId> team_units(uint8_t player, uint32_t wire) const;  // battle host: unit or TEAM members
    lua::TacticalScriptHooks tactical_;    // battle host: script_gaps.hpp hooks of the tactical AI VMs (L1 final list)
#ifdef ALAMO_LUA_HAVE_FOG
    // refixf t03: only object_hidden is set (Find_Nearest's FOW_Object_Hidden test); fog stays null, so the AI VMs'
    // FogOfWar.* bindings keep answering "Command only valid in a tactical game" as before.
    lua::FogScriptHooks fog_hooks_;
#endif
    lua::RemainingScriptHooks remaining_;  // L1 bindings: script_remaining.hpp hooks of the tactical AI VMs (Divert; Int-final: strikes, damage stats, fields, weather, shields)
};

}  // namespace alamo::sim
#endif

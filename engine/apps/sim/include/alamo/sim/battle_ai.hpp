// A1 stage 2 hookup (owner: A1 agent): the tactical AI (engine/ai/tactical tactical_ai.hpp) in an S10 tactical
// battle. BattleAi is the TacticalAiHost of one TacticalBattle: it fills the PerceptionWorld snapshot from the
// mode's primary store, the battle's units / participants, the type database, the galaxy's players and the
// battle's CombatSystem (hard points = the S9 callback), and executes the AI's orders on the battle's own
// movement / combat / abilities paths:
//   MoveToPositionFacing        -> MovementSystem::move (the unit list, clamped to the mode bounds)
//   Attack                      -> CombatSystem::order_attack (ref2 = 1 + HardPointType -> the target's first
//                                  targetable hard point of that type) + a move into 80 % of the attack range;
//                                  the target is also the units' attack goal (refixf t06)
//   PortExtension LuaOrder      -> MoveTo / AttackMove / Guard: a move to the target object / point; Stop.
//                                  refixf t06 (TACTICAL_COMBAT §2.8): AttackMove on an enemy combat object sets it as
//                                  each unit's attack goal (CombatSystem::set_attack_goal, path B); the task-force order
//                                  (arg bit kTfAttackMoveIssueAttack) first issues Order_Attack per unit
//                                  (order_attack_issue, path A) and drops refusing units; land: min-range staging [M].
//                                  Every other move / stop clears the attack goal
//   Reinforce                   -> A2 tactical: TacticalBattle::order_reinforce (the human path: undeployed reserve,
//                                  side not retreating, space unit cap; land: the company drop layout, space: rows of 6 [port])
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
#include <string_view>
#include <vector>

#include "alamo/ai/tactical/tactical_ai.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/lua/object_bridge.hpp"     // sp3 slice 4b: LaneAbilityView, LaneWrite
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
// sp3: the process-wide default of TacticalAiSetup::ai_lane / k_ai (alamo_headless --ai-lane [--ai-lane-k K]). Set once
// at start-up, before any setup is built; never read by sim code after that (each world keeps its own copy, saved).
struct AiLaneDefaults {
    bool on = false;
    uint32_t k_ai = ai::tactical::kDefaultAiLaneLatency;
};
AiLaneDefaults& ai_lane_defaults();

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
    // OPT2 t01 / t02 (TacticalAiConfig::stagger / path_latency): per-player planning phases; the land path finder's
    // background recompute latency (0 = the original timing).
    bool stagger = true;
    uint32_t path_latency = galactic::kDefaultAllPairsLatency;
    // OPT2 t10 (TacticalAiConfig::pp_streams / pp_latency): slot-10 solves on their own RNG streams, applied pp_latency
    // frames after their slice; pp_streams false = the original synced draws in proposal order.
    bool pp_streams = true;
    uint32_t pp_latency = ai::tactical::kDefaultPpLatency;
    // sp3 (TacticalAiConfig::ai_lane / k_ai, docs/SIM_PARALLEL_DESIGN.md 5.1): the AI lane, off by default; the process
    // default (ai_lane_defaults) is what a setup starts with, so every host that builds one follows the command line.
    bool ai_lane = ai_lane_defaults().on;
    uint32_t k_ai = ai_lane_defaults().k_ai;
};

// The AIPlayerType of a non-human, playable, non-neutral player: its faction's Basic_AI (FactionRules::basic_ai, faction
// +0x378; AI_TACTICAL.md "Skirmish slots use Faction.Basic_AI"); "" for none or "None". modc t03: from the data, not by
// faction name (FoC's Basic_AI values are the old built-in mapping Empire -> BasicEmpire, Rebel -> BasicRebel,
// Underworld -> AI_Player_Underworld, so base-FoC battles are unchanged).
std::string default_tactical_ai_type(int32_t player, const galactic::GalacticSim& galaxy);

// a2h t05: the session data of the tactical AI, one loader for every battle host (alamo_headless --skirmish-ai and
// galactic --battles, alamo_game): the tactical_plan_files of both modes (list = the VFS glob lister; empty = the plan
// lists are left as they are), the difficulty table (load_tactical_ai_difficulties) and the GameConstants values
// (TacticalAiConstants::load; gc null or a missing tag keeps the current value, i.e. the default).
void load_tactical_ai_data(TacticalAiSetup& s, const FileProvider& files, const data::GameConstants* gc,
                           const std::function<std::vector<std::string>(std::string_view)>& list);

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
    // a1h t02: the object-goal follow records' 30-frame re-path (the first step of update(); public for the host tests).
    void follow_service();
    // sp3 t04 (AI lane, docs/SIM_PARALLEL_DESIGN.md 5.1): with TacticalAiConfig::ai_lane on, the AI's unit orders (moves,
    // MoveToObject, Attack, the Lua object orders Move_To / Attack_Move / Guard_Target / Stop) are not executed when given:
    // execute() answers with predict_order() and queues an accepted order as a command due on global frame N + k_ai (N =
    // the frame of the AI update). apply_lane_batch(frame) runs at the start of a tick (SimHost T2, before the players'
    // commands) and executes every queued order due by then in (due, player, sequence) order with the same rules as
    // before (execute_order), signals included. The other AI writes stay immediate until t05. Lane off: nothing queued.
    void apply_lane_batch(uint64_t frame);
    // sp3 t02: with the lane on, update() runs the world as TacticalAiWorld::publish() (critical lane) + think() (the AI
    // lane; inline until t03 moves it to the AI thread). Each think() is a job whose outputs - the orders it queued and
    // the digest of the AI world after it - become visible only when the job is collected: at the next update(), or at
    // the start of the tick its orders are due (k_ai = 1), whichever comes first; the battle.ai CRC hashes the digest of
    // the last collected job, so the CRC never reads the world while a job may run. lane_sync() waits until the running
    // job has finished (saves; inline until t03), without collecting it: the save keeps it as the pending job.
    void lane_sync() {}
    size_t lane_pending() const { return lane_batch_.size(); }
    uint32_t lane_mismatches() const { return lane_mismatch_; }  // applied with another result than predicted [diagnostic]
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
    // s78f t08: CombatSystem::attack_state_events of the tick just run (the AI updates after the battle tick).
    std::vector<ai::tactical::AttackStateSignal> attack_state_signals() override;
    // S7 t15 A1: TacticalBattle::ability_finished_events of the tick just run (signal 0x29, in order).
    std::vector<int32_t> ability_finished() override;
    // S7 t21: TacticalBattle::movement_locked (0x39ad40) for the ambush member list.
    bool movement_locked(int32_t object_id) override;
    // S7 t27: the L1 weather system's Sight_Range_Modifier for the type's Weather_Category (every owner weather-affected
    // [port], as the fog's land sight range).
    float sight_weather_factor(int32_t type, int32_t object_id) override;
    // HIJACK_UNIT (AI_GALACTIC_3 §4.3): GalacticSim::magic_spawn at the battle planet, a surface unit launched into the
    // orbit fleet (FAILED when that fails, e.g. blockaded), TacticalBattle::add_reserve; returns the new pool entry.
    // Refused before spawning when the type is not in the battle tables. [port] the galactic AI free store is not touched.
    ai::tactical::TacticalReinforcement hijack_unit(int32_t player, int32_t type, bool steal) override;
    // Active_By_Default re-apply (TACTICAL_GAPS §1.2, A1_READINGS §28.8): AbilitySystem::apply_active_by_default for the
    // player's live battle object. OrderLockDone: always (the AI released the script lock, 0x3a8e50(obj, 0), first; the
    // ability share of the counter is not touched). TaskForce: skipped while movement is locked (0x39ad40: ability
    // share AbilitySystem::immobilize_count > 0, script orders_locked / locomotor_suspended; [port] HUNT active stands
    // in for the locomotor term).
    void unit_released(int32_t player, int32_t object_id, ai::tactical::TacticalAiHost::UnitRelease path) override;
    // a1k t01 (A1_READINGS §39.2, 0x5ff610): a unit left a task force -> its combat priority-set override is cleared
    // (CombatSystem::set_priority_override(id, -1): the type's Targeting_Priority_Set again; hard points share the unit's
    // set in the combat model). cu8f t04a (CU8F_T04_T06_RE §1.1): and its stickiness threshold, then each TEAM member's,
    // := the type's Targeting_Stickiness_Time_Threshold (CombatSystem::reset_stickiness).
    void unit_left_task_force(int32_t object_id) override;
    // refixf t06 - space TF.Reinforce ring search (AI_TACTICAL §7.4, TACTICAL_GAPS §4.2). Entry facing = the player's side
    // deploy facing (TacticalBattle facing_ [port: stands in for the mode +0x288 record]); playable box = the mode bounds.
    // Prevention = live objects with Reinforcement_Prevention_Radius whose owner is not allied (not the player, and an
    // enemy in the combat relation table, or no owner). Drop test: box, prevention, fog (force 1; fog builds), then the
    // space occupancy sweep (SpaceOccupancy::query_segment, statics included [M]) of the pool entry's type footprint
    // (movement radius) from p - sweep x (cos, sin)(facing) to p over frames [now, now + 115] on its Space_Layer (raw
    // mask, 0 -> 4) | 8 [M: the moving query stands in for the swept box; state-4 reservations are not tolerated].
    bool space_drop_frame(int32_t player, ai::tactical::SpaceDropFrame& out) override;
    bool space_prevention_hit(int32_t player, const ai::tactical::Vec3f& p) override;
    bool space_drop_ok(int32_t player, int32_t key, const ai::tactical::Vec3f& p, float facing_deg, float sweep) override;
    // a1g t04 follow-up - land TF.Reinforce drop validity: TacticalBattle::land_drop_valid (a1h t06: moved to the host so the
    // Reinforce command re-runs it) with this battle's mode. AI drops pass human = false (movers ignored, landing
    // transports block).
    bool land_drop_ok(int32_t player, int32_t key, const ai::tactical::Vec3f& p) override { return land_drop_valid(player, key, p, false); }
    // q6 t19 (A1_READINGS §33.2.2): the units the last executed task-force Attack_Move on an object dropped because
    // CombatSystem::order_attack_issue refused them (cleared at every execute).
    std::vector<int32_t> attack_order_refused() override { return attack_refused_; }
    // q6 t19: TacticalBattle::production_done of the tick just run (land builds that went to the reinforcement pool).
    std::vector<ai::tactical::TacticalAiHost::ProductionCompleted> production_completed() override;
    bool land_drop_valid(int32_t player, int32_t key, const ai::tactical::Vec3f& p, bool human);
    // a1j t01 - land TF.Reinforce with BattleConfig::landing_unload_timed (A1_READINGS §36): reinforce_in_flight = the
    // player's timed landing object of pool entry `key` (participant galactic id) has not finished its unload;
    // reinforcements_unloaded = TacticalBattle::landings_unloaded() of the tick just run (owner, galactic id, units).
    bool reinforce_in_flight(int32_t player, int32_t key) override;
    std::vector<ai::tactical::ReinforcementsUnloaded> reinforcements_unloaded() override;
    // --- end battle host ---

private:
    // sp3 t01 (PERF_AUDIT S6): the type facts fill() and the per-object scans (population_cap, space_prevention_hit) read for
    // every object, resolved once per battle from the string-keyed GameObjectTypes getters (types do not change after
    // load). Index = objects::TypeIndex; the extra last entry stands for an out-of-range index (the getters' field
    // defaults, no behaviours). Built serially by ensure_type_facts(); read-only (also from fill's parallel build) after.
    struct TypeFacts {
        bool has_land_evaluator = false, has_space_evaluator = false;
        bool last_state_visible_under_fow = false, initial_state_visible_under_fow = false;
        bool create_team = false, is_dummy = false, stay_in_transport = false;
        bool valid_target = false;  // Collidable_By_Projectile_Living && Is_Valid_Target
        bool squadron_member = false, company_member = false;
        bool indigenous = false, dummy_ground_structure = false, dummy_ground_base = false;  // behaviour union 0x20 / 0x5F / 0x5B
        bool dummy_orbital_structure = false, dummy_star_base = false;                      // behaviour union 0x60 / 0x5C
        bool influences_capture = false;  // Influences_Capture_Point (in-range types only)
        int32_t base_level = 0;
        int32_t additional_population_capacity = 0;
        float reinforcement_enabling_radius = 0.0f;
        float reinforcement_prevention_radius = 0.0f;
    };
    std::vector<TypeFacts> type_facts_;
    void ensure_type_facts();
    const TypeFacts& type_facts(objects::TypeIndex t) const {
        return t < type_facts_.size() - 1 ? type_facts_[t] : type_facts_.back();
    }
    float type_population(int32_t type) const;  // A2 tactical: pop(T, mode) 0x373500
    // OPT2 t10: build_pad_check's blocker scan. The live Influences_Capture_Point objects (store order) are listed once
    // per build_pads call instead of once per pad, and the type flag is memoised per type (0 unknown, 1 no, 2 yes).
    struct CaptureInfluencer {
        objects::ObjectHandle h;
        int32_t owner = -1;
        Vec3 position;
    };
    bool influences_capture(objects::TypeIndex t) const;
    // sp3 t02: the pad checks read the battle, or (lane view) the snapshot store and the captured pad state / relations
    struct PadEnv;
    std::vector<CaptureInfluencer> capture_influencers(const objects::ObjectStore& st) const;
    ai::tactical::PadCheck pad_check(const PadEnv& env, int32_t player, int32_t pad, const std::vector<CaptureInfluencer>& influencers) const;
    std::vector<ai::tactical::TacticalPad> build_pads_in(const PadEnv& env, int32_t player) const;
    float allow_reinforcement_fraction_ = 0.0f;  // A2 tactical: Allow_Reinforcement_Percentage_Normalized
    bool use_reinforcement_points_ = true;       // a1g t04 follow-up: Use_Reinforcement_Points (GC +0x118a0)
    void standin_orders();
    // a1h t02 (A1_READINGS §33.4 / §33.5 rule 2): a group move with an object goal (Guard_Target or Attack_Move on an
    // object, kind 3; s78n t26 review round 2: Move_To on an object is a kind-1 position snapshot and is not followed)
    // follows the object in the movement layer, not by AI re-orders: at most once per 30 frames the group is
    // re-pathed when the object is more than R (half the lead member's attack range) from the recorded goal. Any newer
    // order for a unit takes it out of its follow record (signal 0xb, §33.3); an empty record or a dead object ends it.
    struct ObjectFollow {
        std::vector<objects::ObjectId> units;
        objects::ObjectId target = objects::kNoObjectId;
        float gx = 0.0f, gy = 0.0f;  // the recorded goal point (formation +0x210..+0x218)
        float r = 0.0f;              // R
        float threat = 0.0f;         // the move's threat tolerance (kThreatOff = none)
        uint32_t next = 0;           // the next battle frame the formation update may re-path (+0x1d8)
        // s78n t25.4 (MOVEMENT §4.3.2 items 2 / 8): a land Guard_Target on an object - the re-path is the guard block's
        // re-plan (TacticalBattle::land_attack_order, guard, replan: the slot kept). Saved (battle.ai 4), hashed when true.
        bool guard = false;
    };
    std::vector<ObjectFollow> follows_;  // issue order
    void follow_drop(const std::vector<objects::ObjectId>& units);
    void follow_add(const std::vector<objects::ObjectId>& units, objects::ObjectId target, float x, float y, float threat,
                    bool guard = false);
    commands::RejectReason execute_order(uint8_t player, const commands::EventData& ev);
    // s78f t10: `flags` / `goal_object` = the order's movement block (mv::MoveOrder::flags / goal_object), the same values the
    // host's fighter formation record gets (TacticalBattle::fighter_order), so the movement layer's fighter leash and the
    // host's chase tests read one block.
    // s78n t20 fix: `object_block` - the move stands for an object-goal block (kind 3) although `goal_object` names
    // none (an attack approach, the object follow's re-path): no destination check (MOVEMENT 4.3.1 item 2).
    bool move_unit(objects::ObjectId id, float x, float y, float threat_threshold = ::alamo::tactical::movement::kThreatOff,
                   uint8_t flags = 0, objects::ObjectId goal_object = objects::kNoObjectId, bool object_block = false);
    bool is_fighter(objects::ObjectId id) const;  // s78f t10: a space unit on the fighter locomotor

    TacticalBattle* b_;
    TacticalMode* mode_;
    SimHost* host_;
    std::vector<ai::tactical::HintZone> zones_;
    ai::tactical::ZoneLocator zone_locator_;  // position -> zone (A1_READINGS §4)
    std::unique_ptr<ai::tactical::TacticalAiWorld> world_;
    uint32_t order_listener_ = 0;   // S7 t31: TacticalBattle::add_order_replaced_listener token (world_->order_replaced)
    std::vector<uint8_t> ai_side_;  // per player: has a tactical AI
    uint32_t stale_ = 0;  // object orders whose target died before execution (no-ops)
    // sp3 t04: the AI lane's queued orders (header: player, source AI, sequence, scheduled_tick = due frame), in issue
    // order; hashed and saved with the lane on.
    std::vector<commands::Command> lane_batch_;
    // sp3 t02: the TacticalAiHost answers think() reads, captured on the critical lane at each AI slot (capture_lane_view,
    // before TacticalAiWorld::publish) with the lane on; the overrides answer from it while `valid` (a load clears it: the
    // relink's fill reads the battle). Per player: index = player id.
    struct LaneView {
        bool valid = false;
        std::vector<std::vector<ai::tactical::TacticalReinforcement>> pool;
        std::vector<uint8_t> can_reinforce;
        std::vector<float> pop_cap, pop_used;
        std::vector<ai::tactical::SpaceDropFrame> drop_frame;
        std::vector<uint8_t> drop_frame_ok;
        std::vector<uint8_t> enemy;  // players x players: CombatSystem::is_enemy
        uint32_t frame = 0;                    // the battle frame
        int32_t allied_n = 0;                  // galactic players
        std::vector<uint8_t> allied;           // allied_n x allied_n: TacticalBattle::bt_allied
        std::vector<std::string> faction;      // per galactic player: faction name
        std::vector<int32_t> live_units;       // live, not retreated battle unit ids, ascending
        struct Pad {
            objects::ObjectId pad = objects::kNoObjectId, contents = objects::kNoObjectId;  // contents: built, else UC
            uint32_t cooldown_end = 0;
        };
        std::vector<Pad> pads;  // TacticalBattle::pads_ (ascending pad id)
        std::vector<int32_t> locked;                         // movement_locked object ids, ascending
        std::vector<std::pair<int32_t, int32_t>> in_flight;  // reinforce_in_flight (owner, key), ascending
        std::vector<float> weather_sight;                    // per weather_categories_ index
        ai::tactical::DamageTrackingStore damage;
        // sp3 slice 4c: what predict_order reads - the battle units (TacticalBattle::units_, ascending id), the containers
        // (containers_ order; `target` = team_target answers it), per player the retreat gate (retreat_block == 0), and
        // whether the battle has tactical production.
        struct Unit {
            objects::ObjectId id = objects::kNoObjectId;
            int32_t owner = -1;
            uint32_t container = 0xFFFFFFFFu;  // index into teams
            uint8_t live = 0, movable = 0;     // alive and not retreated; movable
        };
        std::vector<Unit> units;
        struct Team {
            objects::ObjectId id = objects::kNoObjectId;
            uint8_t removed = 0, target = 0;
        };
        std::vector<Team> teams;
        std::vector<uint8_t> retreat_ok;
        bool production = false;
    };
    LaneView view_;
    // sp3 slice 4b: the ability / combat records the AI VMs' bindings read with the lane on (ObjectBridge::lane_abilities,
    // set at each capture and cleared by a load: before the first capture the bindings act live, on the critical lane).
    lua::LaneAbilityView lane_abilities_;
    void capture_lane_abilities();
    void queue_lane_write(const lua::LaneWrite& w);
    // sp3 slice 4d: unload records of lane Reinforce applies that spawned their units at once (or were refused: no units),
    // handed to the AI with the battle's own at the next AI slot (the same tick: never pending at a save).
    std::vector<ai::tactical::ReinforcementsUnloaded> lane_unloaded_;
    bool landing_in_flight(int32_t player, int32_t key) const;  // the battle's timed landing of `key` (live)
    void apply_lane_write(const commands::Command& c);  // a kLaneWriteSubtype command of the batch
    static bool lane_write_command(const commands::EventData& ev);
    std::vector<int32_t> weather_category_;  // per type: index into weather_categories_ (-1: none)
    std::vector<std::string> weather_categories_;
    void capture_lane_view();
    // sp3 slice 4c: TacticalBattle::unit_by_id / team_units / hostile_owner on the view (predict_order on the AI lane).
    const LaneView::Unit* view_unit(objects::ObjectId id) const;
    std::vector<objects::ObjectId> view_team_units(uint8_t player, uint32_t wire) const;
    bool view_hostile(uint8_t player, int32_t owner) const;
    bool lane_view() const { return view_.valid; }
    struct LaneJob {
        bool pending = false;
        uint64_t frame = 0;   // global frame of the update
        uint64_t digest = 0;  // StateHasher digest of the world after think()
        std::vector<commands::Command> batch;  // the orders it queued (execute() appends while it runs)
    };
    LaneJob lane_job_;
    uint64_t lane_digest_ = 0;  // digest of the last collected job (hashed and saved)
    void lane_collect();
    uint32_t lane_seq_ = 0;
    uint32_t lane_mismatch_ = 0;  // not saved, not hashed: orders refused, writes counting other than predicted
    static bool lane_batched(const commands::EventData& ev);
    void count_executed(const commands::EventData& ev, commands::RejectReason r);  // executed_ (diagnostic)
    // The rejections of execute_order that depend on the order and the battle state alone, without side effects.
    commands::RejectReason predict_order(uint8_t player, const commands::EventData& ev);
    commands::RejectReason execute_live(uint8_t player, const commands::EventData& ev);  // execute_order + signals + count
    std::vector<int32_t> attack_refused_;  // q6 t19: attack_order_refused() of the last execute (not saved: read at once)
    std::map<std::string, std::pair<uint32_t, uint32_t>> executed_;  // event name -> (applied, rejected) [diagnostic]
    std::vector<std::pair<uint64_t, int32_t>> opp_prev_;  // battle host: (unit id << 8 | hp, target), ascending key
    // UnitAI data-pack re-projection stamps (A1_READINGS §14) of the listed UNIT_AI objects, drawn once per object
    // (draw_unit_ai_stamps, two synced draws) when fill() first lists it; ascending id.
    std::map<objects::ObjectId, int32_t> unit_ai_stamps_;
    std::vector<objects::ObjectId> team_units(uint8_t player, uint32_t wire) const;  // battle host: unit or TEAM members
    // S7 t26: signal 0xb (a new order replaces the unit's order) on every unit an executed AI / script order took.
    void order_signals(uint8_t player, const commands::EventData& ev);
    // The ordered-attack hostility test (0x39a450 -> Is_Enemy 0x2824f0, relation == 1): the battle's relation table
    // (TACTICAL_COMBAT §2.10, S7 t30), where an Is_Neutral player is relation 2.
    bool hostile_owner(uint8_t player, int32_t owner) const;
    lua::TacticalScriptHooks tactical_;    // battle host: script_gaps.hpp hooks of the tactical AI VMs (L1 final list)
#ifdef ALAMO_LUA_HAVE_FOG
    // refixf t03: object_hidden (Find_Nearest's FOW_Object_Hidden test); S7 t34: fog = the battle's fog (FogOfWar.*).
    lua::FogScriptHooks fog_hooks_;
#endif
    lua::RemainingScriptHooks remaining_;  // L1 bindings: script_remaining.hpp hooks of the tactical AI VMs (Divert; Int-final: strikes, damage stats, fields, weather, shields)
};

}  // namespace alamo::sim
#endif

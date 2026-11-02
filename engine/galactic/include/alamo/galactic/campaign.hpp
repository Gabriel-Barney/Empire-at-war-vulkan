// S3 "Galactic economy & fleets": the galactic mode's simulation state and per-frame update
// (docs/re/GALACTIC_ECONOMY.md = GE, MAIN_LOOP.md, OBJECT_RUNTIME.md, COMMANDS.md).
//
// Ownership: GalacticSim owns the galactic data packs (players' credit store, PlanetaryDataPack-like
// planet state, FleetDataPack-like fleet state, production queues) and is the single mutation owner of
// them. Planets, fleets, starbases, structures and units are game objects in the caller's galactic
// primary ObjectStore (OBJECT_RUNTIME.md section 7): planets get PLANET, fleets FLEET + LOCOMOTOR, so
// the store's serviced list gives the original's newest-first service order. Unit -> fleet/planet
// membership lives here (ObjectStore has no container setter; ships carry no container).
//
// Per galactic frame f (update(), GE 5.1 as corrected by AI_GALACTIC_3 §5):
//   1. object services, newest registered first: fleet locomotors (motion, arrival, merge) and the
//      planet service: derived owner + political-control ramp, then ProductionBehavior::Service (the AI
//      market hook, then the planet's ground and space queues);
//   2. the planets' delayed-spawn (hero respawn) lists 0x3fc750 (P4 6.2), planet row order;
//   3. the store's delayed creations and deferred-delete flush (ObjectStore::update);
//   4. the fiscal cycle when next_cycle_frame <= f (income, black-market reroll); the first one is on frame 1350 (the
//      mode's frame-0 cycle runs before the galaxy exists: start() applies its empty bookkeeping);
//   5. the mode frame advances (T12), unless the galactic pause counter is raised (then nothing runs).
// Commands are applied by the host before update() (CommandPipeline::begin_tick -> handle_command);
// AI and Lua call execute() directly (COMMANDS.md section 5).
//
// Determinism: every container is a vector walked in index order (planets in <Locations> row order =
// the category-3 list order, players in player-list index order, fleets in creation order), all money
// and speed arithmetic is float32 in the documented order, the only RNG is the synced SimRng (one
// black-market draw per qualifying planet per fiscal cycle, planet order). The only parallel step is the
// owner derivation (pure, per planet) when every fleet services before every planet; its results are
// applied serially, so any ParallelFor gives identical state.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/commands/events.hpp"
#include "alamo/core/galaxy.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/economy.hpp"
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/galactic/galactic_events.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/galactic/planet_graph.hpp"
#include "alamo/galactic/special_systems.hpp"  // P4 sim
#include "alamo/galactic/visibility.hpp"       // GRC 2
#include "alamo/objects/object_store.hpp"

namespace alamo::data {
class GameDatabase;
}

namespace alamo::galactic {

using objects::ObjectHandle;
using commands::RejectReason;

inline constexpr int32_t kNoPlayer = -1;
inline constexpr uint32_t kNoFleet = 0xFFFFFFFFu;

enum QueueKind : uint8_t { kGroundQueue = 0, kSpaceQueue = 1 };
// Income breakdown entry kinds (GE 1.4 step 3). Ability entries (Planet_Income_Bonus_Ability,
// Income_Stream_* [L]) use kIncomeAbility and carry an expiry frame.
enum IncomeKind : uint8_t { kIncomeBase = 0, kIncomeForce = 1, kIncomePolitical = 2, kIncomeTrade = 3, kIncomeAbility = 4 };

// ---- setup ------------------------------------------------------------------------------------------------
struct PlayerSetup {
    uint32_t faction = kNoIndex;
    bool human = false;               // Is_Human (+0x62)
    bool ai = false;                  // has an AI system (+0x360)
    uint32_t difficulty = kNoIndex;   // AI difficulty adjustment; kNoIndex = all-1.0
    float credits = 0.0f;             // Starting_Credits (set directly, not through Add_Credits) [L]
    int32_t tech_level = 1;           // Starting_Tech_Level
    int32_t max_tech_level = 5;       // Max_Tech_Level (P +0x88); 5 when the campaign names none [L: MP_Default_Max_Tech_Level]
    PlanetIndex home = kInvalidPlanet;  // Home_Location (GRC 1.4: the local player's home is revealed at start)
};
struct StartingForce {
    int32_t player = kNoPlayer;
    PlanetIndex planet = kInvalidPlanet;
    uint32_t type = kNoIndex;
};
struct GalacticSetup {
    GalaxyMap map;                       // planet rows (<Locations> order) + routes
    std::vector<uint32_t> planet_type;   // per row: TypeIndex of the planet; kNoIndex = no planet object
    std::vector<PlayerSetup> players;    // player id = index (player-list order)
    std::vector<StartingForce> forces;   // spawned in this order
    // Per row political leaning (GE 4.1 step 6). Empty = owner of the row's first starting force, else
    // the neutral player [L].
    std::vector<int32_t> leaning;
    // REMAINING_GAPS 1.4 campaign start (0x292590): Set_Tech_Level(start, sweep) per player. False for skirmish
    // (0x294fa0: tech 1, no sweep) and hand-built setups.
    bool tech_sweep_at_start = false;
    // GALACTIC_REVEAL_CORRUPTION (GRC) 0: campaign Planet_Auto_Reveal (CampaignDataClass +0xCB, ctor true) -> galactic
    // mode +0x340.
    bool planet_auto_reveal = true;
};

struct CampaignOptions {
    std::string human_faction;                  // empty = <Starting_Active_Player>
    std::string ai_difficulty = "Normal_Default";
};
// Builds a GalacticSetup from a campaign definition. Players: every faction the campaign names
// (Starting_Credits / Starting_Forces / Home_Location / AI_Player_Control / Starting_Active_Player)
// plus the first Is_Neutral faction, in FactionFiles order [L: the original's player-list order is not
// recovered]. AI = an AI_Player_Control entry other than "None".
GalacticSetup build_campaign_setup(const data::GameDatabase& db, const GalacticRules& rules, std::string_view campaign,
                                   const CampaignOptions& options = {}, std::vector<std::string>* diagnostics = nullptr);

// ---- state ------------------------------------------------------------------------------------------------
struct IncomeEntry {
    uint8_t kind = kIncomeBase;
    float value = 0.0f;
    int32_t expiry = econ::kNoExpiry;  // removed when expiry < now
    // P4 sim: entries of Planet_Income_Bonus / _Gambling abilities (P4 4.1 / 4.2), keyed by (ability name CRC,
    // owner object). ability == 0: an S3 / story entry (none of these fields used or hashed).
    uint32_t ability = 0;        // special_name_crc(ability name)
    int32_t source = 0;          // ObjectId of the ability's owner (structure / unit)
    uint32_t tag = 0;            // source tag: CRC(Specific_Mod_Source_Text), else the owner's Text_ID CRC
    int32_t next_eval = 0;       // gambling: next evaluation frame
    uint32_t def = 0xFFFFFFFFu;  // SpecialRules::abilities index (derived from `ability`; not hashed)
};

// ObjectUnderConstructionClass (GE 2.1).
struct BuildEntry {
    uint32_t type = kNoIndex;
    float build_time = 0.0f;  // T seconds (+0x10)
    int32_t start = 0;        // timer start frame (+0x14)
    int32_t duration = 0;     // timer frames
    float remaining = 0.0f;   // remaining seconds when last advanced (+0x1c)
    int32_t player = kNoPlayer;
    float cost = 0.0f;        // C (+0x28)
};

struct PlayerState {
    uint32_t faction = kNoIndex;
    bool human = false;
    bool ai = false;
    uint32_t difficulty = kNoIndex;
    econ::CreditAccount account;  // credits +0x70, cap +0x74
    int32_t tech_level = 1;
    float tech_cost_mult = 1.0f;  // m_techupg (player +0x270 fold, min 0.0001) [M]
    bool cheat = false;           // player +0x3c0 [M]
    // P4 sim (special_systems.hpp)
    std::vector<uint32_t> bm_purchased;          // purchased black-market items (item indices, purchase order; P +0x430)
    bool bm_tutorial = false;                    // P +0x448 (Lua Set_Black_Market_Tutorial)
    bool sabotage_tutorial = false;              // P +0x449 (Lua Set_Sabotage_Tutorial)
    std::array<int32_t, 8> corruptions_completed{};  // P +0x458, per corruption type
    std::vector<uint32_t> locked_types;          // Lock_Tech list (P +0x1C0), lock order
    std::vector<uint32_t> unlocked_types;        // unlocked list (P +0x1B0, L1_S4_REMAINING 2.9), unlock order
    std::vector<uint32_t> production_disabled;   // production-disabled list (P +0x1E0; story ENABLE/DISABLE_BUILDABLE)
    // OI galactic (STRATEGIC_GAPS 1.3)
    int32_t max_tech_level = 5;                  // P +0x88 (step 7); default [L], story SET_MAX_TECH_LEVEL writes it
    std::vector<std::pair<uint32_t, int32_t>> built;  // lifetime build counts (P +0x198 map, 0x2821b0), first-build order
};

// OI galactic: buildability 0x2804d0 (STRATEGIC_GAPS 1.3). `reason` = the out-argument code the GUI / Lua read (0 = the
// failing step writes none); `detail` = the step's detail value (required tech level, the missing type); `step` = the
// first failing step (1..24), 0 when buildable.
enum BuildReason : int32_t {
    kBuildOk = 0, kBuildTechLevel = 1, kBuildGroundBase = 2, kBuildStarBase = 3, kBuildRequiredStructure = 4,
    kBuildRequiredPlanet = 5, kBuildLimit = 6, kBuildInitiallyLocked = 7, kBuildPlanetRestricted = 8,
    kBuildHostileFleet = 10
};
struct BuildCheck {
    bool ok = false;
    int32_t step = 0;
    int32_t reason = kBuildOk;
    uint32_t detail = 0xFFFFFFFFu;
};

// OI galactic: an event-driven galactic victory (STRATEGIC_GAPS 1.4: 0x343b90 "VICTORY - Victory type %d"), recorded
// by the sim whatever the campaign's conditions; victory.hpp grants it to players whose side lists the type once the
// countdown (mode frames) ran out. Types: 3 Super_Weapon_Destruction, 6 Super_Weapon_Destroys_Last_Enemy_Planet,
// 7 Super_Weapon_Kills_Enemy_Leader.
struct VictoryClaim {
    int32_t player = -1;
    uint8_t type = 0;
    int32_t frame = 0;      // mode frame of the declaration
    int32_t countdown = 0;  // frames
};

// Player relation (player +0x370 table; L1_S4_REMAINING "Relations": 0 = allied incl. self, 1 = enemy). The values
// match lua::PlayerRelation so the Lua bridge can cast. Defaults [inferred, = the former stand-in]: Self on the
// diagonal, Neutral when either faction Is_Neutral, else Enemy.
enum class Relation : uint8_t { Self = 0, Ally = 1, Neutral = 2, Enemy = 3 };

struct PlanetState {
    bool is_planet = false;  // row has a planet object (scenery rows stay inert)
    uint32_t type = kNoIndex;
    ObjectHandle object;
    Vec3 position{};
    // ownership (data pack +0x68 control, +0x6c owner, +0x70 target control)
    bool initialised = false;  // D +0x1c9
    int32_t owner = kNoPlayer;
    float control = 0.0f;
    float target_control = 1.0f;
    int32_t leaning = kNoPlayer;
    // income
    int32_t base_value = 0;        // D +0x228 runtime base (from Planet_Credit_Value) [L init]
    float black_market = 1.0f;     // D +0x2a0
    bool black_market_influences = false;
    bool destroyed = false;        // D +0x2c8
    int8_t corruption = kCorruptionNone;  // D +0x2f4
    int32_t corrupting_player = kNoPlayer;
    // GRC 4.1 corruption transition. D +0x2e2 is the "gaining" flag (the field keeps its older name: engine/ai reads it).
    bool corruption_protected = false;    // D +0x2e2 gaining (transition to corrupted in progress)
    bool corruption_removing = false;     // D +0x2e3
    uint32_t corruption_start = 0;        // D +0x2e8 start frame
    uint32_t corruption_end = 0;          // D +0x2ec end frame
    uint32_t corruption_changed = 0;      // D +0x2f0 frame of the last completed change
    int8_t corruption_pending = kCorruptionNone;  // D +0x2f8 type being gained (-1 while removing / idle)
    int32_t corruption_pending_player = kNoPlayer;  // [port] corrupting_player once a gain completes
    // GRC 4.5 presentation: particle object D +0x300 present; pairs D +0x308..+0x31c (brightness, scale, energy as
    // start/end), rebuilt only where the original calls 0x3ff230. Not hashed (presentation).
    bool corruption_particles = false;
    std::array<float, 6> corruption_pairs{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // GRC 1: revealed byte D +0x2c9 (the local player's, saved) and the orbit-presence delay counter D +0x2cc (not
    // saved, not hashed).
    bool revealed = false;
    int32_t reveal_delay = 0;
    // GRC 2.1: the stored visibility modifier lists D +0x1B8, one per player index (lazily sized; empty = no entries).
    // Only the explicit entries live here (Ground_Activated spies, story SET_PLANET_VISIBILITY_LEVEL, host adds); the
    // baseline, presence and always-on spy entries are derived at fold time (see GalacticSim::visibility_mask). Hashed in
    // the "VIS1" tail once any list holds an entry.
    std::vector<std::vector<VisibilityModifier>> visibility;
    float piracy_pool = 0.0f;             // D +0x2a4
    int32_t siphon_player = kNoPlayer;    // D +0x200
    float siphon_fraction = 0.0f;         // D +0x204
    int32_t siphon_until = 0;             // D +0x208
    float siphon_amount = 0.0f;           // D +0x20c
    int32_t halt_until = INT32_MIN;       // credit halt list folded: halted while now <= halt_until [M]
    bool min_income_set = false, max_income_set = false;  // D +0x280 / +0x281
    float min_income = 0.0f, max_income = 0.0f;           // D +0x278 / +0x27c
    float build_time_bonus = 0.0f;  // m_planet = 1 - bonus [M]
    float build_cost_bonus = 0.0f;  // m_planet_cost = 1 - bonus [M]
    bool hidden = false;            // D +0x2e0 inaccessible
    bool in_battle = false;         // galactic conflict list (battles are tactical: never set by S3)
    std::vector<IncomeEntry> income;               // breakdown list D +0x268 (order matters)
    std::array<std::vector<BuildEntry>, 2> queue;  // ground, space
    // bases and forces
    int32_t starbase_level = 0;           // D +0x148
    int32_t starbase_owner = kNoPlayer;   // D +0x13c
    ObjectHandle starbase;
    std::vector<ObjectHandle> structures;  // ground structures (level = count) [M]
    std::vector<ObjectHandle> surface;     // ground units (D +0xe0)
    // REMAINING_GAPS 6.1: the influence set D +0xf0 (object ids, first-insertion order), written by the launch reveal.
    // No sim reader is known [M: feeds planet visibility]; not hashed.
    std::vector<int32_t> influence;
    std::vector<uint32_t> orbit;           // fleet indices in arrival order (D +0xc8)
    std::vector<uint32_t> links;           // route rows touching this planet, route order (D +0x58)
    // P4 sim
    int32_t sabotage_count = 0;            // D +0x350 [M]: sabotages done here (cost exponent)
    std::vector<RespawnEntry> respawns;    // hero spawn list D +0x240 (P4 6.2), scheduling order
};

struct FleetState {
    bool alive = false;
    ObjectHandle object;
    int32_t owner = kNoPlayer;
    std::vector<ObjectHandle> contents;
    std::vector<uint8_t> carried;          // per member, last departure's carrier simulation
    PlanetIndex planet = kInvalidPlanet;   // orbiting (kInvalidPlanet while in transit)
    PlanetIndex from = kInvalidPlanet, to = kInvalidPlanet;  // current / last hop
    std::vector<PlanetIndex> path;         // locomotor +0xb8, source first
    uint32_t path_index = 0;               // index of the hop target (+0x80)
    float base_speed = 0.0f;               // +0x64
    float hop_speed = 0.0f;                // +0x68
    Vec3 position{};
    Vec3 facing{};
    bool moving = false;
    // Orbit slot (docs/re/GALAXY_MAP_PRESENTATION.md 1.3: fleet +0xc0 -> +0x48), the authoritative HUD slot state. Plain
    // data, serialisable as it is (N1: save `slot`, `want_planet`, `want_slot` per fleet and GalacticSim::slot_choices_).
    //   slot        0..kOrbitSlots-1 while orbiting a planet; unique among the fleets of one owner at that planet
    //               (owners have separate slot spaces: the HUD resolves collisions between owners). -1 while in transit
    //               or when the owner already has kOrbitSlots fleets there (it takes the first slot that frees up).
    //   want_*      a requested slot at the next arrival at `want_planet` (FleetManagement op 8 with a destination
    //               planet); consumed (cleared) by that arrival or by a later request. -1 / kInvalidPlanet = none.
    int8_t slot = -1;
    PlanetIndex want_planet = kInvalidPlanet;
    int8_t want_slot = -1;
};

// ---- A2 galactic: AI-facing hooks (docs/re/AI_GALACTIC_2.md §3.1, §5.4; COMMANDS.md id 8) -----------------------
// Income tick (AI_GALACTIC_2 §3.1, A2GAL_READINGS §1) [H]: inside the fiscal cycle's payment loop, right after a
// player's Add_Credits and before the next player is paid, one call for each non-human AI player whose cycle income
// I is not 0 (no call at all on a zero-income cycle). gain = the raw Player_Income I: before the AI
// Credit_Multiplier, the zero floor and the cap clamp; PAYG installments and refunds are not part of it.
// period_seconds = the income period T = Fiscal_Cycle_Time_In_Secs (0x4b2010, AI_GALACTIC_3 §8).
struct IncomeTick {
    int32_t player = kNoPlayer;
    float gain = 0.0f;
    float period_seconds = 0.0f;
    int32_t day = 0;    // galactic_day() once this cycle has finished (the counter increments after the loop)
    int32_t frame = 0;  // mode frame of the cycle
};
using IncomeHook = std::function<void(const IncomeTick&)>;

// FleetManagement (event id 8) ops. Op names are [M] (AI_GALACTIC §2.x, AI_GALACTIC_2 §5.4); payload use [port]:
//   kFleetForm   {target = planet, fleet = destination fleet (kNoObject = a new fleet), unit}: the unit leaves its
//                orbiting fleet at the planet and joins the destination (Launch_Units: launch, then form).
//   kFleetDetach {target = planet, fleet = source fleet or kNoObject, unit}: the unit becomes a fleet of its own.
//   kFleetMerge  {target = destination fleet, fleet = source fleet, unit = one unit or kNoObject = all of them}.
//   kFleetLaunch {target = destination fleet or kNoObject = the orbit fleet (found or made), unit = surface unit}.
//   kFleetLand   {target = the issuer's planet, fleet = orbiting fleet, unit = one ground unit or kNoObject = all}.
// Only the issuer's idle fleets in orbit of one planet take part; nothing moves between planets. Ignored (NotAllowed)
// while the galactic pause counter is raised (COMMANDS.md §6.2 "battle pending": counter 0x140b1540c = the galactic
// pause, STORY.md §4.1); MoveThroughObjects likewise.
// enqueue() refusal for the galactic population cap (AI_GALACTIC_3 §3.5; RejectReason::FirstCustom).
inline constexpr RejectReason kRejectPopulationCap = RejectReason::FirstCustom;

// OI galactic (STRATEGIC_GAPS 1.7, executor 0x5af260): the remaining ops.
//   kFleetJoin     {fleet = a fleet, a fleet member or a planet object, unit}: the unit (in an idle orbit fleet of the
//                  issuer, or on the issuer's surface: launched first) joins that fleet; a planet = land there (op 5).
//   kFleetMoveNew / kFleetMoveNewFlag {target = planet, unit, a = destination planet}: the unit becomes a new fleet that
//                  moves toward `a` (op 7's fleet flag 0x435680 is not modelled: same as op 6).
//   kFleetSlot     {fleet, slot, a}: orbit slot (FleetState::slot). a = kNoObject (or the fleet's own planet): the
//                  orbiting fleet moves to `slot`, or merges into the issuer's fleet already in it (the HUD's drop on an
//                  occupied slot). a = another planet: the slot the fleet takes on arrival there (FleetState::want_*),
//                  issued just before the move order; an occupied slot at arrival means merge into that fleet.
//   Ops 0 / 2 / 4 / 6 / 7 (a new fleet) take the slot to give it in `b` (0..kOrbitSlots-1; kNoObject = the first free
//   one; a taken slot is ignored). For ops 6 / 7 it is the slot at the destination `a`.
inline constexpr int32_t kOrbitSlots = 3;  // fleet slots per owner above a planet (g_planet_fleet slot 0..2)
enum FleetOp : uint8_t {
    kFleetForm = 0, kFleetJoin = 1, kFleetDetach = 2, kFleetMerge = 3, kFleetLaunch = 4, kFleetLand = 5,
    kFleetMoveNew = 6, kFleetMoveNewFlag = 7, kFleetSlot = 8
};
commands::EventData fleet_management_event(FleetOp op, uint32_t target, uint32_t fleet, uint32_t unit);
// With the extra id (+0x54, payload "a": ops 6 / 7 destination planet) and the slot (+0x5C, op 8).
commands::EventData fleet_management_event(FleetOp op, uint32_t target, uint32_t fleet, uint32_t unit, uint32_t a,
                                           uint32_t slot = 0);

// ---- S10 battle transitions hook (docs/re/BATTLE_TRANSITIONS.md; implemented by conflicts.hpp) ----------------
// Installed with GalacticSim::set_conflict_hooks (null = the S3 behaviour: no conflicts queued, Invade refused
// against hostile ground forces). Called from serial code only.
class ConflictHooks {
public:
    virtual ~ConflictHooks() = default;
    // A fleet of `attacker` stopped in orbit of `planet` and the orbit-entry test found a conflict (§1.2).
    virtual void on_space_conflict(PlanetIndex planet, int32_t attacker) = 0;
    // Invade against hostile ground forces (§1.3): `units` = the invading ground units in orbit. True =
    // queued (the command succeeds), false = refused.
    virtual bool on_invade(int32_t attacker, PlanetIndex planet, std::span<const ObjectHandle> units) = 0;
    // End of the galactic GOM service (§1.4), every galactic tick, also while the galactic pause is raised.
    virtual void on_service_tail() = 0;
    // 0x2b0350 (REMAINING_GAPS 6.4, BT §1.4 step 2): `player` is attacker or defender of a conflict in either queue.
    virtual bool in_queued_conflict(int32_t /*player*/) const { return false; }
};

class GalacticSim {
public:
    // `rules`, `store` (the galactic mode's primary store) and `rng` (the session's SYNCED instance) must
    // outlive the sim.
    GalacticSim(const GalacticRules& rules, objects::ObjectStore& store, SimRng& rng);

    // Creates planet objects (row order), the starting forces (setup order), the first ownership pass
    // and the credit caps. Returns false (with diagnostics) when the setup is unusable.
    bool start(const GalacticSetup& setup, std::vector<std::string>* diagnostics = nullptr);
    // Optional fan-out for the owner derivation (results do not depend on it).
    void set_parallel_for(ParallelFor pf) { pf_ = std::move(pf); }

    // One galactic mode tick (see the header comment).
    void update();
    int32_t frame() const { return frame_; }  // mode frame (+0x10)
    int32_t galactic_day() const { return cycle_counter_; }  // fiscal cycle counter (+0x370)
    int32_t next_cycle_frame() const { return next_cycle_frame_; }
    int32_t last_cycle_frame() const { return last_cycle_frame_; }
    // Galactic pause counter (GMM +0x2c, MAIN_LOOP 1.5): while > 0 the mode frame is frozen and nothing
    // is serviced.
    void push_pause() { ++pause_; }
    void pop_pause() { if (pause_ > 0) --pause_; }
    // The galactic "paused" query 0x4b2360: the pause counter, or a queued story-linked battle (REMAINING_GAPS 6.4).
    bool paused() const { return pause_ > 0 || linked_battle_pending_; }
    // REMAINING_GAPS 6.4 (GMM +0xf0): set when story LINK_TACTICAL or a corruption mission queues its battle (0x28bd70;
    // refused - returns false - while one is already queued), cleared when that battle starts (0x28c640). While set,
    // SpecialAbility, FleetManagement and move commands are refused [port: for every player; the original gates on the
    // local player being in a queued conflict].
    bool queue_linked_battle() {
        if (linked_battle_pending_) return false;
        linked_battle_pending_ = true;
        return true;
    }
    void clear_linked_battle() { linked_battle_pending_ = false; }
    bool linked_battle_pending() const { return linked_battle_pending_; }

    // ---- commands -----------------------------------------------------------------------------------
    // Applies one event for `player` (scheduled path and AI/Lua direct path alike). Handled:
    // MoveObjectToObject (object = fleet, target = planet), MoveThroughObjects (waypoint planets,
    // object = fleet), Production (build / cancel), Invade (land units from orbit), FleetManagement (FleetOp above:
    // form / detach / merge / launch / land, for the HUD's fleet drag, the AI and Lua alike), P4 sim: SpecialAbility
    // kinds 1 / 4 / 5 / 7 (special_systems.hpp; NotAllowed while the galactic pause is raised). Others (SpecialAbility
    // kind 6 included): Unhandled.
    RejectReason execute(uint8_t player, const commands::EventData& event);
    RejectReason handle_command(uint64_t tick, const commands::Command& cmd);
    // For CommandPipeline::set_handler.
    std::function<RejectReason(uint64_t, const commands::Command&)> command_handler();

    // ---- direct operations (Lua / story / tests) ------------------------------------------------------
    RejectReason enqueue(int32_t player, PlanetIndex planet, uint32_t type);
    // Cancel by index (0x403770). refund = the UI path (true).
    RejectReason cancel(int32_t player, PlanetIndex planet, uint32_t queue, uint32_t index, bool refund = true);
    // Re-evaluation (0x402490 / 0x402660): tail -> head, no refund, stops after deleting the head.
    void reevaluate_queue(PlanetIndex planet, uint32_t queue, int32_t level);
    RejectReason move_fleet(int32_t player, uint32_t fleet, PlanetIndex destination);
    // Explicit hop list (source first, consecutive planets must be graph neighbours).
    RejectReason move_fleet_path(int32_t player, uint32_t fleet, std::span<const PlanetIndex> path);
    // Battle retreat (0x2ba6c0 -> 0x39f380, BATTLE_TRANSITIONS §3.3a): the two-planet path [current, destination], one
    // straight hop whether or not a trade route joins them (no path finding). The normal departure / hop / arrival
    // machinery runs; the hop factor is the route's HS_Speed_Factor only when the planets are connected, else 1.0.
    // Refused (the fleet stays) when moving, not at a planet, the destination is its planet, or it cannot jump.
    RejectReason move_fleet_direct(int32_t player, uint32_t fleet, PlanetIndex destination);
    // Lua Launch_Units stand-in: the player's surface units at `planet` join the player's orbiting fleet.
    RejectReason launch_units(int32_t player, PlanetIndex planet, std::span<const ObjectHandle> units);
    // Invade stand-in: the player's ground units in orbit land (empty list = all of them). Refused when
    // hostile surface forces are present (that needs a tactical battle) [L].
    RejectReason invade(int32_t player, PlanetIndex planet, std::span<const ObjectHandle> units);
    // Removes a unit, structure, starbase or fleet (battle outcome / story stand-in).
    bool destroy_object(ObjectHandle object);
    float add_credits(int32_t player, float amount, bool notify_ai = true);
    float player_income(int32_t player, bool consume);
    float planet_income(PlanetIndex planet, bool gross);
    void recompute_credit_caps();

    // Instant corruption change (story / Lua / host stand-ins): cancels a running transition (0x4b6c50), sets the
    // type, rebuilds the particle pairs (0x3ff230), notifies. Timed changes: begin_corruption_gain / _removal.
    void set_corruption(PlanetIndex planet, int8_t type, int32_t corrupting_player);

    // ---- GRC: planet reveal (src/reveal.cpp) -------------------------------------------------------------------
    // The local player = the first human player in player order, pinned at start (kNoPlayer: nothing is revealed by
    // the pre-step). Multiplayer is not modelled (Is_Planet_Revealed would answer true).
    int32_t local_player() const { return local_player_; }
    // Is_Planet_Revealed 0x3fa420 (takes no player: the local player's byte): no planet / hidden -> false;
    // auto-reveal -> true; else the byte.
    bool is_planet_revealed(PlanetIndex planet) const;
    // The raw byte D +0x2c9: the human hop gate, trade-route visibility, the hyperspace fleet test.
    bool planet_revealed_raw(PlanetIndex planet) const;
    void set_planet_revealed(PlanetIndex planet, bool revealed);
    bool planet_auto_reveal() const { return auto_reveal_; }
    void set_planet_auto_reveal(bool on) { auto_reveal_ = on; }  // story ENABLE_GALACTIC_REVEAL (0x44)
    // Story rewards (GRC 1.5): REVEAL_PLANET (0x33) on every planet of a type (`on` = atoi(Param2) != 0) and
    // REVEAL_ALL_PLANETS (0x39).
    void reveal_planets_of_type(uint32_t planet_type, bool on);
    void reveal_all_planets();
    // True once after a pre-step revealed something on a frame > 0 (SFXEvent_Planet_Revealed, presentation).
    bool consume_reveal_sfx();

    // ---- GRC 2: planet visibility masks (src/visibility.cpp) ----------------------------------------------------------
    // visibility_mask(planet, player) = the OR fold (0x3fa490 / 0x3fd080 / 0x50c550; the list min/max clamps have no
    // writer and are not modelled) of
    //   * baseline (key (planet, 0); the local player only): Planet_Owner 0x9FFF when the planet's owner is the local
    //     player, else the local faction's default (faction_default_visibility);
    //   * presence (key (planet, 1); players with an AI system only): the player's faction default, 0x71 with an own
    //     live fleet in orbit, | 0x191 with an own live surface unit; Maximum [port: the signalled level -1] when the
    //     player owns the planet;
    //   * always-on spies (Galactic_Automatic System_Spy_Ability, TypeSpecial::auto_spy_mask) of the player's live objects
    //     at a planet Q (orbit fleet members, surface units, structures, the planet itself when owned) where Q == planet or
    //     the object's type has Galactic_Influence_Range r > 0 and |Q - planet|^2 <= r^2 (float, as launch_reveal);
    //   * the stored entries of the (planet, player) list, in list order (Ground_Activated spies, story, host).
    // The derived three are recomputed on every query [port: the original upserts them into the list on each refresh
    // 0x3a51e0, i.e. on reveals, ownership, arrivals, spies, story; between refreshes its entries can be stale]. They are
    // pure functions of hashed state, so only the stored entries enter the CRC. 0 for an invalid planet / player.
    uint32_t visibility_mask(PlanetIndex planet, int32_t player) const;
    struct VisibilitySources {
        uint32_t baseline = 0, presence = 0, auto_spy = 0, stored = 0;
        uint32_t mask() const { return baseline | presence | auto_spy | stored; }
    };
    VisibilitySources visibility_sources(PlanetIndex planet, int32_t player) const;
    // The stored entries of (planet, player), list order (empty for an invalid pair).
    std::span<const VisibilityModifier> visibility_modifiers(PlanetIndex planet, int32_t player) const;
    // Upsert by key (an existing entry keeps its list position; a new one is appended). seconds < 0 (-1): never expires;
    // else expiry = mode frame + (int)((float)FPS x seconds + 0.5f) (0: removed by the next frame's planet service).
    // False for an invalid planet / player.
    bool add_visibility_modifier(PlanetIndex planet, int32_t player, const VisibilityKey& key, uint32_t value, float seconds,
                                 uint32_t name_hash = 0);
    // Removes the entry with that key; false when there was none.
    bool remove_visibility_modifier(PlanetIndex planet, int32_t player, const VisibilityKey& key);
    // Story SET_PLANET_VISIBILITY_LEVEL (0x51, 0x461a60): P1 planet, P2 faction (player_of_faction: first match), P3 level
    // text (parse_visibility_level) or "none" (case-insensitive) = remove, P4 key text -> key (planet, crc(P4)) and name
    // hash crc(P4) (special_name_crc), P5 seconds (missing = -1 = never). False when nothing was changed (bad planet /
    // faction / level, or "none" without an entry).
    bool story_set_planet_visibility(PlanetIndex planet, std::string_view faction, std::string_view level, std::string_view key,
                                     float seconds = -1.0f);
    // System_Spy_Ability, Ground_Activated (probe droid; applied through ground_activated_trigger / special_apply): one
    // entry per target planet in the owner's list, key (unit id, 0, ability name crc), value = the spy mask, duration =
    // Duration_In_Secs (fixed-up). Targets [M]: the unit's planet, or every planet within its type's
    // Galactic_Influence_Range (r > 0, squared <=, planet row order).
    // system_spy_remove (0x70fb90) drops every stored entry keyed by the unit, every planet and player; returns the count.
    int32_t system_spy_remove(ObjectHandle unit);
    // Trade-route visibility (GRC 2.4 step 4 / port note 8): both endpoints' raw revealed bytes (a missing planet counts
    // as revealed). Presentation.
    bool trade_route_visible(uint32_t route) const;

    // ---- GRC 4: corruption transitions (src/reveal.cpp) ----------------------------------------------------------
    // Fraction 0x4b7e20 (presentation only: particles, path tint, UI percent; game logic must not branch on it).
    // now = 0 -> the current mode frame.
    float corruption_transition_fraction(PlanetIndex planet, uint32_t now = 0) const;
    // 0x4b7e90: the pending type while a transition runs, else the current type.
    int8_t effective_corruption_type(PlanetIndex planet) const;
    bool corruption_transition_active(PlanetIndex planet) const;  // D +0x2e2 || +0x2e3
    struct CorruptionParticlePairs {
        bool present = false;  // particle object D +0x300
        float brightness_start = 1.0f, brightness_end = 1.0f, scale_start = 1.0f, scale_end = 1.0f, energy_start = 1.0f,
              energy_end = 1.0f;
    };
    CorruptionParticlePairs corruption_particle_pairs(PlanetIndex planet) const;
    // Wrappers 0x3f6810 (gain) / 0x3f6770 (removal): begin (guards of 0x4b72e0 / 0x4b7260) and rebuild the pairs
    // (the "increase_corruption" / "decrease_corruption" script notification is not emitted [port]). False when the
    // guard refused. Completion runs in the planet service (frame >= end).
    // Durations (GRC 4.3): corruption action direct path / mission success 5 s, Corrupt_Planet_Ability its +0x17C,
    // Remove_Corruption_Ability Corruption_Removal_Time_In_Secs, Death Star hack 5 s, story REMOVE_CORRUPTION P2
    // (default 10 s).
    bool begin_corruption_gain(PlanetIndex planet, int8_t type, int32_t corrupting_player, float seconds);
    bool begin_corruption_removal(PlanetIndex planet, float seconds);
    void cancel_corruption_transition(PlanetIndex planet);  // 0x4b6c50
    void set_black_market_influence(PlanetIndex planet, bool on);
    void set_destroyed(PlanetIndex planet, bool destroyed);
    void set_credit_halt(PlanetIndex planet, int32_t until_frame);
    void set_siphon(PlanetIndex planet, int32_t player, float fraction, int32_t until_frame);
    void set_income_limits(PlanetIndex planet, bool has_min, float min_value, bool has_max, float max_value);
    void add_income_entry(PlanetIndex planet, float value, int32_t expiry);
    void set_hidden(PlanetIndex planet, bool hidden);
    // Set_Tech_Level 0x288980 (REMAINING_GAPS 1.4): level < 1 -> 0, then min(level, max tech). With `sweep`, every type
    // in type order that is affiliated with the player's faction, Tech_Level < the new level, Build_Initially_Locked,
    // Build_Can_Be_Unlocked_By_Slicer and not yet unlocked joins the unlocked list (TypeUnlocked events, value 1).
    // Story SET_TECH_LEVEL, Lua Set_Tech_Level, production of a Build_Advances_Tech_Level item and the campaign start
    // sweep; skirmish-style callers pass sweep = false.
    void set_tech_level(int32_t player, int32_t level, bool sweep = true);
    // 0x282190: tech + 1 with the sweep when below the max tech; false at max (slice, Give_Random_Sliceable_Tech,
    // story BUILDABLE_UNIT).
    bool raise_tech_level(int32_t player);

    // ---- queries ----------------------------------------------------------------------------------------
    const GalacticRules& rules() const { return *rules_; }
    const objects::ObjectStore& store() const { return *store_; }
    const PlanetGraph& graph() const { return graph_; }
    const GalaxyMap& map() const { return map_; }
    std::span<const PlayerState> players() const { return players_; }
    std::span<const PlanetState> planets() const { return planets_; }
    std::span<const FleetState> fleets() const { return fleets_; }
    float credits(int32_t player) const { return players_[static_cast<size_t>(player)].account.credits; }
    int32_t neutral_player() const { return neutral_player_; }
    int32_t player_of_faction(std::string_view faction) const;
    uint32_t fleet_of(ObjectHandle object) const;    // kNoFleet if not a live fleet object
    PlanetIndex planet_of(ObjectHandle object) const;  // planet row of a planet object
    // Live fleets of `player` orbiting `planet`, orbit order.
    std::vector<uint32_t> fleets_at(PlanetIndex planet, int32_t player) const;
    // The fleet of `owner` in orbit slot `slot` of `planet`, kNoFleet if none (orbit order is the search order).
    uint32_t fleet_in_slot(PlanetIndex planet, int32_t owner, int32_t slot) const;
    // True while no slot was ever chosen by a command: slots are then only auto-assigned and not hashed (see hash_slots).
    bool slot_state_default() const { return !slot_choices_; }
    int32_t ground_base_level(PlanetIndex planet) const;
    int32_t planets_owned(int32_t player) const;
    int32_t derive_owner(PlanetIndex planet) const;  // GE 4.1 precedence (pure)
    bool corrupted(PlanetIndex planet) const { return planets_[planet].corruption != kCorruptionNone; }
    float build_time_for(int32_t player, PlanetIndex planet, uint32_t type) const;
    int32_t build_cost_for(int32_t player, PlanetIndex planet, uint32_t type) const;
    bool pays_as_you_go(int32_t player) const;
    PlayerView player_view(int32_t player) const;

    // ---- A2 galactic: hooks for the galactic AI (src/ai_hooks.cpp) ---------------------------------------
    // Income tick listeners (IncomeTick above). Called from update() (serial) on the fiscal-cycle frame, inside the
    // payment loop; a listener may read the sim but must not change it. Not hashed, not saved: hosts re-register them.
    uint32_t add_income_hook(IncomeHook hook);  // returns an id for remove_income_hook (never 0)
    void remove_income_hook(uint32_t id);
    // The raw income I of `player` at the last fiscal cycle (IncomeTick::gain for AI players); 0 before the first one.
    float last_income(int32_t player) const;
    // Production queries. queue_kind: kGroundQueue / kSpaceQueue, or -1 when the type cannot be queued (GE 2.1).
    int32_t queue_kind(uint32_t type) const;
    // The planet's queue (head first); empty for an invalid planet / queue.
    std::span<const BuildEntry> queue_of(PlanetIndex planet, uint32_t queue) const;
    // What enqueue() would answer, without side effects (same checks, same order; None = it would be queued).
    RejectReason can_enqueue(int32_t player, PlanetIndex planet, uint32_t type) const;
    // Prerequisites only: can_enqueue minus the queue length, the credits and the population cap = build_check(...).ok
    // (OI galactic: the full 0x2804d0 order, STRATEGIC_GAPS 1.3) for a queueable type.
    bool buildable(PlanetIndex planet, int32_t player, uint32_t type) const;
    // Every buildable type at the planet for the player, type-index order.
    std::vector<uint32_t> buildable(PlanetIndex planet, int32_t player) const;
    // Galactic population (AI_GALACTIC_3 §3.5, A2GAL_READINGS §2 [H]). Applies to humans, and to AI players only with
    // AI_Uses_Galactic_Pop_Cap (never in shipped data). capacity = faction Base_Population_Capacity + Σ
    // Additional_Population_Capacity of the player's planets and live units / structures, + for each owned planet its
    // current starbase type's (0x3f8b40: once per planet, the starbase's owner ignored); used = (int)(Σ
    // Population_Value of the player's live units and structures + 0.5) (starbases are not objects in galactic mode),
    // plus the queued entries' when include_queued. A non-human skips objects whose planet is not revealed
    // (population_skips: Is_Planet_Revealed 0x3fa420; objects in transit are never skipped). enqueue() refuses
    // (kRejectPopulationCap) when pop(T, 0) > max(capacity - used incl. queued, 0), right after the queue-length check
    // (GE 2.x step 3, 0x2ac320).
    bool population_capped(int32_t player) const;
    int32_t population_capacity(int32_t player) const;
    int32_t population_used(int32_t player, bool include_queued = true) const;
    int32_t population_room(int32_t player) const;  // max(capacity - used incl. queued, 0)
    // The non-human population skip for an object located at `at` (kInvalidPlanet = in transit: never skipped).
    bool population_skips(int32_t player, PlanetIndex at) const;
    // Terrain (AI_GALACTIC_3 §6): the planet type's Terrain id (kTerrainNames, -1 none); whether the planet type
    // restricts `type` (Planet_Restricted_Unit_Categories overlap or Planet_Restricted_Unit_Types, 0x373490: the
    // TerrainType marshaller veto); AITerrainEffectiveness of `type` (its MovementClass) on the planet's terrain
    // (1.0 when either is missing).
    int32_t planet_terrain(PlanetIndex planet) const;
    bool planet_restricts(PlanetIndex planet, uint32_t type) const;
    double terrain_effectiveness(PlanetIndex planet, uint32_t type) const;
    // MagicPlan HIJACK_UNIT (AI_GALACTIC_3 §4.3), galactic part:
    // spawn_unit = step 4, the planet production "create object" (0x400b40) as a finished queue item: no cost, no queue,
    // no story event; space units join the player's stationary orbit fleet (found or made), ground units the surface,
    // structures / starbases the planet. Returns the new object; null = refused (invalid player / planet / type, a
    // type with no galactic role) = the task's FAILED. Also the Lua Spawn_Unit at a planet in a galactic script (a1g t06).
    ObjectHandle spawn_unit(int32_t player, PlanetIndex planet, uint32_t type);
    // Step 2 (MagicPlanStealing or a named hero): the first live object of exactly `type` owned by `player`, walking
    // the owner's object list newest first (A2GAL_READINGS 3 [H]: ObjectStore::owned_by in reverse, so a captured
    // object counts as the owner's newest), that is not at `destination` (units in a fleet in transit never are;
    // include_destination: a hero is taken even from there) is killed silently (no story event). Returns true when one
    // was removed.
    bool steal_unit(int32_t player, uint32_t type, PlanetIndex destination, bool include_destination);
    // Steps 2-4 in order: steal (when `steal`), cancel the pending delayed-spawn entries of the type on every planet
    // (any owner), spawn. Step 5 (launch op 4 + form op 0 for a unit created on a surface; FAILED when
    // blockaded) is the caller's, through FleetManagement events.
    ObjectHandle magic_spawn(int32_t player, PlanetIndex planet, uint32_t type, bool steal, bool hero);
    // A hostile combat fleet in orbit of the planet for `player` (the starbase-drop rule's test; HUD / story use).
    bool blockaded(PlanetIndex planet, int32_t player) const { return valid_planet(planet) && enemy_fleet_in_orbit(planet, player); }
    // The AI blockade test 0x3fa370 (A2GAL_READINGS 4 [H]; HIJACK_UNIT launch, the build task, FreeStore Move_Object,
    // TaskForce Launch_Units): the first orbit fleet that is alive and not hidden decides; blockaded unless its owner is
    // the planet's owner or an ally of it. No combat filter (neutrals and unarmed fleets blockade). Relative to the
    // planet owner, not to the asking player.
    bool ai_blockaded(PlanetIndex planet) const;
    // AI production market hook (AI_GALACTIC_3 §5): called for each planet inside the production service, in planet
    // service order (newest registered first), right before that planet's queues advance. The hook may enqueue or
    // cancel at that planet (the AIPlanetBuildQueue market). Not hashed. set_ fills the primary slot (null = none; the
    // galactic AI uses it); add_ registers further listeners, called after the primary one in registration order.
    void set_production_service_hook(std::function<void(PlanetIndex)> hook) { production_hook_ = std::move(hook); }
    uint32_t add_production_service_hook(std::function<void(PlanetIndex)> hook);  // id for remove_ (never 0)
    void remove_production_service_hook(uint32_t id);
    // q2f t01: an AI player's pay-as-you-go installment goes through its AI budget (0x53b900, AI_GALACTIC_2 §3.4). The
    // hook gets the planet, the queue index, the queue head and the installment a, and returns the credits to pay (>= 0;
    // the credits floor at 0 and the build advances) or a negative value (the build pauses). Without a hook every
    // player pays as a human: credits >= a, then a. Not hashed.
    using InstallmentHook = std::function<float(PlanetIndex planet, uint32_t queue, const BuildEntry& head, float a)>;
    void set_installment_hook(InstallmentHook hook) { installment_hook_ = std::move(hook); }

    // ---- story / script event stream (S4, galactic_events.hpp) --------------------------------------------
    // Off by default. Events are appended in emission order; the stream is not part of hash_state.
    void set_emit_events(bool on) { emit_events_ = on; }
    bool emit_events() const { return emit_events_; }
    const std::vector<GalacticEvent>& events() const { return events_; }
    // Moves the pending events to the end of `out` (stream order) and clears the stream.
    void drain_events(std::vector<GalacticEvent>& out);

    // ---- S10 battle transitions (conflicts.hpp) ----------------------------------------------------------
    void set_conflict_hooks(ConflictHooks* hooks) { conflicts_ = hooks; }
    ConflictHooks* conflict_hooks() const { return conflicts_; }
    // Hostility = relation(a, b) == Enemy (every galactic consumer: interception, blockade, conflicts, victory, AI).
    bool is_hostile(int32_t a, int32_t b) const { return hostile(a, b); }
    // ---- diplomacy (Lua Make_Ally / Make_Enemy / Is_Ally / Is_Enemy) ----
    // relation(): Neutral for an invalid player. set_relation() sets both directions (Make_Ally / Make_Enemy are
    // symmetric [inferred]); ignored for a == b, r == Self or an invalid player. The table is hashed (diplomacy tail)
    // once anything was set; reset_relations() returns to the defaults (and drops the tail).
    Relation relation(int32_t a, int32_t b) const;
    void set_relation(int32_t a, int32_t b, Relation r);
    void reset_relations() { relations_.clear(); }
    bool allied(int32_t a, int32_t b) const;  // relation 0 in the original: self or Ally ("held by me or an ally")
    bool diplomacy_state_default() const;      // relation table and unlocked lists untouched (tail skipped)
    // Planet in the galactic conflict list: impassable for the path finder (GALACTIC.md §3 check 4).
    void set_in_battle(PlanetIndex planet, bool on);
    // Battle write-back: replaces the planet's starbase by `type` (level = its Base_Level, same owner);
    // kNoIndex destroys it (level 0). False when there is no starbase.
    bool replace_starbase(PlanetIndex planet, uint32_t type);
    // The planet's current starbase type (0x3f8b40, planet data +0x140; A2GAL_READINGS 2.2), kNoIndex for none. refix t05
    // [port] decision: the original keeps the starbase as a TYPE on the planet (no object); the port keeps it as the
    // planet's starbase object, so the type is that object's type. starbase, starbase_level and starbase_owner are set
    // and cleared together (create_unit, replace_starbase, destroy_object, the story capture, load), and update()
    // clears level and owner of a planet whose starbase object was removed outside the sim (a direct ObjectStore
    // destroy, e.g. a galactic Lua Take_Damage), as destroy_object does, so no "level without a starbase" state lasts
    // past the next galactic frame.
    uint32_t starbase_type(PlanetIndex planet) const;
    // Clears a production queue without refunds (capture after a won battle, §3.2).
    void clear_queue(PlanetIndex planet, uint32_t queue);
    // Appends a host-made event (battle results) to the S4 stream (stamped with the mode frame).
    void post_event(const GalacticEvent& e) { emit(e); }
    // Land campaign-battle credit transfer (A1 tactical_credits.hpp): the player's account and its AI
    // Credit_Multiplier (1 for humans / no difficulty). Null for an invalid player.
    econ::CreditAccount* credit_account(int32_t player) {
        return valid_player(player) ? &players_[static_cast<size_t>(player)].account : nullptr;
    }
    float ai_credit_multiplier(int32_t player) const { return valid_player(player) ? credit_multiplier(player) : 1.0f; }

    // ---- L1/S4 Lua + story operations (src/story_ops.cpp; owner: L1/S4) -----------------------------------------
    // Change_Owner (Lua GameObjectWrapper.Change_Owner and story; GE 4.2 "Forced transfers" for planets). False
    // when nothing changed: dead object, invalid player, same owner, or a member of a moving fleet [port].
    bool change_owner(ObjectHandle object, int32_t player);
    // Assemble_Fleet(units) [inferred]: the units (members of stationary fleets orbiting one planet, or units landed
    // on that planet's surface, owned by the first valid unit's owner; others skipped) gathered into one fleet. A
    // fleet that holds exactly those units is returned as is; otherwise a new fleet takes them (list order; a landed
    // unit is launched as by launch_units: off the surface, launch reveal; a1g t06, so the shipped scripts'
    // SpawnList + Assemble_Fleet of ground units at a planet flies them, COMMANDS.md FleetManagement "AssembleFleet")
    // and emptied fleets die. Returns the fleet index, kNoFleet when no unit qualifies.
    uint32_t assemble_fleet(std::span<const ObjectHandle> units);
    // Containment as GalacticSim keeps it (OBJECT_RUNTIME.md 3; the store holds no container for ships):
    // container_of = the fleet of a fleet member, the planet of a surface unit / structure / starbase /
    // stationary fleet, else the store's container. contents_of = a fleet's members (content order); a
    // planet's orbiting fleets (orbit order), surface units, structures, starbase; else the live objects whose
    // store container it is (insertion order).
    ObjectHandle container_of(ObjectHandle object) const;
    std::vector<ObjectHandle> contents_of(ObjectHandle object) const;
    uint32_t fleet_containing(ObjectHandle unit) const;  // live fleet whose contents hold `unit`, kNoFleet if none

    // ---- P4 sim: FoC galactic special systems (src/special_systems.cpp; overview in special_systems.hpp) ---------------
    // Hero trigger (P4 6.1): every Ground_Activated ability of the unit's type, list order ("apply all"); accept and
    // black-market gate first; an apply that returns true with Causes_Despawn draws vf12 = Get_Float(Min_, Max_Respawn_
    // Time) (one synced draw) and the unit departs (respawn + despawn) unless the apply already removed it ([L] double
    // departure: the extra draw is made, the respawn is scheduled once). Not "ready"/mode tests beyond Initially_Enabled
    // [port]. Returns true when some ability applied; *accepted = some ability accepted (a human's dialog counts).
    bool ground_activated_trigger(ObjectHandle unit, PlanetIndex planet, bool* accepted = nullptr);
    // Respawn scheduling (P4 6.2, 0x48eb10 -> 0x3fe0a0): seconds <= 0 -> Default_Hero_Respawn_Time; a time < 0 then
    // queues NOTHING but still reports success [H quirk kept: Thrawn's Revenge's -360 default loses every
    // default-time respawn]; a named hero with a pending entry is not queued twice. planet invalid = the hero's
    // location. Due = mode frame + int(30 * time + 0.5). False only for a dead hero / no planet.
    bool schedule_hero_respawn(ObjectHandle hero, PlanetIndex planet, float seconds);
    // schedule_hero_respawn then despawn (0x48cf20: removed without a death event).
    bool hero_depart(ObjectHandle hero, PlanetIndex planet, float seconds);
    std::span<const RespawnEntry> respawns_pending(PlanetIndex planet) const;
    // Black market (P4 1).
    int32_t player_tech(int32_t player) const;
    std::vector<BlackMarketOffer> bm_offers(int32_t player, PlanetIndex planet, ObjectHandle hero = {}) const;
    std::span<const uint32_t> bm_purchased(int32_t player) const;
    bool bm_has_item(int32_t player, uint32_t item) const;
    // The Must_Be_Bought_On_Black_Market gate (P4 1.5, 0x39bbd0 / 0x39b0b0): allowed unless `must_be_bought` and some
    // item lists `ability_name_crc` (special_name_crc of the ability name, as written) and the player owns none of the
    // items listing it [inferred for several items]. An ability no item lists is allowed (the TR case).
    bool bm_allows_unit_ability(int32_t player, uint32_t ability_name_crc, bool must_be_bought) const;
    bool bm_allows_special_ability(int32_t player, uint32_t ability_name_crc, bool must_be_bought) const;
    // Executor 0x61ab60 (command kind 4 and the AI path): buyer alive (and the player's [port]); price and seconds are
    // re-quantized (q8 / q5); price > credits -> nothing (CannotAfford); else charge, add the item (idempotent), then
    // the hero departs (respawn at `planet` after `respawn_seconds`, 0 = default).
    RejectReason bm_execute_purchase(int32_t player, PlanetIndex planet, ObjectHandle hero, uint32_t item, float price,
                                     float respawn_seconds);
    bool bm_give_item(int32_t player, std::string_view item_name);  // story reward GIVE_BLACK_MARKET 0x7E: free
    void set_bm_tutorial(int32_t player, bool on);
    void set_sabotage_tutorial(int32_t player, bool on);
    // Sabotage (P4 2). Options of `saboteur`'s first sabotage ability at the planet (SabotageOption values).
    std::vector<uint8_t> sabotage_options(PlanetIndex planet, ObjectHandle saboteur) const;
    // round_half_away(Sabotage_Cost_Credits * factor^n), n = the planet's sabotage counter; -1 when the base cost <= 0;
    // tutorial: Tutorial_Sabotage_Cost_Credits unscaled. factor^n by repeated float multiplication [port: powf is a CRT
    // transcendental; exact for the vanilla 2.0].
    int32_t sabotage_cost(int32_t player, PlanetIndex planet, ObjectHandle structure, ObjectHandle saboteur = {}) const;
    std::vector<SabotageTarget> sabotage_targets(int32_t player, PlanetIndex planet, ObjectHandle saboteur = {}) const;
    // Executor 0x5b21a0 (command kind 5).
    RejectReason sabotage_execute_structure(int32_t player, PlanetIndex planet, ObjectHandle saboteur, ObjectHandle structure,
                                            float cost, float respawn_seconds);
    bool sabotage_structure_reward(PlanetIndex planet, uint32_t type);  // story SABOTAGE_STRUCTURE 0x81
    int32_t sabotage_count(PlanetIndex planet) const { return valid_planet(planet) ? planets_[planet].sabotage_count : 0; }
    // Super-weapon hack (P4 3).
    ObjectHandle hack_find_super_weapon(PlanetIndex planet) const;
    float hack_cost() const { return rules_->special.hack_cost; }
    RejectReason hack_execute(int32_t player, PlanetIndex planet, ObjectHandle hacker, float cost);  // executor 0x61b9f0
    // Rebel tech theft (REMAINING_GAPS 1.1-1.3): the dialog rows of a slicer at the planet, candidate order (the slicer
    // owner's slice_candidates at its current tech; empty when there are none).
    std::vector<SliceEntry> slice_entries(int32_t player, PlanetIndex planet, uint32_t ability) const;
    // Execute 0x619160 (command kind 2 and the AI pick): vigilance, the success draw s, the failure-kind draw k, the
    // unlock (+ tech raise when only_one), respawn scheduling and the despawn. True on success.
    bool slice_execute(int32_t player, PlanetIndex planet, ObjectHandle slicer, const SliceEntry& e, float respawn_seconds,
                       bool only_one);
    // Planet destruction, the IG-88 hack path (0x61b790; TSW_PLANET_DESTROY §5): planet_destroy_core() plus the hack's
    // extras - orbiting objects removed (reverse; this includes the hacked weapon; heroes get a default-time respawn),
    // starbase cleared, owner := the neutral player. False for an invalid or already destroyed planet. [port] the orbit
    // removal runs inside the core, before the structures (the original runs it after the core): the destroy / respawn
    // order of the earlier single routine, so hack CRCs do not move.
    bool planet_destroy(PlanetIndex planet, int32_t destroyer);
    // The shared routine 0x3f76c0 (TSW_PLANET_DESTROY §5; hack, galactic Death Star and tactical super weapon):
    // PLANET_DESTROYED event (value = `event_value`: 1 marks a tactical TSW kill, whose story event goes to the battle's
    // plots only), siphon cleared, structures removed, corruption cleared, destroyed set, surface objects removed
    // (reverse; heroes respawn). Orbit fleets, starbase and owner are NOT touched. False when invalid / already destroyed.
    bool planet_destroy_core(PlanetIndex planet, int32_t destroyer, int32_t event_value = 0);
    // Victory 7 leader test (§6; 0x42a7b0 / 0x707670): the victim = the planet's owner (D +0x6c); its faction's
    // Faction_Leader must be a team member (Company_Units) of an object on the planet's surface (D +0xe0). Only members
    // are compared, not a company object itself; orbit fleets do not count. [port] (refix t06) a bare ground unit (a
    // type without Company_Units) is its own one-unit transport, so a surface unit of the leader type itself counts.
    // [M] members one level deep only; exact type match (variants such as Tyber_Zann_Passenger do not count).
    // [port] the condition record's optional leader-name list is not modelled (the claim is granted by the side's
    // condition list, victory.hpp).
    bool super_weapon_leader_hit(PlanetIndex planet) const;
    // Tactical super weapon, galactic part of state 9 (TSW_PLANET_DESTROY §4 steps 7-9), called by the battle host in the
    // tactical frame of the shot: leader test -> claim 7 (countdown 30) for `tsw_owner`, then planet_destroy_core(value
    // 1). Returns true when the planet was destroyed; *leader_hit = the test hit (the battle then ends with victory 0x10).
    bool tsw_destroy_planet(PlanetIndex planet, int32_t tsw_owner, bool* leader_hit = nullptr);
    // Locked ("obsolete") list P +0x1C8 (hack step 1, story LOCK_UNIT / BUILDABLE_UNIT P2, Lua Lock_Tech): add 0x27f7c0 /
    // remove 0x286100. Removing does not unlock: Lua Unlock_Tech is unlock_tech().
    void lock_type(int32_t player, uint32_t type, bool locked);
    bool type_locked(int32_t player, uint32_t type) const;       // in the locked list
    // Unlocked list P +0x1B0 (add 0x27f860: no duplicates, unlock order). `source` = the TypeUnlocked event value.
    void unlock_type(int32_t player, uint32_t type, int32_t source = 3);
    bool type_unlocked(int32_t player, uint32_t type) const;
    void remove_unlock(int32_t player, uint32_t type);           // 0x286150 (corruption replaced)
    // Lua Unlock_Tech 0x604540 / story BUILDABLE_UNIT P1: leaves the locked list and joins the unlocked list, no tech
    // check (REMAINING_GAPS 1.5 items 2-3).
    void unlock_tech(int32_t player, uint32_t type);
    // Story BUILDABLE_UNIT (reward 1, 0x45e8b0): P1 unlock_tech, P2 (kNoIndex = none) locked; then, only for the
    // GameConstants Good_Side_Name faction, raise the tech when no sliceable candidate remains at the current tech.
    void buildable_unit_reward(int32_t player, uint32_t unlock, uint32_t obsolete);
    // Affiliation test of the sweep / slice candidates (0x3751a0; hand-built rules: an empty list = any faction).
    bool type_affiliated(uint32_t type, int32_t player) const;
    // Slice candidates (0x618cb0, type order): affiliated, Tech_Level <= tech, Build_Initially_Locked, not unlocked, not
    // locked, Build_Can_Be_Unlocked_By_Slicer.
    std::vector<uint32_t> slice_candidates(int32_t player) const;
    // Production-disabled list P +0x1E0 (story ENABLE_BUILDABLE 0x70 / DISABLE_BUILDABLE 0x6F). Read by the GUI build
    // bar and AI production only, not by buildability (REMAINING_GAPS 1.5).
    void set_production_disabled(int32_t player, uint32_t type, bool disabled);
    bool production_disabled(int32_t player, uint32_t type) const;
    // Build-locked = in the locked list, or (initial build locks on) Build_Initially_Locked and not unlocked
    // (MagicPlan +0xC1 / 0x282580). prerequisites_met / enqueue / buildable refuse build-locked types.
    bool build_locked(int32_t player, uint32_t type) const;
    // Build_Initially_Locked enforcement (buildability step 9), on by default (REMAINING_GAPS 1.5: every unlock source
    // is wired - slice, tech sweep, story BUILDABLE_UNIT, Lua Unlock_Tech / Give_Random_Sliceable_Tech, corruption).
    void set_initial_build_locks(bool on) { initial_build_locks_ = on; }
    bool initial_build_locks() const { return initial_build_locks_; }
    // Income abilities (P4 4) and the economy panel (P4 4.3).
    std::vector<EconomyRow> economy_breakdown_by_tag(int32_t player) const;
    float economy_income_for_tag(int32_t player, uint32_t tag) const;
    float mining_income(int32_t player) const { return economy_income_for_tag(player, economy_mining_tag()); }
    // Corruption (P4 5).
    std::span<const CorruptionChoiceDef> corruption_choices(PlanetIndex planet) const;
    const CorruptionChoiceDef* corruption_choice(PlanetIndex planet, int8_t type) const;  // first of that type
    bool corruption_accepts(PlanetIndex planet, ObjectHandle defiler) const;  // Accept vf5 0x52be20
    CorruptionRequirements corruption_requirements(int32_t player, PlanetIndex planet, const CorruptionChoiceDef& choice) const;
    // AI pick (0x52b160): candidates passing all four checks, weights of P4 5.4, one Get_Float(0, total) draw when
    // total > 0; returns an index into corruption_choices(planet), -1 = none.
    int32_t corruption_ai_pick(int32_t player, PlanetIndex planet);
    // Executor checks in the original order (P4 5.2): credits (cost <= 0 or <= credits, else NoCredits), the charge,
    // then the Required_Unit check (UnitsUnmet: no refund [H as read]), then the consumed prerequisites (P4 5.3).
    // The choice comes from the planet type; without one (hand-built rules) `fallback_cost` is used and there is no
    // unit check.
    CorruptionCharge corruption_charge(int32_t player, PlanetIndex planet, int8_t type, float fallback_cost);
    void count_corruption(int32_t player, int8_t type);  // completed counter +1 (0x2820a0; [M] also a won mission)
    int32_t corruptions_completed(int32_t player, int8_t type) const;
    bool special_state_default() const;  // nothing of the P4 state set (the CRC tail is skipped)

    // ---- OI galactic (docs/re/STRATEGIC_GAPS.md = SG; src/strategic.cpp) -------------------------------------------
    // Fleet speed multiplier M (SG 2.1) = D x mode scale x 1.0, read by every departure (the mode rewrites it every
    // tick; D = debug game speed, 1.0). [port] the mode scale is 1.0, not Object_Max_Speed_Multiplier_Galactic (the
    // 2026-10-28 playtest: 5.0 made fleets ~5x faster than retail; see strategic.cpp).
    float fleet_speed_multiplier() const;
    // Company transports (SG 1.7). Port model: a company (DUMMY_GROUND_COMPANY with a Company_Transport_Unit) is one
    // galactic object whose store type stays the COMPANY type (the "original type" of build counts, AI learning, story
    // and battles); the sim treats it as a TRANSPORT of type Company_Transport_Unit[0] carrying the Company_Units:
    // travel_type() is what hyperspace speed and capability read (SG 1.7 "Speed": transports count with their own type).
    // Placement at creation: surface when accessible, surface_forces < Max_Ground_Forces_On_Planet and no member
    // Always_Spawn_In_Orbit while (stealthed or no hostile visible object in orbit) [M]; else a NEW Galactic_Fleet in
    // orbit. A company type without a transport keeps the former surface placement [port: the original creates
    // nothing]. Launch / land / form: FleetManagement ops (FleetOp).
    uint32_t travel_type(ObjectHandle unit) const;          // transport type of a company, else the store type
    bool is_company_transport(ObjectHandle unit) const;     // a company with a Company_Transport_Unit
    // The representative type 0x3973b0 (L1P4_LUA_GAPS 1.2, A2GAL_READINGS 3.2): a fleet's squadron type (FLEET
    // behaviour), a transport's company type (TRANSPORT behaviour), a team's type, else the object's own type. Port
    // model: a squadron and a company are single objects whose store type IS that squadron / company type, and a fleet
    // object's own type stands for Galactic_Fleet, so this is the store type for every galactic object (kNoIndex for a
    // stale handle). The galactic Activate_Ability fleet filter and steal_unit match on it.
    uint32_t representative_type(ObjectHandle object) const;
    int32_t surface_forces(PlanetIndex planet) const;       // 0x3f9530 [M]: live surface units, any owner
    // Buildability 0x2804d0 in the SG 1.3 order. check_credits: step 6 for players with Is_Human (+0x62 [M]).
    // Build_Initially_Locked (step 9) only while initial_build_locks() is on.
    BuildCheck build_check(int32_t player, PlanetIndex planet, uint32_t type, bool check_credits = false) const;
    int32_t built_count(int32_t player, uint32_t type) const;  // step 22 count (0x281c70: + containers listing `type`)
    int32_t live_count(int32_t player, uint32_t type) const;   // step 23 live count (0x281df0)
    int32_t max_tech_level(int32_t player) const;
    void set_max_tech_level(int32_t player, int32_t level);    // story SET_MAX_TECH_LEVEL
    // Galactic stealth (SG 2.2). The byte is set at creation for types with an Initially_Enabled Galactic_Stealth_Ability
    // (Galactic_Automatic) and by set_galactic_stealth (ability activation / deactivation). hidden() = predicate 0x3a9760:
    // removed (dead), a fleet whose every member is hidden, else the byte (a company transport: its type's flag = all
    // cargo stealthed).
    void set_galactic_stealth(ObjectHandle object, bool on);
    bool galactic_stealth(ObjectHandle object) const;  // the byte
    bool hidden(ObjectHandle object) const;
    // DetectHeroAbility service (SG 2.2) for one detector: candidates = stealthed objects in the detector's planet orbit,
    // then its surface (list orders); a non-allied candidate is discovered when not (detector generic and candidate
    // named) and ((detector named and candidate generic) or one synced Get_Float(0,1) draw < chance). Discovery clears
    // the byte [M: the discovery mark 0x3a0110]. Returns the number discovered. No FoC type has a detector ability.
    int32_t detect_stealthed(ObjectHandle detector, float chance);
    // Planet-info display queries (REMAINING_GAPS 6.3 as corrected 2026-10-28; GUI and planet info only, no sim effect,
    // nothing written or hashed). "player can see X at planet":
    //   1. corruption bypass (queries marked B): the planet is corrupted and the player's faction
    //      Benefits_From_Corruption -> true;
    //   2. stealth gate: with Planet_Starbase_Stealth (S) and a starbase owner (D +0x13c), or Planet_Groundbase_Stealth (G)
    //      and a ground-base owner (D +0x124 [port: the owner of the first live ground structure]), the player must be
    //      allied with that owner (0x2823c0 [M]), else false [port reading: passing continues to step 3];
    //   3. the bit test on visibility_mask(planet, player) (0x50c550 = the fold; true when any bit of the query is set).
    // Query (address): bypass, gate, bits:
    //   StarbaseLevel (0x50c420) B S 0x2, else fallback: allied with the starbase owner, or the faction's
    //     info_base_level_threshold (+0x20ac [M]) <= the starbase level (D +0x148);
    //   GroundBaseLevel (0x50b840) B G 0x4, else the same fallback with the ground-base owner / ground_base_level (D +0x130);
    //   FleetContents (0x50b3f0) B S 0x40; FleetPresence (0x50b4c0) B S 0x10 | 0x20 (0x10, or the NumFleets query);
    //   NumFleets (0x50b5a0) B S 0x20 (rest not traced); GroundCompanyContents (0x50b970) B G 0x100;
    //   NumGroundCompanies (0x50ba40) B G 0x80 | 0x100; MostPowerfulShip (0x50bb10) S 0x10000;
    //   SpecialStructures (0x50c300) B S+G 0x200; PoliticalControl (0x50c270) B 0x400 | 0x800;
    //   CreditIncome (0x50b2a0) 0x8 | 0x8000; CreditIncomeBreakdown (0x50b2d0) 0x8000; MinorStealthHeroes (0x50b370) B 0x2000;
    //   MajorStealthHeroes (0x50b2f0) B 0x4000; ForceSensitiveHeroes (0x50b820) 0x20000.
    // False for an invalid player or planet. The composite ownership check 0x50bf10 and the per-object 0x50bba0 are not
    // exposed ([M] readings).
    enum class PlanetInfo : uint8_t {
        StarbaseLevel, GroundBaseLevel, FleetContents, FleetPresence, NumFleets, GroundCompanyContents, NumGroundCompanies,
        MostPowerfulShip, SpecialStructures, PoliticalControl, CreditIncome, CreditIncomeBreakdown, MinorStealthHeroes,
        MajorStealthHeroes, ForceSensitiveHeroes
    };
    bool can_see_planet_info(int32_t player, PlanetIndex planet, PlanetInfo what) const;
    bool can_see_starbase_info(int32_t player, PlanetIndex planet) const;    // PlanetInfo::StarbaseLevel (0x50c420)
    bool can_see_groundbase_info(int32_t player, PlanetIndex planet) const;  // PlanetInfo::GroundBaseLevel (0x50b840)
    static constexpr uint32_t kPlanetVisStarbase = kVisStarbaseLevel, kPlanetVisGroundbase = kVisGroundBaseLevel;
    // Victory events (SG 1.4). The super weapon of a battle at `planet` = the first live object in its orbit (fleet
    // order, member order) whose type has a Planet_Destruction_Ability [port: the conflict record's +0x28 id].
    ObjectHandle super_weapon_at(PlanetIndex planet) const;
    // Battle resolution (mode common tick 0x3639d0): weapon alive and not the winner's, and the winner owns any
    // Is_Super_Weapon_Killer object -> the weapon is killed and claim 3 (countdown 5); else when the weapon's owner's
    // playable enemies own no undestroyed planet -> claim 6 for the owner (countdown 30). Returns true when killed.
    bool resolve_battle_super_weapon(PlanetIndex planet, int32_t winner);
    bool owns_super_weapon_killer(int32_t player) const;  // any live Is_Super_Weapon_Killer object
    // TACTICAL_GAPS 4.8 battle-end step 4: the weapon is alive, not the winner's, and the winner has no killer -> the
    // winner is the side that retreats (R := winner).
    bool super_weapon_forces_retreat(PlanetIndex planet, int32_t winner) const;
    // Planet_Destruction_Ability (0x707670): claim 7 (countdown 30) when super_weapon_leader_hit(), then
    // planet_destroy_core() - the Death Star's own fleet, the starbase and the owner stay (TSW_PLANET_DESTROY §5).
    // False when nothing was destroyed.
    bool super_weapon_destroy_planet(PlanetIndex planet, int32_t attacker);
    std::span<const VictoryClaim> victory_claims() const { return claims_; }
    void claim_victory(int32_t player, uint8_t type, int32_t countdown);

    // ---- CRC ----------------------------------------------------------------------------------------------
    // Ordered digest of every S3 data pack (players, planets incl. queues and income lists, fleets, cycle).
    void hash_state(StateHasher& h) const;
    // N1 save / load (contract: alamo/core/save_file.hpp; layout in src/galactic_save.cpp), chunk "galactic.state".
    // Every galactic data pack (players, planets incl. queues / income / visibility / respawn lists, fleets incl. paths
    // and slots, relations, OI / GRC state, pending S4 events, mode counters). read() needs the session built from the
    // same setup (start()) and the store restored first (objects are saved by ObjectId); false and nothing changed on
    // a version / fingerprint mismatch, an invalid value, an unknown object id or truncated data. Hooks are the host's.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
    // Registers "<prefix>.state" (hash_state) and, when include_store, the store as "<prefix>.objects"
    // (+ "<prefix>.objects.parity", the retail object-CRC sum). The SimRng is the host's to register.
    void register_crc(CrcRegistry& registry, const std::string& prefix = "galactic", bool include_store = true) const;
    // Launch reveal 0x4f40c0 (REMAINING_GAPS 6.1), on lift-off from `planet`: R = max Galactic_Influence_Range of the
    // unit's type and its Company_Units [port: a company is one galactic object]; R <= 0 -> the launch planet's influence
    // set only, else every planet within R (squared 3-D distance <=, planet row order).
    void launch_reveal(PlanetIndex planet, ObjectHandle unit);
    // Surface test 0x375220 (STRATEGIC_GAPS 1.3 step 24) [H]: the type's Planet_Surface_Accessible, false once the planet
    // is destroyed (planet data +0x2C8). Every retail caller goes through it (land, invade, auto-land, AI token, build).
    bool surface_accessible(PlanetIndex planet) const {
        if (planet >= planets_.size()) return false;
        const PlanetState& p = planets_[planet];
        return !p.destroyed && (p.type >= rules_->types.size() || rules_->types[p.type].surface_accessible);
    }

private:
    struct Derived {
        int32_t owner = kNoPlayer;
    };
    const FactionRules& faction_of(int32_t player) const;
    bool benefits(int32_t player) const;
    bool hostile(int32_t a, int32_t b) const;
    bool valid_player(int32_t p) const { return p >= 0 && static_cast<size_t>(p) < players_.size(); }
    bool valid_planet(PlanetIndex p) const { return p < planets_.size() && planets_[p].is_planet; }
    float credit_multiplier(int32_t player) const;
    int32_t planet_base(const PlanetState& p) const;
    void set_income_entry(PlanetState& p, uint8_t kind, float value);
    void remove_income_entry(PlanetState& p, uint8_t kind);

    // ownership
    void apply_ownership(PlanetIndex planet, int32_t derived);
    void on_owner_change(PlanetIndex planet, int32_t old_owner);
    // production
    int32_t queue_kind_of(const TypeInfo& t) const;  // -1 = not producible
    bool prerequisites_met(int32_t player, PlanetIndex planet, const TypeInfo& t, bool at_enqueue) const;
    // The planet type's corruption choice of `type`: its Success_Unlock_Unit_Type, kNoIndex when none.
    uint32_t corruption_unlock(PlanetIndex planet, int8_t type) const;
    // REMAINING_GAPS 1.5 item 5 (0x3f9690), after the planet's corruption changed from (old_type, old_player): the
    // replaced type's Success_Unlock_Unit_Type leaves the old player's unlocked list (even when another planet still
    // grants it), the current type's joins the corrupting player's when its planet build cost (0x3711c0) > 0.
    void corruption_unlocks(PlanetIndex planet, int8_t old_type, int32_t old_player);
    // enqueue()'s checks; *cost = the build cost when None (A2 galactic: shared with can_enqueue).
    RejectReason check_enqueue(int32_t player, PlanetIndex planet, uint32_t type, int32_t* cost) const;
    void restart_head(BuildEntry& e) const;
    void pop_head(PlanetIndex planet, uint32_t queue);
    void service_production(PlanetIndex planet);
    void planet_production(PlanetIndex planet);  // production hook + service_production
    void complete_build(PlanetIndex planet, const BuildEntry& e);
    bool enemy_fleet_in_orbit(PlanetIndex planet, int32_t player) const;
    bool contested_surface(PlanetIndex planet) const;
    // objects
    ObjectHandle spawn(int32_t player, PlanetIndex planet, uint32_t type, ObjectHandle container);
    bool create_unit(int32_t player, PlanetIndex planet, uint32_t type);
    uint32_t create_fleet(int32_t player, PlanetIndex planet);
    uint32_t orbit_fleet(int32_t player, PlanetIndex planet);  // find or create
    void kill_fleet(uint32_t fleet);
    void remove_from_orbit(PlanetIndex planet, uint32_t fleet);
    // fleets
    RejectReason begin_move(uint32_t fleet, std::vector<PlanetIndex> path);
    void depart_hop(uint32_t fleet, PlanetIndex from, PlanetIndex to, bool leave_orbit);
    void service_fleet(uint32_t fleet);
    void arrive(uint32_t fleet);
    bool orbit_conflict(PlanetIndex planet, int32_t mover) const;
    void merge_fleet(uint32_t from, uint32_t into);
    // Orbit slots (src/fleet_slots.cpp)
    void settle_slots(PlanetIndex planet);                       // unslotted idle fleets take the owner's lowest free slot
    int32_t lowest_free_slot(PlanetIndex planet, int32_t owner, uint32_t except) const;  // -1: all kOrbitSlots taken
    bool place_in_slot(uint32_t fleet, int32_t slot);            // false: slot invalid or taken by another fleet
    uint32_t slot_occupant(PlanetIndex planet, int32_t owner, int32_t slot, uint32_t except) const;
    void hash_slots(StateHasher& h) const;
    RejectReason fleet_management(int32_t player, const commands::EventData& ev);  // FleetOp (A2 galactic, ai_hooks.cpp)
    uint32_t route_between(PlanetIndex a, PlanetIndex b) const;  // kNoRoute if none
    // cycle
    void fiscal_cycle();
    // events
    void emit(GalacticEvent e) {
        if (!emit_events_) return;
        e.frame = frame_;
        events_.push_back(e);
    }
    void emit_production(GalacticEventKind kind, PlanetIndex planet, uint32_t queue, const BuildEntry& e, int32_t extra);
    PlanetIndex location_of(ObjectHandle object) const;  // planet of a unit / structure / starbase / fleet
    // P4 sim (src/special_systems.cpp)
    const SpecialRules& special() const { return rules_->special; }
    const TypeSpecial& special_type(ObjectHandle h) const;
    bool is_hero_type(uint32_t type) const;
    bool faction_pairing(int32_t player, int32_t owner) const;  // 0x48d900
    bool special_accepts(const SpecialAbilityDef& a, ObjectHandle unit, int32_t owner, PlanetIndex planet) const;
    bool special_apply(uint32_t ability, ObjectHandle unit, int32_t owner, PlanetIndex planet);
    bool apply_black_market(const SpecialAbilityDef& a, ObjectHandle hero, int32_t player, PlanetIndex planet);
    bool apply_slicer(const SpecialAbilityDef& a, ObjectHandle slicer, int32_t player, PlanetIndex planet);
    bool slice_accepts(int32_t player, PlanetIndex planet) const;
    bool apply_sabotage(const SpecialAbilityDef& a, ObjectHandle saboteur, int32_t player, PlanetIndex planet);
    bool apply_hack(ObjectHandle hacker, int32_t player, PlanetIndex planet);
    bool apply_corruption(ObjectHandle defiler, int32_t player, PlanetIndex planet);
    std::vector<uint8_t> sabotage_options_of(const SpecialAbilityDef& a, PlanetIndex planet, int32_t player) const;
    int32_t sabotage_cost_with(const SpecialAbilityDef* a, int32_t player, PlanetIndex planet, ObjectHandle structure) const;
    std::vector<BlackMarketOffer> offers_with(const SpecialAbilityDef* a, int32_t player, PlanetIndex planet) const;
    bool bm_purchase(int32_t player, PlanetIndex planet, ObjectHandle hero, uint32_t item, float price, float seconds);
    void hack_effect(int32_t player, PlanetIndex planet, ObjectHandle hacker, ObjectHandle weapon);
    void destroy_structure_sabotage(PlanetIndex planet, ObjectHandle structure, int32_t player, bool story);
    void consume_prerequisites(int32_t player, PlanetIndex planet, const CorruptionChoiceDef& c);
    int32_t count_required_units(int32_t player, const std::vector<uint32_t>& types) const;
    void despawn(ObjectHandle h);
    void service_respawns();
    void service_income_abilities(PlanetIndex planet);
    RejectReason special_command(int32_t player, const commands::EventData& ev);
    bool planet_destroy_impl(PlanetIndex planet, int32_t destroyer, int32_t event_value, bool hack_orbit);  // L1 TSW split
    void emit_special(GalacticEventKind kind, int32_t player, PlanetIndex planet, ObjectHandle object, uint32_t type,
                      int32_t value, int32_t other);
    void hash_special(StateHasher& h) const;
    Relation default_relation(int32_t a, int32_t b) const;
    void hash_diplomacy(StateHasher& h) const;
    // OI galactic (src/strategic.cpp)
    bool place_company(int32_t player, PlanetIndex planet, uint32_t type);
    bool hostile_visible_in_orbit(PlanetIndex planet, int32_t player) const;
    void count_build(int32_t player, uint32_t type);
    int32_t queued_count(int32_t player, uint32_t type) const;
    bool oi_state_default() const;
    void hash_oi(StateHasher& h) const;
    void oi_on_spawn(ObjectHandle h);

    const GalacticRules* rules_;
    objects::ObjectStore* store_;
    SimRng* rng_;
    ParallelFor pf_ = serial_for;

    GalaxyMap map_;
    PlanetGraph graph_;
    std::vector<PlayerState> players_;
    std::vector<uint8_t> relations_;  // P x P Relation, [a * P + b]; empty = defaults (default_relation)
    bool initial_build_locks_ = true;   // config, not hashed (REMAINING_GAPS 1.6: on, every unlock source is wired)
    std::vector<PlanetState> planets_;
    std::vector<FleetState> fleets_;
    std::vector<uint32_t> fleet_by_slot_;   // store slot -> fleet index (kNoFleet)
    bool slot_choices_ = false;  // a command chose a slot or a destination slot: hashed in the "FSL1" tail from then on
    std::vector<PlanetIndex> planet_by_slot_;  // store slot -> planet row
    int32_t neutral_player_ = kNoPlayer;
    int32_t frame_ = 0;
    int32_t pause_ = 0;
    bool linked_battle_pending_ = false;  // REMAINING_GAPS 6.4; hashed in the OI tail
    // Fiscal-cycle fields. The mode's frame-0 cycle runs before the galaxy is built, so it pays nobody and draws no
    // black-market value; the first payment is on frame 1350 (Q2 traces: retail F fields 0 / 1350 / 1 at mode frame 0,
    // first credit change at mode frame 1351) [H]. start() leaves them as that empty cycle does (q2f t02).
    int32_t last_cycle_frame_ = 0;   // +0x368
    int32_t next_cycle_frame_ = 0;   // +0x36c: one fiscal period after start()
    int32_t cycle_counter_ = 0;      // +0x370: 1 after start()
    PathScratch scratch_;
    std::vector<Derived> derived_;   // per planet row (parallel derivation output)
    bool emit_events_ = false;
    ObjectHandle last_spawn_;  // newest object made by spawn() (event payloads only, never hashed)
    std::vector<GalacticEvent> events_;  // S4 stream (not hashed)
    ConflictHooks* conflicts_ = nullptr;  // S10 (not owned)
    // A2 galactic (not hashed: listeners and the last cycle's gains, both re-derived by hosts / the next cycle)
    std::vector<std::pair<uint32_t, IncomeHook>> income_hooks_;
    uint32_t next_hook_id_ = 1;
    std::vector<float> last_income_;  // per player
    std::function<void(PlanetIndex)> production_hook_;
    InstallmentHook installment_hook_;
    std::vector<std::pair<uint32_t, std::function<void(PlanetIndex)>>> production_hooks_;
    // OI galactic (hashed in the "OIG1" tail once non-default)
    std::vector<int32_t> stealthed_;      // ObjectIds with the galactic stealth byte set, ascending
    std::vector<VictoryClaim> claims_;    // declaration order
    // GRC reveal / corruption transitions (src/reveal.cpp; hashed in the "RVC1" tail once non-default)
    int32_t local_player_ = kNoPlayer;
    bool auto_reveal_ = true;   // galactic mode +0x340
    bool reveal_sfx_ = false;   // presentation latch (not hashed)
    void reveal_prestep();      // 0x4b2600
    void sync_starbases();      // refix t05: a starbase object removed outside the sim -> level 0 (starbase_type())
    bool reveal_one(PlanetIndex planet);  // pre-step step 4: true when the byte was clear
    bool begin_gain_raw(PlanetIndex planet, float seconds, int8_t type, int32_t player);  // 0x4b72e0
    bool begin_removal_raw(PlanetIndex planet, float seconds);                            // 0x4b7260
    void complete_corruption(PlanetIndex planet);                                         // 0x3fc5b0 (frame >= end)
    void rebuild_corruption_pairs(PlanetIndex planet);                                    // 0x3ff230
    bool reveal_state_default() const;
    void hash_reveal(StateHasher& h) const;
    int32_t groundbase_owner(PlanetIndex planet) const;  // D +0x124 [port: first live ground structure owner]
    // GRC 2 (src/visibility.cpp)
    uint32_t visibility_baseline(PlanetIndex planet, int32_t player) const;
    uint32_t visibility_presence(PlanetIndex planet, int32_t player) const;
    uint32_t visibility_auto_spy(PlanetIndex planet, int32_t player) const;
    uint32_t visibility_expiry_frame(float seconds) const;
    bool apply_system_spy(const SpecialAbilityDef& a, ObjectHandle unit, int32_t owner, PlanetIndex planet);
    void expire_visibility(PlanetIndex planet);  // planet service 0x3fe1b0 -> 0x285e40
    bool visibility_state_default() const;
    void hash_visibility(StateHasher& h) const;
};

}  // namespace alamo::galactic

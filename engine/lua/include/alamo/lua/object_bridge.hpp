// L1 stage 2, binding family OBJECTS (src/bind_objects.cpp): what the object bindings need from the
// sim beyond LuaHost (store / types / rng).
//
// ObjectBridge is owned by the host (one per game mode, shared by every script VM of that mode) and
// attached to each VM with attach_object_bridge(). Without a bridge the object bindings still run
// on LuaHost::store: queries work, orders are not issued (their BlockingStatus is finished at once),
// script-only flags are not kept (setters are no-ops, getters return the defaults below).
//
// Per-object script state (ScriptObjectState) holds what scripts set or read that the ObjectStore does
// not model yet (hint, Hide, Set_Selectable, Make_Invulnerable, ability modes, ...). It is keyed by
// ObjectId in an ordered map: lookups only, plus prune() which walks ids ascending (deterministic).
// When the owning systems exist (selection, combat, abilities) these fields move there.
//
// Orders (Move_To, Attack_Target, Guard_Target, Stop, Activate_Ability, ...) are built as EventData and
// run through CommandPipeline::execute_direct (docs/re/COMMANDS.md §5: AI/Lua orders bypass the
// queue) at tick = the VM frame (LuaVm::set_frame), issuer = the ordered object's owner (0 if none),
// source CommandSource::Script. Orders that have a per-object event in the original catalogue use it
// (MoveToObject, Attack, Facing, Garrison, SetUnitAbilityMode, SetAbilityAutofire, TacticalSell); the
// others, whose original Lua path calls the unit directly rather than through an event, use the
// port-only PortExtension event with subtype kLuaOrderSubtype + LuaOrder and the payload below.
//
// Stage 3 additions:
//   * proximity events (Event_Object_In_Range / Cancel_Event_Object_In_Range / Service_Wrapper): the
//     registrations live in the VM (Lua functions); candidates come from objects_in_range (the host's
//     spatial tree) or a brute-force walk of the VM's store; see bind_objects.cpp;
//   * reinforcements (Reinforce_Unit, Spawn_From_Reinforcement_Pool): host hooks below, with direct
//     spawn stand-ins until a tactical reinforcement system exists;
//   * hash(): ordered digest of the script state (for the host's CRC system, e.g. S4 "story").
//
// L1/S4 wave 2a additions (tactical hosts): `abilities` (S9 AbilitySystem) and `anim` (R3 AnimWorld). With
// an AbilitySystem that knows the object, the ability bindings act on it directly, as the original executes
// them immediately (ABILITIES_HARDPOINTS.md §2.9 / §5; no command queue, no script-state copy):
//   Has_Ability / Is_Ability_Active / Is_Ability_Ready / Is_Ability_Autofire -> has_ability / is_active /
//   is_ready / is_autofire; Activate_Ability by argument family (§2.9 table: toggle / optional target ->
//   set_unit_ability_mode, object target -> targeted_ability (command 40), position -> targeted_ability on
//   the host's Dummy_Barrage_Target (command 57, spawn_ability_target)); Cancel_Ability -> mode off;
//   Set_Single / Set_All_Abilities_Autofire -> set_autofire; Force_Ability_Recharge / Reset_Ability_Counter
//   -> force_recharge / reset_counter; Get_Hull / Get_Shield -> CombatSystem script_hull / shield_fraction;
//   Are_Engines_Online / Is_Under_Effects_Of_Ability -> combat + ability state. Objects the systems do not
//   know (and builds without engine/tactical/abilities) keep the stage-2 behaviour (type data, script state,
//   SetUnitAbilityMode / SetAbilityAutofire events on the direct path).
// With an AnimWorld that has the object's row: Play_Animation(name, loop[, index]) -> find_anim_type +
// AnimWorld::play (blend 0.33 s, speed 1: ANIM_PARTICLES §A.4); Get_Bone_Position(bone) ->
// AnimWorld::bone_world_position with object_to_world (default [port]: position, facing yaw, Scale_Factor).
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/lua/script_remaining.hpp"  // L1 final: RemainingScriptHooks, RemainingObjectState
#include "alamo/objects/object_store.hpp"

namespace alamo {
class StateHasher;
}
namespace alamo::commands {
class CommandPipeline;
}
namespace alamo::tactical::abilities {
class AbilitySystem;
}
namespace alamo::anim {
class AnimWorld;
struct Mat34;
}  // namespace alamo::anim

namespace alamo::lua {

class LuaVm;
struct TacticalScriptHooks;  // script_gaps.hpp (L1/S4 wave 2b)

// PortExtension subtype base for Lua object orders: subtype = kLuaOrderSubtype + LuaOrder.
inline constexpr uint16_t kLuaOrderSubtype = 0x4C00;
// Payload (24 bytes, little-endian): u32 object id | u32 target id (0x3FFFFF none) | f32 x | f32 y |
// f32 z | u32 arg. Floats are stored as their bit patterns.
enum class LuaOrder : uint8_t {
    MoveTo = 1,          // to the position x,y,z (Move_To with a position); arg = threat threshold T float bits (0 = off)
    AttackMove = 2,      // to target / position, engaging on the way
    Guard = 3,           // Guard_Target: target or position
    Stop = 4,            // Stop
    HyperspaceAway = 5,  // Hyperspace_Away (arg 1 = the optional boolean)
    CancelHyperspace = 6,  // Cancel_Hyperspace (L1/S4 wave 2a; the locomotor / reinforcement host handles it)
};
inline constexpr size_t kLuaOrderPayloadBytes = 24;

// ---- sp3 (AI lane, docs/SIM_PARALLEL_DESIGN.md 5.1 / 11.6): the ability / combat state the bindings read off the
// critical lane, and the writes they queue there ----
// One ability type of a captured object (AbilitySystem answers at the capture frame): has_ability, is_ready,
// can_activate(t, true, true), is_active, is_autofire, and the dry runs the lane's same-call answers predict from
// (set_unit_ability_mode on / off gates 1-3, targeted_ability's per-user checks).
struct LaneAbility {
    uint8_t type = 0;
    uint8_t has = 0, ready = 0, can = 0, active = 0, autofire = 0, mode_on = 0, mode_off = 0, targeted = 0;
};
// One object of the battle's ability or combat system as captured at the AI slot.
struct LaneObject {
    objects::ObjectId id = objects::kNoObjectId;
    objects::ObjectId container = objects::kNoObjectId;  // AbilitySystem::container_of
    uint8_t abilities = 0;  // the ability system knows it (the bindings' ability_system() answers)
    uint8_t combat = 0;     // the combat system has the unit (Get_Hull / Get_Shield / Set_Targeting_Priorities)
    uint8_t tractored = 0, ion_stunned = 0;
    uint8_t engine_hardpoints = 0, engines_live = 0;  // any ENGINE hard point; any of them functioning
    int32_t immobilize = 0;
    float hull = 0.0f, shield = 0.0f;  // CombatSystem::script_hull / shield_fraction
    uint32_t first = 0, count = 0;    // its LaneAbility entries (LaneAbilityView::types, ascending type)
};
struct LaneAbilityView {
    uint32_t frame = 0;                 // the ability frame of the capture
    std::vector<LaneObject> objects;    // ascending id
    std::vector<LaneAbility> types;
    const LaneObject* find(objects::ObjectId id) const {
        const auto it = std::lower_bound(objects.begin(), objects.end(), id, [](const LaneObject& o, objects::ObjectId v) { return o.id < v; });
        return it != objects.end() && it->id == id ? &*it : nullptr;
    }
    const LaneAbility* ability(const LaneObject& o, uint8_t type) const {
        for (uint32_t i = o.first; i < o.first + o.count; ++i)
            if (types[i].type == type) return &types[i];
        return nullptr;
    }
};
// A queued write: applied by the host at the start of the due tick, in its batch order, through the same calls the
// binding makes on the critical lane (ObjectBridge::lane_write). PortExtension subtype kLaneWriteSubtype, payload
// encode_lane_write.
enum class LaneWriteOp : uint8_t {
    SetAutofire = 1,      // AbilitySystem::set_autofire(ids, type, on)
    AbilityMode = 2,      // set_unit_ability_mode(ids, type, on, frame, target)
    Targeted = 3,         // targeted_ability(ids, type, target, -1, frame)
    DummyAbility = 4,     // spawn_ability_target(p, owner), then on the dummy: mode on (jet = on) or targeted
    ForceRecharge = 5,    // force_recharge(ids[0], type, seconds)
    ResetCounter = 6,     // reset_counter(ids[0], type)
    UnlockMovement = 7,   // lock_movement(ids[0], false, frame)
    PriorityOverride = 8, // CombatSystem::set_priority_override(ids[0], value)
    Stickiness = 9,       // CombatSystem::set_stickiness_override(id, seconds) for each combat unit of ids
    OrderReplaced = 10,   // ObjectBridge::raise_order_replaced(id) for each of ids
    HostFirst = 64,       // 64..255: the host's own hook writes (apply_lane_write leaves them to the host)
};
inline constexpr uint16_t kLaneWriteSubtype = 0x4157;  // "AW"
struct LaneWrite {
    LaneWriteOp op = LaneWriteOp::SetAutofire;
    uint8_t type = 0;
    uint8_t on = 0;  // AbilityMode / SetAutofire; DummyAbility: 1 = jet pack (mode on)
    int32_t owner = -1;  // the first object's owner (the batch's player); DummyAbility: the dummy's owner
    int32_t value = -1;  // PriorityOverride: the set index; AbilityMode / Targeted / DummyAbility: the predicted count
    float seconds = -1.0f;
    objects::ObjectId target = objects::kNoObjectId;
    Vec3 p{};
    std::vector<objects::ObjectId> ids;
    Vec3 q{};  // a second point (host writes: a facing)
};
std::vector<uint8_t> encode_lane_write(const LaneWrite& w);
bool decode_lane_write(std::span<const uint8_t> bytes, LaneWrite& out);  // false on malformed bytes
class ObjectBridge;
struct ScriptLockFact;  // sp3: below ObjectBridge
// The host's two halves (with ALAMO_LUA_HAVE_ABILITIES; without, an empty view and nothing applied): the capture of every
// object of `a` and of its combat system at `frame` (on the critical lane, at the AI slot), and the apply of a queued
// write at the due tick's `frame` through the calls the binding makes live. `host` = the VMs' bridge (its
// spawn_ability_target and raise_order_replaced). apply returns the count of a write with a predicted answer (compare
// LaneWrite::value), else -1.
void capture_lane_abilities(const tactical::abilities::AbilitySystem& a, uint32_t frame, LaneAbilityView& out);
int32_t apply_lane_write(tactical::abilities::AbilitySystem& a, uint32_t frame, const LaneWrite& w, const ObjectBridge& host);

struct AbilityState {
    bool active = false;
    bool autofire = false;
};

struct ScriptObjectState {
    std::string hint;                 // script-set hint (Create_Cinematic_Transport); map hints come from ObjectBridge::map_hint
    bool hidden = false;              // Hide / Hide_Object
    int8_t selectable = -1;           // Set_Selectable; -1 = default (type has SELECTABLE behaviour)
    bool prevent_ai_usage = false;
    bool prevent_all_fire = false;
    bool prevent_opportunity_fire = false;
    bool invulnerable = false;        // Make_Invulnerable (Take_Damage is kind 0 and passes it, q6 t12)
    bool cannot_be_killed = false;    // Set_Cannot_Be_Killed: Take_Damage cannot reach 0
    bool locomotor_suspended = false;
    bool highlighted = false, highlighted_small = false;
    bool in_limbo = false;
    bool garrison_spawn = false;
    bool check_contested_space = false;
    bool capture_disabled = false;
    bool orders_locked = false;
    bool has_orders = false;          // set by an issued order, cleared by Stop
    float max_speed_override = -1.0f; // Override_Max_Speed; < 0 = none
    float shield = -1.0f;             // shield fraction 0..1; < 0 = full (1 if the type has shields, else 0)
    float highlight_offset = 0.0f;
    int32_t importance = 0;
    objects::ObjectId attack_target = objects::kNoObjectId;
    std::string targeting_priorities;
    std::string animation;            // Play_Animation (name, loop, index): for the presentation side
    bool animation_loop = false;
    int32_t animation_index = -1;
    std::map<uint8_t, AbilityState> abilities;  // by UnitAbilityType id (OBJECT_TYPES.md §9.1.4)
};

// q6 (OBJECT_SCRIPTS §10.1): the outcome of Prevent_AI_Usage on the owner's AI free store. The warnings log and go on; the
// errors end the script after its pass.
enum class PreventAiResult : uint8_t {
    Done = 0,
    NoAiPlayer,      // warning "No AI Player active on the owner of this object." (a human or AI-less owner: no effect)
    NoAiPresence,    // error: the owner's AI has no presence in this mode
    NotInitialised,  // error: the owner's AI is not initialised yet
    NoFreeStore,     // warning "No freestore exists for this AI Player."
};

// Reinforce_Unit(type, location | false, player [, ignore_rules [, obey_zones]]) as the host sees it.
struct ReinforceRequest {
    uint32_t id = 0;                                  // the request's BlockingStatus id
    objects::TypeIndex type = objects::kNone;
    int32_t player = -1;
    bool has_location = false;                        // false: `false` given, the host picks the entry
    Vec3 position{};
    objects::ObjectId marker = objects::kNoObjectId;  // the location object (entry marker), if any
    bool ignore_rules = false;                        // 4th argument (PGSpawnUnits: ignore_reinforcement_rules)
    bool obey_zones = false;                          // 5th argument (reinforcement zones)
};
struct ReinforceState {
    bool finished = false;
    std::vector<objects::ObjectId> units;  // BlockingStatus.Result(): the new units, creation order
};

class ObjectBridge {
public:
    // ---- host inputs ----
    commands::CommandPipeline* pipeline = nullptr;  // orders; nullptr = not issued
    // Parent-mode store (galactic, while a tactical battle runs): Mark_Parent_Mode_Object_For_Death.
    objects::ObjectStore* parent_store = nullptr;
    // Player relation for Find_Nearest(obj, player, false) ("enemies of player"). PROVISIONAL default
    // until the players family supplies diplomacy: a != b and both >= 0.
    std::function<bool(int32_t a, int32_t b)> is_enemy;
    // Change_Owner: hosts whose systems cache owners apply it (S4 story VMs: GalacticSim::change_owner;
    // tactical hosts: store + TreeSet::set_owner). Without a hook (or without a bridge) the binding calls
    // ObjectStore::set_owner on the VM's store. Returns success.
    std::function<bool(objects::ObjectHandle, int32_t new_owner)> change_owner;
    // Set_Garrison_Spawn(on): called after ScriptObjectState::garrison_spawn is stored. The stored flag
    // defaults to false while the original's garrison spawn defaults to enabled, so hosts take the
    // change from this hook rather than polling the flag. Unset: only the flag is stored.
    std::function<void(objects::ObjectHandle, bool)> garrison_spawn_changed;
    // L1/S4 wave 1 - containment and removal for stores whose membership lives outside the ObjectStore
    // (galactic: GalacticSim keeps fleet / planet membership, ships carry no store container):
    //   container_of: Get_Parent_Object / Get_Planet_Location (climbed) / Is_In_Garrison; null = none.
    //   contents_of:  Get_Contained_Object_Count, Contains_Object_Type, Contains_Hero,
    //                 Get_Contained_Heroes, Get_Garrisoned_Units, Destroy_Contained_Objects (in order).
    //   destroy_object: Despawn / Destroy_Contained_Objects (GalacticSim::destroy_object keeps its data
    //                 packs consistent and emits ObjectDestroyed); returns success.
    // Without hooks the bindings use the store's container links (walk in insertion order) and
    // ObjectStore::destroy.
    std::function<objects::ObjectHandle(objects::ObjectHandle)> container_of;
    std::function<std::vector<objects::ObjectHandle>(objects::ObjectHandle)> contents_of;
    std::function<bool(objects::ObjectHandle)> destroy_object;
    // BlockingStatus.IsFinished for orders (object id, order event serial). Default: finished at once
    // (PROVISIONAL until locomotion / combat report order completion).
    std::function<bool(objects::ObjectId object, uint32_t serial)> order_finished;
    // Event_Object_In_Range candidates: ids of objects of the VM's store within `radius` of `center`
    // (any order, may over-report: the binding re-tests the 3D distance and sorts by id). Hosts with a
    // spatial tree (engine/tactical TreeSet) plug a radius query in here. Without a hook every live
    // object of the store is tested.
    std::function<void(Vec3 center, float radius, std::vector<objects::ObjectId>& out)> objects_in_range;
    // Reinforce_Unit: true = the host accepted request `r` and calls finish_reinforcement(r.id, units)
    // when the units have arrived; false = refused (the block finishes with no units). Without a hook
    // [inferred stand-in, no transports / reinforcement points yet]: with a location the units are
    // created there at once (a company: container + Company_Units, like Spawn_Unit), without one
    // nothing is created; either way the block is finished immediately.
    std::function<bool(const ReinforceRequest& r)> reinforce;
    // s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns"): the facing of the stand-in's Reinforce_Unit units = `player`'s
    // entry-facing record (the tactical mode's +0x288 table: start / spawn marker facing, +X when unset). Unset: +X.
    // A host with `reinforce` applies the record itself.
    std::function<Vec3(int32_t player)> reinforce_facing;
    // Spawn_From_Reinforcement_Pool(type, position, player): the host takes the unit out of the player's
    // tactical reinforcement pool and spawns it; returns the new top-level objects (empty = none in the
    // pool). Without a hook [inferred stand-in]: created directly like Spawn_Unit. Facing (s78i t02): toward the world
    // origin, as Spawn_Unit, in every mode (TACTICAL_COMBAT §3.1.1 "other spawns"); a host hook applies the same rule.
    std::function<std::vector<objects::ObjectHandle>(objects::TypeIndex type, int32_t player, Vec3 position)>
        spawn_from_pool;
    // q6 object scripts (OBJECT_SCRIPTS.md §6.1 step 3): Spawn_Unit / Create_Generic_Object-style direct creation
    // (bind_objects.cpp spawn(): the unit and each Company_Units member) through the host, which links the object into
    // its systems (a tactical battle: trees, and combat / movement when the type is in the battle tables). Returns the
    // new object, or a null handle to fall back to ObjectStore::create. Unset (default): ObjectStore::create.
    std::function<objects::ObjectHandle(const objects::SpawnParams& p)> spawn_unit;
    // q6 t12: Spawn_Unit of a company type (one with Company_Units) when the host lays out companies itself (a tactical
    // battle: its members as battle units plus their TEAM containers, TacticalBattle::script_spawn_company). Returns the
    // object Spawn_Unit hands to Lua; null = not handled (the binding creates the company and its members itself, each
    // through spawn_unit). Unset (default): the binding's own layout.
    std::function<objects::ObjectHandle(const objects::SpawnParams& p)> spawn_company;
    // q6 t12: Take_Damage(amount) without a hardpoint name through the host's damage system (a tactical battle: the
    // combat Take_Damage with kind 0, TacticalBattle::script_damage); `cannot_die` = the object's Set_Cannot_Be_Killed.
    // True = applied by the host (the death, if any, processed there); false or unset: the store-only health change.
    std::function<bool(objects::ObjectHandle unit, float amount, bool cannot_die)> take_damage;
    // q6 (OBJECT_SCRIPTS §10.2): Make_Invulnerable(on), called after ScriptObjectState::invulnerable is stored: the host's
    // INVULNERABLE behaviour (a tactical battle: the combat gate that drops every damage kind but 0 and 13; a TEAM
    // container passes it to its members). Unset: only the flag is stored.
    std::function<void(objects::ObjectHandle, bool on)> invulnerable_changed;
    // q6 (OBJECT_SCRIPTS §10.1): Prevent_AI_Usage(on) on the owner's AI free store of the current mode (true: removed,
    // false: admitted by the normal rules); no object flag. Returns the outcome; unset: only the flag is stored.
    std::function<PreventAiResult(objects::ObjectHandle, bool on)> prevent_ai_usage;
    // q6 (OBJECT_SCRIPTS §10.3): whether the object has a model in this mode (decided from the type's model name, never from
    // loaded art); Hide on an object without one is a script error. Unset: every object has one.
    std::function<bool(objects::ObjectHandle)> has_model;
    // q6 (OBJECT_SCRIPTS §10.4 step 2): Spawn_Unit's facing for a point in this mode (a tactical battle: toward the map
    // origin). Unset: the location object's facing, else +X.
    std::function<Vec3(Vec3 position)> spawn_facing;
    // refixf t03: the representative type 0x3973b0 (docs/re/LUA_API.md "Find_All_Objects_Of_Type and the sibling
    // finders"; A1_READINGS §29) that Find_All_Objects_Of_Type and Find_Nearest match on: a team container -> the
    // company's Create_Team_Type, else the company type; a transport -> its company type; a squadron fleet -> its
    // squadron type; else the object's own type. Unset, or kNone returned: the object's store type, which is the
    // representative type for every galactic object of the port model (GalacticSim::representative_type) and for
    // tactical objects other than team containers without a Create_Team_Type (store type "Team") and transports.
    std::function<objects::TypeIndex(objects::ObjectHandle)> representative_type;
    // q6 t28 (TERRAIN_WATER_POST 1.6): the object's map hint name (.ted chunk 0x482, loaded with the map object). Find_Hint,
    // Get_Hint and Find_All_Objects_With_Hint read ScriptObjectState::hint when a script set one (Create_Cinematic_Transport),
    // else this. Unset or "": no map hint.
    std::function<std::string_view(objects::ObjectHandle)> map_hint;

    // ---- L1/S4 wave 2a: tactical systems (see the header comment) ----
    // S9 ability system of the battle (object ids = store ids). Used only in builds where engine/lua links
    // engine/tactical/abilities (ALAMO_LUA_HAVE_ABILITIES); otherwise ignored.
    tactical::abilities::AbilitySystem* abilities = nullptr;
    // Frame passed to the AbilitySystem (its combat tick frame). Unset: the VM frame (LuaVm::set_frame).
    std::function<uint32_t()> ability_frame;
    // Command 57: a position-targeted ability (Activate_Ability(name, position)) targets a Dummy_Barrage_Target
    // the host spawns at the point (an object id is consumed). Returns its id, or kNoObjectId to refuse. Unset:
    // the SetUnitAbilityMode event with the position goes to the pipeline instead [port].
    std::function<objects::ObjectId(Vec3 position, int32_t owner)> spawn_ability_target;
    // sp3 (AI lane): set by a host whose VMs run off the critical lane (BattleAi with TacticalAiConfig::ai_lane). The
    // ability / combat bindings then read the object's record in `lane_abilities` (captured at the AI slot; an object
    // missing from it is unknown to both systems) instead of `abilities`, and hand their writes to `lane_write`, which
    // queues them for the due tick; a write's same-call answer is predicted from the record's dry runs. Null: live.
    const LaneAbilityView* lane_abilities = nullptr;
    std::function<void(const LaneWrite&)> lane_write;
    // R3 animation world of the mode (rows by store id). Used only with ALAMO_LUA_HAVE_ANIM.
    anim::AnimWorld* anim = nullptr;
    // Global frame for AnimWorld::play. Unset: the VM frame.
    std::function<uint64_t()> anim_frame;
    // Object -> world transform for Get_Bone_Position (ANIM_PARTICLES §A: "position, facing, Scale_Factor; the
    // caller's"). Unset [port]: translation = store position, rotation = yaw of the store facing (XY, x forward,
    // z up), uniform scale = the type's Scale_Factor (0 -> 1).
    std::function<bool(objects::ObjectHandle, anim::Mat34& out)> object_to_world;
    // Generic bone read (hosts without an AnimWorld, tests). Checked first. True = `out` written.
    std::function<bool(objects::ObjectHandle, std::string_view bone, Vec3& out)> bone_position;

    // ---- L1/S4 wave 2b (script_gaps.hpp): special weapons, TSW, engines, cinematic transport / retreat ----
    // Null: those bindings answer as a mode without the systems. hash() folds in the cinematic transports'
    // machines when the hooks have some (nothing is added while the list is empty: existing CRCs unchanged).
    TacticalScriptHooks* tactical = nullptr;

    // ---- L1 final (script_remaining.hpp): space fields, diversion, builds, combat / galactic queries ----
    // Null: those bindings answer "not available" or use their documented data fallback. Their per-object state
    // (remaining_state) is hashed after everything else, only when non-empty (existing CRCs unchanged).
    RemainingScriptHooks* remaining = nullptr;
    RemainingObjectState& remaining_state(objects::ObjectId id) { return remaining_states_[id]; }
    const RemainingObjectState* find_remaining_state(objects::ObjectId id) const {
        const auto it = remaining_states_.find(id);
        return it == remaining_states_.end() ? nullptr : &it->second;
    }

    // ---- reinforcement requests (ids from 1, never reused) ----
    uint32_t new_reinforcement() {
        const uint32_t id = next_reinforcement_++;
        reinforcements_[id];
        return id;
    }
    void finish_reinforcement(uint32_t id, std::vector<objects::ObjectId> units) {
        ReinforceState& s = reinforcements_[id];
        s.finished = true;
        s.units = std::move(units);
    }
    const ReinforceState* reinforcement(uint32_t id) const {
        const auto it = reinforcements_.find(id);
        return it == reinforcements_.end() ? nullptr : &it->second;
    }

    // ---- S7 t15 A1: "unit ability finished" (object signal 0x29, ABILITIES_HARDPOINTS §2.6 / MOVEMENT §3.1.1 step 6) ----
    // A blocking GameObjectWrapper.Activate_Ability (AbilityBlockStatus, count 1) waits for the unit's next signal 0x29 (or
    // its death / removal). The bridge counts the signals of the units such a block watches: watch_ability(id) (at block
    // creation) returns the unit's count, which the block keeps; the host calls ability_finished(id) for every signal, in
    // its order (ids no block watches are ignored, so a battle without blocking Activate_Ability keeps its CRCs). The
    // block is finished once ability_signals(id) moved past its value.
    // S7 t26 (AI_TACTICAL §7.3.3 item 2): the block listens to signals 0x29, 0xb and 0x10 alike (the first ends it), so the
    // count is of all three: ability_finished (0x29) and order_replaced (0xb: a new order replaces the unit's order);
    // 0x10 (death / removal) the block reads from the store.
    uint32_t watch_ability(objects::ObjectId id) { return ability_signals_[id]; }
    void ability_finished(objects::ObjectId id) {
        if (const auto it = ability_signals_.find(id); it != ability_signals_.end()) ++it->second;
    }
    void order_replaced(objects::ObjectId id) { ability_finished(id); }
    // S7 t31 (§7.3.3 item 1): Activate_Ability raises 0xb on the unit (and each TEAM member) before executing the ids of
    // the list. Set: the host's raiser (the battle's listeners: every script world and the story bridge, each adds the
    // TEAM container); unset: this bridge's order_replaced for the unit and its container.
    std::function<void(objects::ObjectId)> raise_order_replaced;
    uint32_t ability_signals(objects::ObjectId id) const {
        const auto it = ability_signals_.find(id);
        return it == ability_signals_.end() ? 0u : it->second;
    }

    // ---- per-object script state ----
    ScriptObjectState& state(objects::ObjectId id) { return states_[id]; }
    const ScriptObjectState* find_state(objects::ObjectId id) const {
        const auto it = states_.find(id);
        return it == states_.end() ? nullptr : &it->second;
    }
    void set_hint(objects::ObjectId id, std::string hint) { states_[id].hint = std::move(hint); }
    // Drops the state of ids no longer registered in `store` (call after its flush).
    void prune(const objects::ObjectStore& store) {
        for (auto it = states_.begin(); it != states_.end();)
            it = store.find(it->first) ? std::next(it) : states_.erase(it);
        for (auto it = remaining_states_.begin(); it != remaining_states_.end();)
            it = store.find(it->first) ? std::next(it) : remaining_states_.erase(it);
        for (auto it = ability_signals_.begin(); it != ability_signals_.end();)
            it = store.find(it->first) ? std::next(it) : ability_signals_.erase(it);
    }
    size_t state_count() const { return states_.size(); }
    // sp3: every object with a script lock or a Divert (ascending id; struct ScriptLockFact below).
    std::vector<ScriptLockFact> script_lock_facts() const;

    // ---- diagnostics ----
    uint32_t orders_issued = 0;  // events built by the bindings (also counted without a pipeline)

    // Ordered digest of the script state (per-object states in id order, reinforcement requests in id
    // order, order counter). Not registered anywhere by itself: the host folds it into its CRC system.
    void hash(StateHasher& h) const;

    // N1 save / load (contract: alamo/core/save_file.hpp; S4 session chunk "story.bridge"): u32 version, orders_issued,
    // the per-object script states and the remaining family's per-object states (ascending id), the next
    // reinforcement id and the requests (ascending id), then (version 2) the watched units' ability-signal counts. Not saved: the host hooks and pointers above (the reader's own)
    // and the tactical hooks' CinematicTransports (saved by their owner, CinematicTransports::write). read() replaces
    // the saved fields (hash() then equals the writer's when the host restored the transports too); false and
    // unchanged on bad data. Reads its own fields only (the owner checks done() of its chunk).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    std::map<objects::ObjectId, ScriptObjectState> states_;
    std::map<objects::ObjectId, RemainingObjectState> remaining_states_;  // L1 final
    std::map<uint32_t, ReinforceState> reinforcements_;
    uint32_t next_reinforcement_ = 1;
    std::map<objects::ObjectId, uint32_t> ability_signals_;  // S7 t15 A1: watched units' signal 0x29 counts
};

// sp3 (AI lane): script_movement_locked's inputs for one object - Lock_Current_Orders, and a Divert with its goal. A host
// whose VMs run off the critical lane publishes these (ObjectBridge::script_lock_facts, ascending id) with the AI's batch
// and answers its movement lock from the copy.
struct ScriptLockFact {
    objects::ObjectId id = objects::kNoObjectId;
    uint8_t locked = 0, diverted = 0;
    Vec3 goal{};
};
inline bool script_movement_locked(std::span<const ScriptLockFact> facts, objects::ObjectId id,
                                   const std::function<bool(objects::ObjectId, Vec3&)>& goal) {
    const auto it = std::lower_bound(facts.begin(), facts.end(), id, [](const ScriptLockFact& f, objects::ObjectId v) { return f.id < v; });
    if (it == facts.end() || it->id != id) return false;
    if (it->locked) return true;
    if (!it->diverted || !goal) return false;
    Vec3 g{};
    return goal(id, g) && g.x == it->goal.x && g.y == it->goal.y;
}

// OI follow-up t03 (A1_READINGS §28.9): this bridge's shares of the obj +0x3a8 movement lock (0x39ad40) - a script
// Lock_Current_Orders, or the locomotor's forced-move term while the unit's current move is the one a Divert started
// (`goal` reports the unit's current move goal in XY, false when it has none). The abilities hold the other share.
inline bool script_movement_locked(const ObjectBridge& b, objects::ObjectId id,
                                   const std::function<bool(objects::ObjectId, Vec3&)>& goal) {
    if (const ScriptObjectState* s = b.find_state(id); s && s->orders_locked) return true;
    const RemainingObjectState* r = b.find_remaining_state(id);
    if (!r || !r->diverted || !goal) return false;
    Vec3 g{};
    return goal(id, g) && g.x == r->divert_goal.x && g.y == r->divert_goal.y;
}

// Attaches (nullptr: detaches) the bridge to a VM. The bridge must outlive the attachment.
void attach_object_bridge(LuaVm& vm, ObjectBridge* bridge);
ObjectBridge* attached_object_bridge(LuaVm& vm);  // nullptr when none is attached

// UnitAbilityType names (OBJECT_TYPES.md §9.1.4 = engine/tactical/abilities AbilityType). With
// ALAMO_LUA_HAVE_ABILITIES the lookup is S9's ability_type_from_name (one table for the engine); the local
// table below is the fallback for builds without it, and the tests check both tables agree.
uint8_t unit_ability_id(std::string_view name);  // case-insensitive, 0 if unknown
std::string_view unit_ability_name(uint8_t id);  // local table ("" for 0 / out of range)

}  // namespace alamo::lua

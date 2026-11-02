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
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
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

struct AbilityState {
    bool active = false;
    bool autofire = false;
};

struct ScriptObjectState {
    std::string hint;                 // map-editor hint (Find_Hint / Get_Hint); set by the map loader
    bool hidden = false;              // Hide / Hide_Object
    int8_t selectable = -1;           // Set_Selectable; -1 = default (type has SELECTABLE behaviour)
    bool prevent_ai_usage = false;
    bool prevent_all_fire = false;
    bool prevent_opportunity_fire = false;
    bool invulnerable = false;        // Make_Invulnerable: Take_Damage does nothing
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
    // Spawn_From_Reinforcement_Pool(type, position, player): the host takes the unit out of the player's
    // tactical reinforcement pool and spawns it; returns the new top-level objects (empty = none in the
    // pool). Without a hook [inferred stand-in]: created directly like Spawn_Unit.
    std::function<std::vector<objects::ObjectHandle>(objects::TypeIndex type, int32_t player, Vec3 position)>
        spawn_from_pool;
    // refixf t03: the representative type 0x3973b0 (docs/re/LUA_API.md "Find_All_Objects_Of_Type and the sibling
    // finders"; A1_READINGS §29) that Find_All_Objects_Of_Type and Find_Nearest match on: a team container -> the
    // company's Create_Team_Type, else the company type; a transport -> its company type; a squadron fleet -> its
    // squadron type; else the object's own type. Unset, or kNone returned: the object's store type, which is the
    // representative type for every galactic object of the port model (GalacticSim::representative_type) and for
    // tactical objects other than team containers without a Create_Team_Type (store type "Team") and transports.
    std::function<objects::TypeIndex(objects::ObjectHandle)> representative_type;

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
    uint32_t watch_ability(objects::ObjectId id) { return ability_signals_[id]; }
    void ability_finished(objects::ObjectId id) {
        if (const auto it = ability_signals_.find(id); it != ability_signals_.end()) ++it->second;
    }
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

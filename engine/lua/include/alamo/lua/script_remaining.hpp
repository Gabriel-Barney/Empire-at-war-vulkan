// L1 final (roadmap L1: the last 13 globals / 50 methods of logs/lua_binding_coverage.txt): host contract of the
// "remaining" binding family (src/bind_script_remaining.cpp). Specs: docs/re/LUA_API.md (error strings),
// L1_S4_REMAINING.md (RE, in progress), AI_TACTICAL.md, AI_GALACTIC.md, TACTICAL_COMBAT.md, OBJECT_TYPES.md.
//
// Bound by the family (vm_internal.hpp impl_script_remaining):
//   globals  Are_On_Opposite_Sides_Of_Shield, Find_Best_Local_Threat_Center, Find_Nearest_Space_Field, Force_Weather,
//            Is_Point_In_Asteroid_Field / _Ion_Storm / _Nebula, _ProduceObject; the singleton objects Budget,
//            DiscreteDistribution, WeightedTypeList, SFXManager and GlobalValue are callable no-ops [port: LuaUserVar's
//            default call returns nothing] (GlobalValue also gets Get / Set, see below; plan hosts replace Budget per
//            plan with ai_bridge.hpp set_budget_global).
//   GameObjectWrapper  Build, Can_Garrison_Fire, Divert, Is_On_Diversion, Enable_Behavior, Enable_Dynamic_LOD,
//            Explore_Area, Force_Test_Space_Conflict, Get_AI_Power_Vs_Unit, Get_Affiliated_Indigenous_Type,
//            Get_All_Projectile_Types, Get_Current_Projectile_Type, Get_Build_Pad_Contents, Lock_Build_Pad_Contents,
//            Get_Energy, Get_Final_Blow_Player, Get_Rate_Of_Damage_Taken, Get_Time_Till_Dead, Is_In_Asteroid_Field,
//            Is_In_Ion_Storm, Is_In_Nebula, Release, Set_Prefer_Ground_Over_Space,
//            Set_Targeting_Stickiness_Time_Threshold
//   GameObjectTypeWrapper  Get_Bribe_Cost, Is_Obsolete
//   PlayerWrapper  Get_GameSpy_Stats_Player_ID, Give_Random_Sliceable_Tech
//   TaskForceClass  Add_Force, Add / Remove / Clear_Opportunity_Fire_Event_Subscription(s), Can_Garrison, Garrison,
//            Leave_Garrison, Get_AI_Power_Vs_Unit, Get_Type_Of_Unit, Set_All_Abilities_Autofire,
//            Set_Single_Ability_Autofire, Set_Targeting_Stickiness_Time_Threshold
//   Land/SpaceTaskForceClass  Find_Closest_Enemy;  LandTaskForceClass  Bombing_Run, Fire_Orbital_Bombardment
//   GalacticTaskForceClass  Refit_To_Definition;  FindTargetClass  Best_Of
//   LuaScriptWrapper  Call_Function, Get_Variable, Set_Variable, Is_Valid (ScriptWrapperHost below)
//   extras (not in the RE table): GlobalValueClass.Get / Set
//
// Host contract. Every hook is optional; without one the binding answers with the "not available" result named next
// to it (or the documented data fallback, marked [port]). The battle / galactic hosts set what their systems answer:
//   * RemainingScriptHooks (this header) on ObjectBridge::remaining - tactical systems (space fields, weather, shields,
//     diversion, builds / build pads, combat queries, superweapon runs) and the galactic final-blow / tech queries;
//   * AiScriptHost / AiTacticalScriptHost virtuals with defaults (ai_bridge.hpp, ai_tactical_bridge.hpp) - task-force
//     membership, Best_Of, Refit_To_Definition, AI power, explore, bombing run / orbital bombardment;
//   * GlobalValueStore (below) - the session-wide GlobalValue table for VMs without an AI host;
//   * ScriptWrapperHost (below) - the VMs a LuaScriptWrapper refers to.
// Error discipline: as script_gaps.hpp ("SCRIPT ERROR: ..." / "SCRIPT WARNING: ..." messages, nil result) where the
// original logs; the argument errors LUA_API.md lists as raising helpers are script errors too
// (Call::script_error_stop, no Lua error: q6 t11, the rule of every family).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/lua/ai_bridge.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}

namespace alamo::lua {

class LuaVm;

// Space field kinds (Find_Nearest_Space_Field's "space field category mask": "Asteroid | Nebula | Ion_Storm").
inline constexpr uint32_t kFieldAsteroid = 1u, kFieldIonStorm = 2u, kFieldNebula = 4u;
// Parses a field mask string ('|' / ',' / space separated, case-insensitive: Asteroid, Asteroid_Field, Ion_Storm,
// IonStorm, Nebula); unknown words are ignored. 0 = nothing recognised.
uint32_t parse_space_field_mask(std::string_view text);

struct RemainingScriptHooks {
    // ---- space fields (tactical-systems agent) ----
    // Unset: the data fallback [port] - objects of the mode (insertion order) whose type sets Is_Asteroid_Field
    // (asteroid; Is_Impassable_Asteroid does not count, L1_S4_REMAINING §1.6), Is_Ion_Storm or Is_Nebula and has
    // SPACE_OBSTACLE: a circle at position + Space_Obstacle_Offset with Space_Obstacle_Radius, XY distance; nearest:
    // strict <, the first wins ties.
    std::function<bool(Vec3 point, uint32_t field_mask)> point_in_field;
    std::function<objects::ObjectHandle(Vec3 point, uint32_t field_mask)> nearest_field;
    // Is_In_Nebula / _Ion_Storm / _Asteroid_Field of one object (§2.7; kind = one kField* bit, the victim-behaviour time
    // stamps): 1 / 0, -1 unknown -> the point test at the object's position. TEAM containers: the binding ORs over
    // the members (rule 1).
    std::function<int32_t(objects::ObjectHandle object, uint32_t kind)> object_in_field;
    // ---- shields / weather ----
    // Are_On_Opposite_Sides_Of_Shield (L1_S4_REMAINING §1.2; land mode only): player = -1 when not given. Unset:
    // false (no base shield system). The 4-argument form's bool ("shield friendly to player") goes to
    // opposite_sides_of_shield4 when set, else it is ignored.
    std::function<bool(Vec3 a, Vec3 b, int32_t player)> opposite_sides_of_shield;
    std::function<bool(Vec3 a, Vec3 b, int32_t player, bool friendly)> opposite_sides_of_shield4;
    // Force_Weather(): one phase advance (§3.2). Unset: SCRIPT WARNING, nothing happens.
    std::function<bool()> force_weather;
    // ---- movement (Divert / Is_On_Diversion, §2.3) ----
    // The binding keeps the diversion flag (move flag 0x40) itself: Divert issues a plain move through move_to and
    // records the goal move_goal reports right after; Is_On_Diversion = that goal is still the unit's current goal.
    // move_to: a position move of one unit (type 1, mask 0x3f) -> accepted. Unset: Divert answers false.
    std::function<bool(objects::ObjectHandle unit, Vec3 position)> move_to;
    // The unit's current (last) move goal: false when it has none (idle / arrived).
    std::function<bool(objects::ObjectHandle unit, Vec3& goal)> move_goal;
    // ---- builds / build pads (§2.2, §3.6) ----
    // GameObjectWrapper.Build(type, check_rules) on a pad: 1 started, 0 refused by the game rules (check_rules only),
    // -1 start failed. Unset: the binding's own start build [port] (rules check, the UC type created at the pad,
    // recorded as the pad's contents; construction is not serviced).
    std::function<int(objects::ObjectHandle pad, objects::TypeIndex type, bool check_rules)> build;
    // The built object, else the under-construction object of a pad. Unset: the binding's record, else the pad's
    // first live attached child / store content [port], else nothing.
    std::function<objects::ObjectHandle(objects::ObjectHandle pad)> build_pad_contents;
    // Lock_Build_Pad_Contents(lock): the host's pad state. The flag is always kept in the family state too.
    std::function<void(objects::ObjectHandle pad, bool lock)> lock_build_pad;
    // ---- garrisons (S4 garrison system) ----
    // Can_Garrison_Fire of a holder. Unset: the type's Property_Flags GarrisonCanFire (OBJECT_TYPES.md 9.1.2) [port].
    std::function<bool(objects::ObjectHandle holder)> can_garrison_fire;
    // ---- combat (tactical combat system) ----
    // Enable_Behavior(behavior id, on): unset -> the family state keeps the override (no system reads it yet).
    std::function<bool(objects::ObjectHandle unit, int32_t behavior, bool on)> enable_behavior;
    // Set_Targeting_Stickiness_Time_Threshold (TACTICAL_COMBAT.md 2.5: the Lua override +0x158). Kept in the state too.
    std::function<void(objects::ObjectHandle unit, float seconds)> set_stickiness;
    std::function<void(objects::ObjectHandle unit, bool prefer_ground)> set_prefer_ground;
    // Every projectile type the unit can fire (weapon and hard points, the combat system's order). Unset: the data
    // fallback [port] = the type's Projectile_Types (hard point data is not in GameObjectTypes).
    std::function<std::vector<objects::TypeIndex>(objects::ObjectHandle unit)> projectile_types;
    // Current projectile type (kNone = unknown -> the first of the list above).
    std::function<objects::TypeIndex(objects::ObjectHandle unit)> current_projectile;
    // Get_Energy (§2.12: tactical only, a fraction 0..1). energy_fraction first; else energy (absolute pool) divided
    // by Energy_Capacity; < 0 = no pool / unknown. Unset: 1 for a type with Energy_Capacity > 0, else 0 [port].
    std::function<float(objects::ObjectHandle unit)> energy_fraction;
    std::function<float(objects::ObjectHandle unit)> energy;
    // Damage tracking (§2.4): rate (damage / s) and the time until death (s); < 0 = unknown. Unset: 0 / 1e18.
    std::function<double(objects::ObjectHandle unit)> rate_of_damage;
    std::function<double(objects::ObjectHandle unit)> time_till_dead;
    // The final-blow player id (obj +0x330, §2.4); -1 = none -> the neutral player. Unset: -1.
    std::function<int32_t(objects::ObjectHandle object)> final_blow_player;
    // The AI power-versus weight of `unit`'s type against `other`'s categories (§2.12 M_s; the WeightedTypeList table);
    // < 0 unknown -> 1 [port].
    std::function<double(objects::ObjectHandle unit, objects::ObjectHandle other)> ai_power_vs;
    // ---- galactic ----
    // Force_Test_Space_Conflict on a fleet (§2.12: re-run the arrival; conflict -> Initiate_Space_Conflict). True = a
    // conflict was started. Unset: the GalacticSim conflict hooks when the players family has a sim, else false.
    std::function<bool(objects::ObjectHandle fleet)> test_space_conflict;
    // Give_Random_Sliceable_Tech(player) -> the chosen type (kNone = none). Unset: §2.9 on the players world.
    std::function<objects::TypeIndex(int32_t player)> give_random_sliceable_tech;
    // GameObjectTypeWrapper.Is_Obsolete(player). Unset: the player's obsolete list (players.hpp LuaPlayerState).
    std::function<bool(objects::TypeIndex type, int32_t player)> type_obsolete;
    // Get_Bribe_Cost (§2.8): the battle planet of the parent galactic mode: -1 none (the base cost), 0 uncorrupted,
    // 1 corrupted. Unset: -1.
    std::function<int(void)> battle_planet_corruption;
    // Orbital bombardment / bombing run of a land task force's player at a position (no AI host answer). Unset: false.
    std::function<bool(int32_t player, Vec3 position)> orbital_bombardment;
    std::function<bool(int32_t player, Vec3 position)> bombing_run;
    // The land strike managers (§3.7 / §3.8): availability (Bombing_Run's warning choice; the AI perception tokens
    // IsBombingRunAvailable / IsOrbitalBombardmentAvailable), the BombingBlockStatus of a started run (IsFinished = no
    // bomber left, Result = undamaged / total) and the PlayerWrapper switches (Disable_Bombing_Run,
    // Disable_Orbital_Bombardment, Remove_Orbital_Bombardment; the players world keeps its flags as well).
    std::function<bool(int32_t player)> bombing_run_available;
    std::function<bool(int32_t player)> bombardment_available;
    std::function<bool(int32_t player)> bombing_run_finished;
    std::function<float(int32_t player)> bombing_run_result;
    std::function<void(int32_t player, bool disabled)> set_bombing_run_disabled;
    std::function<void(int32_t player, bool disabled)> set_bombardment_disabled;
    std::function<void(int32_t player, bool removed)> remove_bombardment;
};
// BlockingStatus kind of a started bombing run (Ud::reserved; Ud::id = the player; vm_internal.hpp kinds 0..4 taken).
inline constexpr int32_t kBlockBombingRun = 5;

// Per-object script state of the family (ObjectBridge::remaining_state, ordered by id).
struct RemainingObjectState {
    bool dynamic_lod = true;              // Enable_Dynamic_LOD (model +0x198, default on: RENDER.md)
    bool build_pad_locked = false;        // Lock_Build_Pad_Contents
    int8_t prefer_ground = -1;            // Set_Prefer_Ground_Over_Space; -1 = never set
    float stickiness = -1.0f;             // Set_Targeting_Stickiness_Time_Threshold; < 0 = the type's value
    std::map<int32_t, bool> behaviors;    // Enable_Behavior overrides (behaviour id -> enabled)
    bool diverted = false;                // Divert: move flag 0x40 on the current move (§2.3)
    Vec3 divert_goal{};                   // the goal reported right after the diversion move
    objects::ObjectId pad_uc = objects::kNoObjectId;     // Build without a host: the pad's under-construction object
};

// ---- GlobalValue (§1.5): one process-wide string -> value map, saved with the game ----
class GlobalValueStore {
public:
    AiGlobalValue get(std::string_view name) const;  // Nil when unknown
    void set(std::string_view name, const AiGlobalValue& v);  // Nil erases
    size_t size() const { return values_.size(); }
    void clear() { values_.clear(); }                 // game teardown / new game
    void hash(StateHasher& h) const;                  // name order
    void write(ByteWriter& w) const;                  // save (name order)
    bool read(ByteReader& r);                         // load (replaces the contents)

private:
    std::map<std::string, AiGlobalValue, std::less<>> values_;
};
// Hash layout of one value: kind (u8), number (f64), text (str); a Table adds its entry count and each key / value.
void hash_global_value(StateHasher& h, const AiGlobalValue& v);
// The process-wide store (0x140b0a060): used by every VM that has neither an attached store nor an AI host.
GlobalValueStore& process_global_values();
// GlobalValue.Get / Set resolve: the attached store, else the attached AI host (ai_bridge.hpp; [port] the AI worlds
// keep their own map so their CRC sections stay theirs), else process_global_values(). nullptr detaches.
void attach_global_values(LuaVm& vm, GlobalValueStore* store);

// ---- LuaScriptWrapper: a handle on another script VM ----
class ScriptWrapperHost {
public:
    virtual ~ScriptWrapperHost() = default;
    // The live VM of script `id`, nullptr once it is gone (Is_Valid false). Every VM returned must be owned by the
    // calling VM's thread (the sim thread).
    virtual LuaVm* script_vm(int32_t id) = 0;
    // OI lua (STRATEGIC_GAPS §5.2): the id of the script running in `vm`, -1 when unknown. A script object (`Script`)
    // copied out of `vm` into another VM (LuaScriptWrapper Set_Variable / Call_Function arguments / results) becomes a
    // LuaScriptWrapper of this id there (nil for -1).
    virtual int32_t script_id(const LuaVm& /*vm*/) { return -1; }
};
void attach_script_wrapper_host(LuaVm& vm, ScriptWrapperHost* host);  // nullptr detaches
// Pushes a LuaScriptWrapper for script `id` into `vm` and stores it in the global `name`.
void set_script_wrapper_global(LuaVm& vm, std::string_view name, int32_t id);

}  // namespace alamo::lua
